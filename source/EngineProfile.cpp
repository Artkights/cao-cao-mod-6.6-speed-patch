#include "Patch.h"
#include "EngineProfile.h"
#include "EngineLayout.h"
#include "PeImage.h"
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {
bool Range(size_t offset, size_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

// Version strings in this engine are stale (often "1,0,0,1"). Its actual
// generation is LOWORD(dwFileVersionLS), consistently 5 or 6 in supported bases.
bool ResourceVersion(const BYTE* image, size_t size, const IMAGE_NT_HEADERS32& nt,
    unsigned& generation, std::wstring& error, VS_FIXEDFILEINFO* fixed = nullptr) {
    generation = 0;
    if (fixed) *fixed = VS_FIXEDFILEINFO();
    if (nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE) return true;
    const auto& resource = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    if (!resource.Size) return true;
    if (!Range(resource.VirtualAddress, resource.Size, size)) {
        error = L"主程序资源段越界，不能识别引擎版本。";
        return false;
    }
    // Walk only RT_VERSION, including its language variants. Arbitrary bitmap
    // or icon data must never be interpreted as an engine version record.
    auto walk = [&](auto&& self, size_t relative, unsigned depth) -> bool {
        if (depth > 3 || !Range(relative, sizeof(IMAGE_RESOURCE_DIRECTORY), resource.Size)) return false;
        IMAGE_RESOURCE_DIRECTORY directory = {};
        memcpy(&directory, image + resource.VirtualAddress + relative, sizeof(directory));
        const size_t count = static_cast<size_t>(directory.NumberOfNamedEntries) + directory.NumberOfIdEntries;
        const size_t entries = relative + sizeof(directory);
        if (count > 4096 || !Range(entries, count * sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY), resource.Size)) return false;
        for (size_t index = 0; index < count; ++index) {
            IMAGE_RESOURCE_DIRECTORY_ENTRY entry = {};
            memcpy(&entry, image + resource.VirtualAddress + entries + index * sizeof(entry), sizeof(entry));
            if (depth == 0 && (entry.NameIsString || entry.Id != 16)) continue;
            if (entry.DataIsDirectory) {
                if (!self(self, entry.OffsetToDirectory, depth + 1)) return false;
                continue;
            }
            if (depth < 2 || !Range(entry.OffsetToData, sizeof(IMAGE_RESOURCE_DATA_ENTRY), resource.Size)) return false;
            IMAGE_RESOURCE_DATA_ENTRY data = {};
            memcpy(&data, image + resource.VirtualAddress + entry.OffsetToData, sizeof(data));
            if (!Range(data.OffsetToData, data.Size, size) || data.Size < 6 || data.Size > 1024 * 1024) return false;
            const BYTE* blob = image + data.OffsetToData;
            WORD length = 0, valueLength = 0, type = 0;
            memcpy(&length, blob, 2);
            memcpy(&valueLength, blob + 2, 2);
            memcpy(&type, blob + 4, 2);
            const wchar_t key[] = L"VS_VERSION_INFO";
            const size_t valueOffset = (6 + sizeof(key) + 3) & ~size_t(3);
            if (length > data.Size || type != 0 || valueLength < sizeof(VS_FIXEDFILEINFO) ||
                !Range(6, sizeof(key), length) || memcmp(blob + 6, key, sizeof(key)) ||
                !Range(valueOffset, sizeof(VS_FIXEDFILEINFO), length)) return false;
            VS_FIXEDFILEINFO info = {};
            memcpy(&info, blob + valueOffset, sizeof(info));
            if (info.dwSignature != VS_FFI_SIGNATURE || info.dwStrucVersion != VS_FFI_STRUCVERSION) return false;
            if (fixed) {
                if (fixed->dwSignature && (fixed->dwFileVersionMS != info.dwFileVersionMS ||
                    fixed->dwFileVersionLS != info.dwFileVersionLS)) return false;
                *fixed = info;
            }
            const unsigned value = LOWORD(info.dwFileVersionLS);
            if (!value) continue;
            if (generation && generation != value) return false;
            generation = value;
        }
        return true;
    };
    if (!walk(walk, 0, 0)) {
        error = L"主程序版本资源结构无效或语言版本互相冲突。";
        return false;
    }
    return true;
}

bool KnownProtectedDiskImage(const PeImageMetadata& metadata, const BYTE* mapped,
    size_t mappedSize, uint64_t fileSize, const std::wstring& sha256,
    EngineDetection& detection, std::wstring& error) {
    constexpr wchar_t fingerprint[] = L"1D8E83F73E7780C60B0B5DE5277D54490F72472270F727C97753F8E72AE4B381";
    if (sha256 != fingerprint) return false;
    const auto& nt = metadata.nt;
    if (fileSize != 43263298 || nt.FileHeader.NumberOfSections != 5 ||
        nt.FileHeader.Characteristics != 0x030F || nt.OptionalHeader.ImageBase != 0x400000 ||
        nt.OptionalHeader.SizeOfImage != 0x246000 || nt.OptionalHeader.SizeOfHeaders != 0x400 ||
        nt.OptionalHeader.AddressOfEntryPoint != 0x16F7D0 || nt.OptionalHeader.SectionAlignment != 0x1000 ||
        nt.OptionalHeader.FileAlignment != 0x200 || nt.OptionalHeader.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE || metadata.sections.size() != 5) {
        error = L"已知保护构建的 PE 头与审计记录不一致，未安装补丁。"; return false;
    }
    const DWORD expected[][5] = {
        {0x1000,0x16C198,0x400,0x800,0x40000000},
        {0x16E000,0x59000,0xC00,0x58200,0xE0000000},
        {0x1C7000,0x6C000,0x58E00,0x6A400,0xE0000020},
        {0x233000,0x2000,0xC3200,0x1200,0x40000000},
        {0x235000,0x11000,0xC4400,0x10E00,0x40000000}
    };
    for (size_t index = 0; index < _countof(expected); ++index) {
        const auto& section = metadata.sections[index];
        if (section.VirtualAddress != expected[index][0] || section.Misc.VirtualSize != expected[index][1] ||
            section.PointerToRawData != expected[index][2] || section.SizeOfRawData != expected[index][3] ||
            section.Characteristics != expected[index][4]) {
            error = L"已知保护构建的节结构与审计记录不一致，未安装补丁。"; return false;
        }
    }
    const auto& resource = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    unsigned generation = 0; VS_FIXEDFILEINFO fixed = {};
    if (resource.VirtualAddress != 0x235000 || resource.Size != 0x10C04 ||
        !ResourceVersion(mapped, mappedSize, nt, generation, error, &fixed) || generation != 5 ||
        fixed.dwFileVersionMS != 0x00010000 || fixed.dwFileVersionLS != 0x00060005) {
        error = L"已知保护构建的固定版本资源与 1.0.6.5 审计记录不一致，未安装补丁。"; return false;
    }
    detection.profile = EngineProfileId::Ccz65;
    detection.exeSha256 = sha256;
    detection.requiresRuntimeValidation = true;
    detection.variant = L"Protected220";
    detection.container = L"Protected";
    detection.packedContainer = true;
    detection.planStatus = EnginePlanStatus::VersionSelected;
    detection.layoutStatus = L"RuntimeValidationRequired";
    detection.evidence = L"已匹配加强版 2.2.0 的完整 SHA-256、PE 节结构及固定版本资源；"
        L"仅选择 6.5 载荷，等待启动后验证展开的原生调用布局。";
    error.clear();
    return true;
}
}

