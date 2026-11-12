#include "Patch.h"
#include "EngineProfile.h"
#include "EngineLayout.h"
#include "PeImage.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
bool Load(const wchar_t* path, std::vector<BYTE>& image, DWORD& base) {
    PeFileView file; PeAnalysisView mapped; std::wstring error;
    if (!file.Open(path, error) || !mapped.Map(file, error)) return false;
    image.assign(mapped.Data(), mapped.Data() + mapped.Size());
    base = static_cast<DWORD>(mapped.PreferredBase());
    return true;
}
IMAGE_NT_HEADERS32* Nt(std::vector<BYTE>& image) {
    return reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data() + reinterpret_cast<IMAGE_DOS_HEADER*>(image.data())->e_lfanew);
}

void CheckRuntimeImage(const std::vector<BYTE>& image, DWORD preferredBase, EngineProfileId expected) {
    BYTE* live = static_cast<BYTE*>(VirtualAlloc(nullptr, image.size(), MEM_RESERVE | MEM_COMMIT,
        PAGE_EXECUTE_READWRITE));
    Check(live != nullptr, "allocate isolated runtime verification image");
    if (!live) return;
    memcpy(live, image.data(), image.size());
    const DWORD loadedBase = static_cast<DWORD>(reinterpret_cast<uintptr_t>(live));
    // Synthetic images do not execute an OS relocation pass. Rebase their
    // original absolute image operands so the runtime API sees live addresses.
    const auto* originalNt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.data() +
        reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data())->e_lfanew);
    const auto resource = originalNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    for (size_t at = originalNt->OptionalHeader.SizeOfHeaders; at + sizeof(DWORD) <= image.size(); ++at) {
        if (at >= resource.VirtualAddress && at - resource.VirtualAddress < resource.Size) continue;
        DWORD pointer = 0;
        memcpy(&pointer, live + at, sizeof(pointer));
        if (pointer >= preferredBase && pointer - preferredBase < image.size()) {
            pointer = loadedBase + (pointer - preferredBase);
            memcpy(live + at, &pointer, sizeof(pointer));
            at += sizeof(DWORD) - 1;
        }
    }
    EngineDetection detection; std::wstring error;
    const bool liveVerified = DetectEngineProfileRuntime(live, image.size(), detection, error);
    Check(liveVerified && detection.profile == expected && detection.planStatus == EnginePlanStatus::AutoVerified &&
        detection.layoutStatus == L"RuntimeValidated" &&
        detection.planReport.find(L"RuntimeValidated=1") != std::wstring::npos &&
        detection.planReport.find(L"ByteSource=RuntimeOnly") != std::wstring::npos,
        "strict runtime API validates live executable pages and captures RuntimeOnly restoration bytes");
    if (!liveVerified) {
        const std::wstring report = error + L"\n" + detection.planReport;
        const int count = WideCharToMultiByte(CP_UTF8, 0, report.data(), static_cast<int>(report.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(count), '\0');
        if (count) WideCharToMultiByte(CP_UTF8, 0, report.data(), static_cast<int>(report.size()), &utf8[0], count, nullptr, nullptr);
        fprintf(stderr, "runtime verification: %s\n", utf8.c_str());
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(live + reinterpret_cast<IMAGE_DOS_HEADER*>(live)->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) section[i].SizeOfRawData = 0;
    Check(DetectEngineProfileRuntime(live, image.size(), detection, error) &&
        !DetectEngineProfileImage(live, image.size(), loadedBase, detection, error),
        "runtime-only validation succeeds without raw bytes while disk strict stays rejected");
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        section[i].Characteristics &= ~IMAGE_SCN_MEM_EXECUTE;
    Check(DetectEngineProfileRuntime(live, image.size(), detection, error) &&
        detection.profile == expected,
        "runtime-only caller scanning uses executable page permissions when packed section flags omit execute");
    DWORD previous = 0;
    const bool writable = VirtualProtect(live, image.size(), PAGE_READWRITE, &previous) != FALSE;
    Check(writable && !DetectEngineProfileRuntime(live, image.size(), detection, error),
        "runtime strict rejects a readable image whose code pages are not executable");
    VirtualFree(live, 0, MEM_RELEASE);
}

std::vector<BYTE> RawFile(const wchar_t* path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") || !file) return {};
    _fseeki64(file, 0, SEEK_END);
    const __int64 length = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    std::vector<BYTE> bytes(length > 0 && length < 512ll * 1024 * 1024 ?
        static_cast<size_t>(length) : 0);
    if (bytes.empty() || fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
    fclose(file);
    return bytes;
}

void CheckVersionSelection(const wchar_t* source, EngineProfileId expected, bool protectedFixture = false) {
    std::vector<BYTE> raw = RawFile(source);
    wchar_t temporary[MAX_PATH] = {}, unique[MAX_PATH] = {};
    const bool directory = GetTempPathW(MAX_PATH, temporary) &&
        GetTempFileNameW(temporary, L"xav", 0, unique) && DeleteFileW(unique) &&
        CreateDirectoryW(unique, nullptr);
    Check(directory && !raw.empty(), "create isolated version-selection fixture");
    if (!directory || raw.empty()) return;
    const std::wstring root = unique;
    const std::wstring target = root + L"\\Ekd5.exe";
    auto write = [&]() {
        FILE* file = nullptr;
        if (_wfopen_s(&file, target.c_str(), L"wb") || !file) return false;
        const bool ok = fwrite(raw.data(), 1, raw.size(), file) == raw.size();
        return fclose(file) == 0 && ok;
    };
    EngineDetection selected;
    std::wstring error;
    Check(write() && DetectVersionEngineProfile(root, selected, error) &&
        selected.profile == expected && selected.versionFallback &&
        (selected.variant == L"VersionFallback" || selected.variant == L"Protected220") &&
        selected.exeSha256.size() == 64,
        "fixed version selects explicit fallback ABI without claiming automatic verification");
    const std::wstring originalHash = selected.exeSha256;
    const auto original = raw;
    auto nt = Nt(raw);
    const auto resource = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        const bool resourceSection = resource.VirtualAddress >= section.VirtualAddress &&
            resource.VirtualAddress - section.VirtualAddress <
                (section.Misc.VirtualSize > section.SizeOfRawData ? section.Misc.VirtualSize : section.SizeOfRawData);
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || resourceSection) continue;
        memset(raw.data() + section.PointerToRawData, 0x90, section.SizeOfRawData);
    }
    // A protected build can have no executable raw section at all. Changing
    // an overlay byte invalidates its strict fingerprint without touching the
    // PE headers, resource version or disk/runtime size asymmetry.
    if (protectedFixture || raw == original) raw.push_back(0xA5);
    Check(write() && DetectVersionEngineProfile(root, selected, error) &&
        selected.profile == expected && selected.exeSha256 != originalHash,
        "changed code or protected-file hash does not block version fallback");
    EngineDetection strict;
    Check(!DetectEngineProfile(root, strict, error),
        "altered fixture remains unverified by strict automatic detection");
    const EngineProfileId other = expected == EngineProfileId::Ccz64 ? EngineProfileId::Ccz65 : EngineProfileId::Ccz64;
    Check(!DetectVersionEngineProfile(root, selected, error, other),
        "explicit choice cannot silently override a different fixed version");
    raw = original;
    auto& missing = Nt(raw)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    missing.VirtualAddress = missing.Size = 0;
    Check(write() && !DetectVersionEngineProfile(root, selected, error),
        "missing fixed version requires an explicit profile instead of guessing");
    Check(DetectVersionEngineProfile(root, selected, error, expected) &&
        selected.profile == expected && selected.versionFallback,
        "missing fixed version accepts the user's explicit profile");
    raw = original;
    Nt(raw)->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    Check(write() && !DetectVersionEngineProfile(root, selected, error, expected),
        "explicit profile cannot turn a non-x86 executable into a compatible ABI");
    raw.assign(64, 0);
    Check(write() && !DetectVersionEngineProfile(root, selected, error, expected),
        "truncated input is rejected before selecting a template");
    DeleteFileW(target.c_str());
    RemoveDirectoryW(root.c_str());
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && !_wcsicmp(argv[1], L"--packed")) {
        const std::wstring path = argv[2];
        const std::wstring root = path.substr(0, path.find_last_of(L"\\/"));
        EngineDetection detection; std::wstring error;
        const bool verified = DetectEngineProfile(root, detection, error);
        Check(verified && detection.profile == EngineProfileId::Ccz66 &&
            detection.planStatus == EnginePlanStatus::AutoVerified && detection.variant == L"Standard" &&
            detection.container == L"Enigma" && detection.packedContainer && detection.layoutStatus == L"AutoVerified",
            "large Enigma image maps by logical virtual range and auto-verifies 6.6 layout");
        Check(detection.planReport.find(L"SignatureAttempts.Unique=13") != std::wstring::npos &&
            detection.planReport.find(L"SignatureAttempts.Missing=0") != std::wstring::npos &&
            detection.planReport.find(L"SignatureAttempts.Ambiguous=0") != std::wstring::npos &&
            detection.planReport.find(L"WaitCallers.Unique=17") != std::wstring::npos,
            "packed image reports 13 unique signatures and 17 wait callers");
        PeFileView file; PeAnalysisView view;
        Check(file.Open(path, error) && file.Size() > 64ull * 1024 * 1024 &&
            view.Map(file, error) && view.Size() < 64ull * 1024 * 1024,
            "stream and map a packed executable larger than analysis image ceiling");
        printf("EngineProfileTests: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 3 && !_wcsicmp(argv[1], L"--protected-fixture")) {
        const std::wstring path = argv[2];
        const std::wstring root = path.substr(0, path.find_last_of(L"\\/"));
        EngineDetection detection;
        std::wstring error;
        const bool recognized = DetectEngineProfile(root, detection, error);
        Check(recognized && detection.profile == EngineProfileId::Ccz65 &&
            detection.requiresRuntimeValidation && detection.variant == L"Protected220" &&
            detection.exeSha256 == L"1D8E83F73E7780C60B0B5DE5277D54490F72472270F727C97753F8E72AE4B381",
            "exact Protected220 disk fingerprint selects runtime validation");
        CheckVersionSelection(argv[2], EngineProfileId::Ccz65, true);
        printf("EngineProfileTests: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    if (argc != 4) return 2;
    for (int index = 1; index <= 3; ++index) {
        const EngineProfileId expected = static_cast<EngineProfileId>(63 + index);
        const std::wstring path = argv[index];
        const std::wstring root = path.substr(0, path.find_last_of(L"\\/"));
        CheckVersionSelection(argv[index], expected);
        EngineDetection detection;
        std::wstring error;
        Check(DetectEngineProfile(root, detection, error) && detection.profile == expected &&
            detection.exeSha256.size() == 64, "detect engine from code and resource, with SHA-256");
        std::vector<BYTE> image;
        DWORD base = 0;
        if (!Load(argv[index], image, base)) { Check(false, "load fixture"); continue; }
        const auto original = image;
        CheckRuntimeImage(original, base, expected);
        Check(DetectEngineProfileImage(image.data(), image.size(), base, detection, error) &&
            detection.profile == expected, "mapped image agrees with on-disk detection");
        auto& resources = Nt(image)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        resources.VirtualAddress = resources.Size = 0;
        Check(DetectEngineProfileImage(image.data(), image.size(), base, detection, error) &&
            detection.profile == expected, "missing version resource uses unique semantic call graph");
        image = original;
        const auto resource = Nt(image)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        bool changedVersion = false;
        for (size_t at = resource.VirtualAddress; at + sizeof(VS_FIXEDFILEINFO) <= resource.VirtualAddress + resource.Size; at += 4) {
            auto info = reinterpret_cast<VS_FIXEDFILEINFO*>(image.data() + at);
            if (info->dwSignature == VS_FFI_SIGNATURE && info->dwStrucVersion == VS_FFI_STRUCVERSION) {
                const unsigned conflictingGeneration = expected == EngineProfileId::Ccz64 ? 6u :
                    expected == EngineProfileId::Ccz65 ? 4u : 5u;
                info->dwFileVersionLS = (info->dwFileVersionLS & 0xffff0000u) | conflictingGeneration;
                changedVersion = true;
            }
        }
        Check(changedVersion && !DetectEngineProfileImage(image.data(), image.size(), base, detection, error),
            "conflicting version resource cannot select a payload");
        for (size_t at = resource.VirtualAddress; at + sizeof(VS_FIXEDFILEINFO) <= resource.VirtualAddress + resource.Size; at += 4) {
            auto info = reinterpret_cast<VS_FIXEDFILEINFO*>(image.data() + at);
            if (info->dwSignature == VS_FFI_SIGNATURE && info->dwStrucVersion == VS_FFI_STRUCVERSION)
                info->dwFileVersionLS = (info->dwFileVersionLS & 0xffff0000u) | 7u;
        }
        Check(!DetectEngineProfileImage(image.data(), image.size(), base, detection, error),
            "future generation resource conflicts with an older layout");
        image = original;
        EngineLayout layout;
        Check(ResolveEngineLayout(image.data(), layout, base), "resolve fixture identity graph");
        Check(layout.plan.status == EnginePlanStatus::AutoVerified && layout.plan.confidence >= 50,
            "resolved layout carries an auditable automatic patch plan");
        const bool hasNativeRoute = expected != EngineProfileId::Ccz64;
        Check(layout.plan.sites[SiteDialog].present &&
            layout.plan.sites[SiteDialog].rawFileOffset != SIZE_MAX &&
            (!hasNativeRoute || layout.plan.nativeRoute.rawFileOffset != SIZE_MAX),
            "automatic plan maps runtime sites back to raw file offsets");
        bool nativeBytes = true;
        if (hasNativeRoute) {
            nativeBytes = memcmp(layout.plan.nativeRoute.original,
                    image.data() + layout.nativeSuper - 0x400000, 5) == 0 &&
                memcmp(layout.plan.nativeState.original,
                    image.data() + layout.nativeSuperState - 0x400000, 1) == 0 &&
                memcmp(layout.plan.nativeBusy.original,
                    image.data() + layout.nativeSuperBusy - 0x400000, 1) == 0 &&
                memcmp(layout.plan.phaseTable.original,
                    image.data() + layout.phaseTable - 0x400000, 4) == 0;
        }
        // Every generation must expose the timing state used by the patch,
        // including 6.4's proxy-owned native route.  These are read-only
        // dependencies but still require a valid executable/data mapping and
        // captured original bytes in the audit plan.
        const EngineSitePlan* timing[] = {
            &layout.plan.phaseValue, &layout.plan.speedValue,
            &layout.plan.timerContext};
        for (const EngineSitePlan* site : timing) {
            if (!site->required) continue;
            const size_t rva = site->legacyAddress - 0x400000;
            nativeBytes = nativeBytes && site->present && site->rawFileOffset != SIZE_MAX &&
                memcmp(site->original, image.data() + rva, site->length) == 0;
        }
        Check(nativeBytes,
            "native routing, live data and phase table reports capture actual original bytes");
        EngineLayout missingSite = layout;
        missingSite.sites[SiteDelay] = 0;
        EnginePatchPlan incompletePlan;
        Check(!BuildEnginePatchPlan(image.data(), image.size(), missingSite, incompletePlan, base) &&
            incompletePlan.status == EnginePlanStatus::Rejected,
            "every required code site must exist before automatic acceptance");
        const size_t delay = layout.sites[SiteDelay] - 0x400000;
        image[delay] = 0x90;
        const bool missingFeature = !DetectEngineProfileImage(image.data(), image.size(), base, detection, error) &&
            (expected == EngineProfileId::Ccz64 || detection.planReport.find(L"Resolution=Missing") != std::wstring::npos);
        Check(missingFeature,
            "a missing acceleration feature stops automatic installation despite a recognized engine family");
        image = original;
        auto mappedSections = IMAGE_FIRST_SECTION(Nt(image));
        bool unmappedData = false;
        for (WORD sectionIndex = 0; sectionIndex < Nt(image)->FileHeader.NumberOfSections; ++sectionIndex) {
            auto& section = mappedSections[sectionIndex];
            // 6.4 owns native routing in the proxy, so use a required code
            // site for its raw mapping check; 6.5/6.6 retain native state.
            const uintptr_t stateRva = expected == EngineProfileId::Ccz64
                ? layout.sites[SiteDelay] - 0x400000
                : layout.nativeSuperState - 0x400000;
            if (stateRva < section.VirtualAddress ||
                stateRva - section.VirtualAddress >= section.SizeOfRawData) continue;
            section.SizeOfRawData = 0;
            unmappedData = true;
            break;
        }
        Check(unmappedData && !BuildEnginePatchPlan(image.data(), image.size(), layout,
            incompletePlan, base) && incompletePlan.status == EnginePlanStatus::Rejected,
            "required native data without a raw file mapping cannot be automatically accepted");
        image = original;
        // Move a recognized caller's complete context into executable padding,
        // erase its old location, and repair the CALL's relative destination.
        // This rejects a resolver that merely validates a fixed return table.
        size_t movedContext = 0;
        const auto sections = IMAGE_FIRST_SECTION(Nt(image));
        for (WORD sectionIndex = 0; sectionIndex < Nt(image)->FileHeader.NumberOfSections && !movedContext;
            ++sectionIndex) {
            const auto& section = sections[sectionIndex];
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            size_t run = 0;
            for (size_t at = section.VirtualAddress;
                at < section.VirtualAddress + section.SizeOfRawData; ++at) {
                run = image[at] == 0x90 ? run + 1 : 0;
                if (run >= 128) { movedContext = at - 95; break; }
            }
        }
        bool callerMoved = movedContext && layout.waitCallerCount;
        if (callerMoved) {
            const size_t oldContext = layout.waitCallers[0] - 0x400000 - 21;
            memcpy(image.data() + movedContext, image.data() + oldContext, 48);
            memset(image.data() + oldContext, 0x90, 48);
            const int32_t relative = static_cast<int32_t>(layout.waitWrapper -
                (0x400000 + movedContext + 21));
            memcpy(image.data() + movedContext + 17, &relative, sizeof(relative));
            EngineLayout moved;
            callerMoved = ResolveEngineLayout(image.data(), moved, base) &&
                moved.waitCallerCount == layout.waitCallerCount;
            bool hasMoved = false;
            for (size_t caller = 0; caller < moved.waitCallerCount; ++caller) {
                callerMoved = callerMoved && moved.waitCallers[caller] != layout.waitCallers[0];
                if (moved.waitCallers[caller] == 0x400000 + movedContext + 21)
                    hasMoved = moved.waitKinds[caller] == layout.waitKinds[0];
            }
            callerMoved = callerMoved && hasMoved;
        }
        Check(callerMoved,
            "context signatures resolve a relocated caller after its original location is erased");
        image = original;
        if (movedContext) {
            // Duplicate the complete wait-wrapper identity signature.  The
            // resolver must report an ambiguous candidate instead of silently
            // selecting the first match.
            memcpy(image.data() + movedContext,
                image.data() + layout.waitWrapper - 0x400000, 32);
            Check(!DetectEngineProfileImage(image.data(), image.size(), base, detection, error) &&
                detection.planReport.find(L"Resolution=Ambiguous") != std::wstring::npos,
                "duplicated identity signatures retain explicit ambiguous diagnostics");
            image = original;
        }
        EnginePatchPlan unavailable;
        EngineLayout unknown;
        Check(!BuildEnginePatchPlan(image.data(), image.size(), unknown, unavailable, base) &&
            unavailable.status == EnginePlanStatus::Rejected,
            "an unresolved layout is rejected and never silently accepted");
        if (layout.waitWrapper) image[layout.waitWrapper - 0x400000] = 0x90;
        Check(!DetectEngineProfileImage(image.data(), image.size(), base, detection, error),
            "partial feature matches cannot masquerade as engine identity");
        image = original;
        Nt(image)->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        Check(!DetectEngineProfileImage(image.data(), image.size(), base, detection, error), "reject non-x86 target");
        image = original;
        Nt(image)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress = 0xfffffff0;
        Check(!DetectEngineProfileImage(image.data(), image.size(), base, detection, error), "reject out-of-range resource directory");
        Check(!DetectEngineProfileImage(image.data(), 64, base, detection, error), "reject truncated PE headers");
    }
    printf("EngineProfileTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
