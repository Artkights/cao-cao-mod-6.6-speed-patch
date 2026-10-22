#include "Patch.h"
#include "EngineProfile.h"
#include "EngineLayout.h"
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {
constexpr size_t kMaximumImage = 64 * 1024 * 1024;

bool Range(size_t offset, size_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

bool Headers(const BYTE* data, size_t size, IMAGE_NT_HEADERS32& nt, size_t& sectionOffset) {
    IMAGE_DOS_HEADER dos = {};
    if (!data || size < sizeof(dos)) return false;
    memcpy(&dos, data, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) ||
        !Range(static_cast<size_t>(dos.e_lfanew), sizeof(nt), size)) return false;
    memcpy(&nt, data + dos.e_lfanew, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        (nt.FileHeader.Characteristics & IMAGE_FILE_DLL) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32) ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96 ||
        nt.OptionalHeader.SizeOfImage < 4096 || nt.OptionalHeader.SizeOfImage > kMaximumImage ||
        nt.OptionalHeader.SizeOfHeaders > nt.OptionalHeader.SizeOfImage) return false;
    sectionOffset = static_cast<size_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        nt.FileHeader.SizeOfOptionalHeader;
    return Range(sectionOffset, nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER), size) &&
        Range(sectionOffset, nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER), nt.OptionalHeader.SizeOfHeaders);
}

bool MapImage(const std::vector<BYTE>& raw, std::vector<BYTE>& mapped, uintptr_t& base) {
    IMAGE_NT_HEADERS32 nt = {};
    size_t sections = 0;
    if (!Headers(raw.data(), raw.size(), nt, sections) ||
        nt.OptionalHeader.SizeOfHeaders > raw.size()) return false;
    mapped.assign(nt.OptionalHeader.SizeOfImage, 0);
    memcpy(mapped.data(), raw.data(), nt.OptionalHeader.SizeOfHeaders);
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section = {};
        memcpy(&section, raw.data() + sections + i * sizeof(section), sizeof(section));
        if (!Range(section.PointerToRawData, section.SizeOfRawData, raw.size()) ||
            !Range(section.VirtualAddress, section.SizeOfRawData, mapped.size()) ||
            !Range(section.VirtualAddress, section.Misc.VirtualSize, mapped.size())) return false;
        if (section.SizeOfRawData)
            memcpy(mapped.data() + section.VirtualAddress, raw.data() + section.PointerToRawData, section.SizeOfRawData);
    }
    base = nt.OptionalHeader.ImageBase;
    return true;
}

bool Hash(const std::vector<BYTE>& raw, std::wstring& result) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, copied = 0;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<BYTE*>(&objectBytes), sizeof(objectBytes), &copied, 0) >= 0;
    std::vector<BYTE> object(objectBytes);
    BYTE digest[32] = {};
    if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) >= 0;
    if (ok) ok = BCryptHashData(hash, const_cast<BYTE*>(raw.data()), static_cast<ULONG>(raw.size()), 0) >= 0;
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return false;
    result.clear();
    static const wchar_t digits[] = L"0123456789ABCDEF";
    for (BYTE value : digest) {
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
    }
    return true;
}

// Version strings in this engine are stale (often "1,0,0,1"). Its actual
// generation is LOWORD(dwFileVersionLS), consistently 5 or 6 in supported bases.
bool ResourceVersion(const BYTE* image, size_t size, const IMAGE_NT_HEADERS32& nt,
    unsigned& generation, std::wstring& error) {
    generation = 0;
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
}

bool DetectEngineProfileImage(const unsigned char* image, size_t size, uintptr_t virtualBase,
    EngineDetection& detection, std::wstring& error) {
    detection = EngineDetection();
    error.clear();
    IMAGE_NT_HEADERS32 nt = {};
    size_t sectionOffset = 0;
    if (!Headers(image, size, nt, sectionOffset) || nt.OptionalHeader.SizeOfImage > size) {
        error = L"目标主程序不是结构完整的 32 位曹操传引擎。";
        return false;
    }
    unsigned generation = 0;
    if (!ResourceVersion(image, size, nt, generation, error)) return false;
    EngineLayout layout;
    if (!ResolveEngineLayout(image, layout, virtualBase) ||
        (layout.profile != EngineProfileId::Ccz65 && layout.profile != EngineProfileId::Ccz66) ||
        !layout.waitWrapper || !layout.nativeStage || !layout.nativeSuper ||
        !layout.nativeSuperState || !layout.phase || !layout.speed || !layout.phaseTable ||
        !(layout.capabilities & CapabilityPhase) || !(layout.capabilities & CapabilityNativeSuper)) {
        error = L"无法唯一确认标准 6.5 或 6.6 的等待、阶段和原生加速调用关系，未安装补丁。";
        return false;
    }
    const unsigned expected = layout.profile == EngineProfileId::Ccz65 ? 5 : 6;
    if (generation && generation != expected) {
        error = L"主程序版本资源与引擎调用布局冲突，未安装补丁。";
        return false;
    }
    detection.profile = layout.profile;
    detection.evidence = std::wstring(L"已识别 ") + EngineProfileName(layout.profile) +
        L"：已校验等待包装、阶段函数、原生加速及状态变量" +
        (generation ? L"，版本资源一致。" : L"；版本资源缺失，使用独立调用布局确认。");
    return true;
}

bool DetectEngineProfile(const std::wstring& root, EngineDetection& detection, std::wstring& error) {
    detection = EngineDetection();
    const std::wstring path = root + L"\\Ekd5.exe";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") || !file) {
        error = L"无法读取游戏根目录的 Ekd5.exe。";
        return false;
    }
    bool ok = _fseeki64(file, 0, SEEK_END) == 0;
    const __int64 length = ok ? _ftelli64(file) : -1;
    ok = ok && length >= 256 && length <= kMaximumImage && _fseeki64(file, 0, SEEK_SET) == 0;
    std::vector<BYTE> raw(ok ? static_cast<size_t>(length) : 0);
    ok = ok && fread(raw.data(), 1, raw.size(), file) == raw.size();
    fclose(file);
    if (!ok) {
        error = L"无法完整读取主程序，或主程序大小异常。";
        return false;
    }
    std::vector<BYTE> mapped;
    uintptr_t base = 0;
    if (!MapImage(raw, mapped, base)) {
        error = L"主程序 PE 段结构无效，不能安装补丁。";
        return false;
    }
    if (!DetectEngineProfileImage(mapped.data(), mapped.size(), base, detection, error)) return false;
    if (!Hash(raw, detection.exeSha256)) {
        detection = EngineDetection();
        error = L"无法计算主程序指纹，未安装补丁。";
        return false;
    }
    return true;
}