namespace {
void ClassifyContainer(const PeImageMetadata& metadata, EngineDetection& detection);
}

bool DetectEngineProfileImage(const unsigned char* image, size_t size, uintptr_t virtualBase,
    EngineDetection& detection, std::wstring& error) {
    detection = EngineDetection(); error.clear();
    IMAGE_DOS_HEADER dos = {};
    if (!image || size < sizeof(dos)) { error = L"主程序 PE 映像截断。"; return false; }
    memcpy(&dos, image, sizeof(dos));
    if (dos.e_lfanew < sizeof(dos) || static_cast<uint64_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > size) {
        error = L"主程序 PE 头截断或偏移无效。"; return false;
    }
    IMAGE_NT_HEADERS32 nt = {};
    memcpy(&nt, image + dos.e_lfanew, sizeof(nt));
    if (nt.OptionalHeader.SizeOfHeaders > size) { error = L"主程序 PE 头超出映像范围。"; return false; }
    PeImageMetadata metadata;
    if (!ParsePeImageMetadata(image, nt.OptionalHeader.SizeOfHeaders, 0, false, metadata, error) ||
        metadata.nt.OptionalHeader.SizeOfImage > size) return false;
    unsigned generation = 0;
    if (!ResourceVersion(image, size, metadata.nt, generation, error)) return false;
    EngineLayout layout;
    if (!ResolveEngineLayout(image, layout, virtualBase, EngineAnalysisSource::Disk) ||
        (layout.profile != EngineProfileId::Ccz64 && layout.profile != EngineProfileId::Ccz65 &&
            layout.profile != EngineProfileId::Ccz66) || layout.plan.status != EnginePlanStatus::AutoVerified) {
        detection.planReport = EnginePatchPlanReport(layout.plan);
        detection.evidence = layout.plan.evidence;
        detection.planStatus = layout.plan.status;
        error = L"无法唯一确认完整加速功能的站点、调用关系及文件偏移，自动安装已停止。";
        return false;
    }
    const unsigned expected = layout.profile == EngineProfileId::Ccz64 ? 4 :
        layout.profile == EngineProfileId::Ccz65 ? 5 : 6;
    if (generation && generation != expected) {
        error = L"主程序版本资源与引擎调用布局冲突，未安装补丁。"; return false;
    }
    detection.profile = layout.profile;
    detection.confidence = layout.plan.confidence;
    detection.planStatus = layout.plan.status;
    detection.layoutStatus = L"AutoVerified";
    detection.planReport = EnginePatchPlanReport(layout.plan);
    ClassifyContainer(metadata, detection);
    detection.evidence = std::wstring(L"已识别 ") + EngineProfileName(layout.profile) +
        L"：已校验等待包装、阶段函数、原生加速及状态变量" +
        (generation ? L"，版本资源一致。" : L"；版本资源缺失，使用独立调用布局确认。");
    return true;
}

