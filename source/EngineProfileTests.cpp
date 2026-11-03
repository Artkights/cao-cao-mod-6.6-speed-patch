#include "Patch.h"
#include "EngineProfile.h"
#include "EngineLayout.h"
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
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb")) return false;
    _fseeki64(file, 0, SEEK_END);
    const auto length = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    if (length < 256 || length > 64 * 1024 * 1024) { fclose(file); return false; }
    std::vector<BYTE> raw(static_cast<size_t>(length));
    const bool read = fread(raw.data(), 1, raw.size(), file) == raw.size();
    fclose(file);
    if (!read) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data() + dos->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage > 64 * 1024 * 1024) return false;
    image.assign(nt->OptionalHeader.SizeOfImage, 0);
    memcpy(image.data(), raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        memcpy(image.data() + sections[i].VirtualAddress, raw.data() + sections[i].PointerToRawData, sections[i].SizeOfRawData);
    base = nt->OptionalHeader.ImageBase;
    return true;
}
IMAGE_NT_HEADERS32* Nt(std::vector<BYTE>& image) {
    return reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data() + reinterpret_cast<IMAGE_DOS_HEADER*>(image.data())->e_lfanew);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 4) return 2;
    for (int index = 1; index <= 3; ++index) {
        const EngineProfileId expected = static_cast<EngineProfileId>(63 + index);
        const std::wstring path = argv[index];
        const std::wstring root = path.substr(0, path.find_last_of(L"\\/"));
        EngineDetection detection;
        std::wstring error;
        Check(DetectEngineProfile(root, detection, error) && detection.profile == expected &&
            detection.exeSha256.size() == 64, "detect engine from code and resource, with SHA-256");
        std::vector<BYTE> image;
        DWORD base = 0;
        if (!Load(argv[index], image, base)) { Check(false, "load fixture"); continue; }
        const auto original = image;
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
            incompletePlan.status == EnginePlanStatus::ForceRequired,
            "every required code site must exist before automatic acceptance");
        const size_t delay = layout.sites[SiteDelay] - 0x400000;
        image[delay] = 0x90;
        const bool missingFeature = !DetectEngineProfileImage(image.data(), image.size(), base, detection, error) &&
            detection.forceRequired &&
            (expected == EngineProfileId::Ccz64 || detection.planReport.find(L"Resolution=Missing") != std::wstring::npos);
        Check(missingFeature,
            "a missing acceleration feature requires explicit force despite a recognized engine family");
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
            incompletePlan, base) && incompletePlan.status == EnginePlanStatus::ForceRequired,
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
                detection.forceRequired && detection.planReport.find(L"Resolution=Ambiguous") != std::wstring::npos,
                "duplicated identity signatures retain explicit ambiguous diagnostics");
            image = original;
        }
        EnginePatchPlan forced;
        Check(BuildEnginePatchPlan(image.data(), image.size(), layout, forced, base,
            EngineResolveMode::Forced) && forced.status == EnginePlanStatus::ForceAccepted,
            "an explicitly accepted plan records forced mode without changing addresses");
        Check(DetectForcedEngineProfileImage(image.data(), image.size(), base, expected,
            detection, error) && detection.forced && detection.profile == expected &&
            detection.planReport.find(L"RawFileOffset=") != std::wstring::npos,
            "forced detection reports selected template, actual bytes and raw offsets");
        EnginePatchPlan unavailable;
        EngineLayout unknown;
        Check(!BuildEnginePatchPlan(image.data(), image.size(), unknown, unavailable, base,
            EngineResolveMode::Forced) && unavailable.status == EnginePlanStatus::ForceRequired,
            "an unresolved layout is force-required and never silently accepted");
        if (layout.waitWrapper) image[layout.waitWrapper - 0x400000] = 0x90;
        Check(!DetectEngineProfileImage(image.data(), image.size(), base, detection, error),
            "partial feature matches cannot masquerade as engine identity");
        Check(DetectForcedEngineProfileImage(image.data(), image.size(), base, expected,
            detection, error) && detection.forced && !detection.planReport.empty(),
            "explicit forced template accepts a modified identity while preserving an audit plan");
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