namespace {
void ClassifyContainer(const PeImageMetadata& metadata, EngineDetection& detection) {
    bool packed = false, enigma = false;
    for (const auto& section : metadata.sections) {
        const uint32_t logical = PeLogicalSectionSize(section);
        const bool name = memcmp(section.Name, ".enigma", 7) == 0;
        enigma = enigma || name;
        if (section.SizeOfRawData > logical &&
            (section.SizeOfRawData - logical > 1024 * 1024 ||
                (logical && section.SizeOfRawData / logical >= 8))) packed = true;
    }
    detection.packedContainer = packed || enigma;
    detection.container = enigma ? L"Enigma" : packed ? L"Packed" : L"Standard";
}

bool OpenAndMap(const std::wstring& root, PeFileView& file, PeAnalysisView& view,
    std::wstring& hash, std::wstring& error) {
    if (!file.Open(root + L"\\Ekd5.exe", error) || !view.Map(file, error) ||
        !file.HashSha256(hash, error)) return false;
    return true;
}
}

bool DetectEngineProfile(const std::wstring& root, EngineDetection& detection, std::wstring& error) {
    detection = EngineDetection();
    PeFileView file; PeAnalysisView view; std::wstring sha256;
    if (!OpenAndMap(root, file, view, sha256, error)) return false;
    if (DetectEngineProfileImage(view.Data(), view.Size(), view.PreferredBase(), detection, error)) {
        detection.exeSha256 = sha256;
        detection.variant = L"Standard";
        ClassifyContainer(view.Metadata(), detection);
        return true;
    }
    // Preserve the single exact protected-build exception. Its small audited
    // artifact may be read for its section/resource assertions; arbitrary large
    // executables are never copied into an unbounded raw-file vector.
    constexpr uint64_t protectedSize = 43263298;
    constexpr wchar_t protectedHash[] = L"1D8E83F73E7780C60B0B5DE5277D54490F72472270F727C97753F8E72AE4B381";
    if (file.Size() == protectedSize && sha256 == protectedHash &&
        KnownProtectedDiskImage(view.Metadata(), view.Data(), view.Size(), file.Size(),
            sha256, detection, error)) return true;
    return false;
}

bool DetectEngineProfileRuntime(const unsigned char* image, size_t sizeHint,
    EngineDetection& detection, std::wstring& error) {
    detection = EngineDetection(); error.clear();
    if (!image) { error = L"运行时主程序地址为空。"; return false; }
    RuntimeImageView view;
    if (!view.Capture(image, sizeHint, error)) return false;
    const uintptr_t loadedBase = view.RuntimeBase();
    unsigned generation = 0;
    if (!ResourceVersion(view.Data(), view.Size(), view.Metadata().nt, generation, error)) return false;
    EngineLayout layout;
    if (!ResolveEngineLayout(view.Data(), layout, loadedBase, EngineAnalysisSource::Runtime) ||
        layout.plan.status != EnginePlanStatus::AutoVerified) {
        detection.planStatus = layout.plan.status;
        detection.evidence = layout.plan.evidence;
        detection.planReport = EnginePatchPlanReport(layout.plan);
        detection.profile = layout.profile;
        detection.layoutStatus = L"Rejected";
        error = L"运行时调用布局未能唯一验证，未启用补丁功能。"; return false;
    }
    auto check = [&](const EngineSitePlan& site, bool executable) {
        if (!site.required && !site.present) return true;
        if (!site.present || site.rva > view.Size() || site.length > view.Size() - site.rva ||
            (executable && !view.IsExecutable(site.rva, site.length)) ||
            (!executable && !view.IsReadable(site.rva, site.length))) return false;
        BYTE current[sizeof(site.original)] = {};
        return site.length <= sizeof(current) &&
            ValidateRuntimeMemory(reinterpret_cast<const void*>(loadedBase + site.rva), site.length, executable, error) &&
            ReadRuntimeMemory(reinterpret_cast<const void*>(loadedBase + site.rva), current, site.length, error) &&
            (!executable || memcmp(current, site.original, site.length) == 0);
    };
    for (const auto& site : layout.plan.sites)
        if (!check(site, true)) { if (error.empty()) error = L"运行时补丁站点页权限或边界校验失败。"; return false; }
    const EngineSitePlan* code[] = {&layout.plan.nativeRoute};
    for (const auto* site : code) if (!check(*site, site->required)) {
        if (error.empty()) error = L"运行时原生加速入口页权限校验失败。"; return false;
    }
    const EngineSitePlan* data[] = {&layout.plan.nativeState, &layout.plan.nativeBusy,
        &layout.plan.phaseTable, &layout.plan.phaseValue, &layout.plan.speedValue, &layout.plan.timerContext};
    for (const auto* site : data) if (!check(*site, false)) {
        if (error.empty()) error = L"运行时加速状态数据页权限校验失败。"; return false;
    }
    const uintptr_t codeDependencies[] = {layout.waitWrapper, layout.nativeStage,
        layout.nativeSuper, layout.sleep, layout.deadline, layout.pump};
    for (uintptr_t address : codeDependencies) {
        if (!address) continue;
        if (address < 0x400000 || address - 0x400000 >= view.Size() ||
            !view.IsExecutable(address - 0x400000, 3)) {
            error = L"运行时等待/阶段调用链位于不可执行页面。"; return false;
        }
    }
    for (size_t i = 0; i < layout.waitCallerCount; ++i) {
        const uintptr_t context = layout.waitCallers[i] - 21;
        if (context < 0x400000 || !view.IsExecutable(context - 0x400000, 48)) {
            error = L"运行时等待调用者上下文不完整或不可执行。"; return false;
        }
    }
    const unsigned expected = static_cast<unsigned>(layout.profile) - 60;
    if (generation && generation != expected) {
        error = L"运行时版本资源与已验证引擎布局冲突。"; return false;
    }
    layout.plan.runtimeValidated = true;
    detection.profile = layout.profile;
    detection.planStatus = layout.plan.status;
    detection.layoutStatus = L"RuntimeValidated";
    detection.requiresRuntimeValidation = false;
    detection.confidence = layout.plan.confidence;
    detection.planReport = EnginePatchPlanReport(layout.plan);
    ClassifyContainer(view.Metadata(), detection);
    detection.evidence = L"运行时 PE 页面、唯一调用关系及所需站点原始字节均已验证。";
    return true;
}

bool DetectVersionEngineProfile(const std::wstring& root, EngineDetection& detection,
    std::wstring& error, EngineProfileId requestedProfile) {
    detection = EngineDetection();
    error.clear();
    const bool explicitProfile = requestedProfile == EngineProfileId::Ccz64 ||
        requestedProfile == EngineProfileId::Ccz65 || requestedProfile == EngineProfileId::Ccz66;
    if (requestedProfile != EngineProfileId::Unknown && !explicitProfile) {
        error = L"请选择 6.4、6.5 或 6.6 引擎版本。";
        return false;
    }
    PeFileView file;
    PeAnalysisView mappedView;
    std::wstring streamedHash;
    if (!OpenAndMap(root, file, mappedView, streamedHash, error)) return false;
    const BYTE* mapped = mappedView.Data();
    const size_t mappedSize = mappedView.Size();
    const IMAGE_NT_HEADERS32& nt = mappedView.Metadata().nt;
    unsigned generation = 0;
    VS_FIXEDFILEINFO fixed = {};
    std::wstring versionError;
    const bool versionRead = ResourceVersion(mapped, mappedSize, nt,
        generation, versionError, &fixed);
    EngineProfileId recorded = EngineProfileId::Unknown;
    if (versionRead && fixed.dwSignature == VS_FFI_SIGNATURE) {
        // Existing CCZ bases record 1.0.6.5 as MS=0x00010000,
        // LS=0x00060005. The final 5 selects the 6.5 ABI. Also
        // understand the conventional 6.5.0.0 fixed version form.
        const unsigned major = HIWORD(fixed.dwFileVersionMS);
        const unsigned minor = LOWORD(fixed.dwFileVersionMS);
        if (major == 6 && minor >= 4 && minor <= 6)
            recorded = static_cast<EngineProfileId>(60 + minor);
        else if (generation >= 4 && generation <= 6)
            recorded = static_cast<EngineProfileId>(60 + generation);
        else if (generation >= 64 && generation <= 66)
            recorded = static_cast<EngineProfileId>(generation);
    }
    if (recorded != EngineProfileId::Unknown && explicitProfile && requestedProfile != recorded) {
        error = L"所选引擎版本与主程序固定版本资源不一致。";
        return false;
    }
    // A real but unsupported generation must not quietly become 6.5 merely
    // because the caller supplied a default. Only missing/unreadable version
    // information permits an explicit selection by the user.
    if (versionRead && fixed.dwSignature == VS_FFI_SIGNATURE &&
        recorded == EngineProfileId::Unknown && generation) {
        error = L"主程序记录的引擎版本不属于 6.4、6.5、6.6。";
        return false;
    }
    const EngineProfileId selected = recorded != EngineProfileId::Unknown ? recorded :
        explicitProfile ? requestedProfile : EngineProfileId::Unknown;
    if (selected == EngineProfileId::Unknown) {
        error = versionRead ? L"主程序缺少可识别的固定版本资源，请明确选择 6.4、6.5 或 6.6。" :
            L"主程序版本资源无法读取，请明确选择 6.4、6.5 或 6.6。\r\n" + versionError;
        return false;
    }
    detection.exeSha256 = streamedHash;
    if (streamedHash.empty()) {
        error = L"无法计算主程序指纹，不能建立备份记录。";
        return false;
    }
    // Preserve the audited protected build identity when the caller is using
    // the permissive version path.  This keeps the runtime import bridge
    // limited to a known packed image; ordinary version-selected images use
    // the regular runtime hooks only.
    EngineDetection protectedDetection;
    std::wstring protectedError;
    if (KnownProtectedDiskImage(mappedView.Metadata(), mapped, mappedSize, file.Size(),
        detection.exeSha256, protectedDetection, protectedError)) {
        detection = protectedDetection;
        detection.versionFallback = true;
        detection.requiresRuntimeValidation = true;
        detection.variant = L"Protected220";
        return true;
    }
    detection.profile = selected;
    detection.versionFallback = true;
    detection.requiresRuntimeValidation = true;
    detection.variant = L"VersionFallback";
    detection.planStatus = EnginePlanStatus::VersionSelected;
    detection.layoutStatus = L"RuntimeValidationRequired";
    ClassifyContainer(mappedView.Metadata(), detection);
    detection.confidence = 0;
    detection.evidence = std::wstring(recorded != EngineProfileId::Unknown ?
        L"已根据主程序固定版本资源选择 " : L"版本资源不可用，已根据用户明确选择使用 ") +
        EngineProfileName(selected) + L" 模板；未要求代码签名、raw offset 或已知文件哈希。"
        L"安装前备份文件，游戏启动后对展开映像保存原始字节并按版本模板应用。";
    detection.planReport = L"Resolution=VersionSelected\r\nEngineProfile=" +
        std::to_wstring(static_cast<unsigned>(selected)) + L"\r\n" + detection.evidence;
    return true;
}
