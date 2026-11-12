#include "EngineLayout.h"
#include "PeImage.h"
#include <stdio.h>
#include <vector>
#include <string.h>

HMODULE gInstance = nullptr;
uintptr_t gGameBase = 0;
bool EngineTestInitializeImage(uintptr_t virtualBase);
bool EngineTestInitializeRuntimeImage(uintptr_t virtualBase);
bool EngineTestInitializeVersionImage(uintptr_t virtualBase, EngineProfileId profile);
void EngineTestFailWrite(int index);
bool EngineTestOwnedPhaseState(int phase, int& value, int& counter);

namespace {
int failures = 0;
void Check(bool condition, const char* name) {
    wprintf(L"%ls %hs\n", condition ? L"PASS" : L"FAIL", name);
    fflush(stdout);
    if (!condition) ++failures;
}
bool LoadImage(const wchar_t* path, std::vector<BYTE>& image, DWORD& base) {
    PeFileView file;
    PeAnalysisView view;
    std::wstring error;
    if (!file.Open(path, error) || !view.Map(file, error)) return false;
    image.assign(view.Data(), view.Data() + view.Size());
    base = static_cast<DWORD>(view.PreferredBase());
    return true;
}

void ExerciseVersionFallback(const std::vector<BYTE>& original, DWORD base, EngineProfileId profile) {
    std::vector<BYTE> candidate = original;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(candidate.data());
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(candidate.data() + dos->e_lfanew);
    auto sections = IMAGE_FIRST_SECTION(nt);
    // Model code expanded into a protected virtual region: runtime bytes are
    // readable, but the on-disk sections offer neither code flags nor raw bytes.
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        sections[i].Characteristics &= ~IMAGE_SCN_MEM_EXECUTE;
        sections[i].SizeOfRawData = 0;
    }
    candidate[0x2D5B0] = 0x90;
    candidate[0x2C4E8] = 0x90;
    candidate[0x13324] = 0x12;
    const auto baseline = candidate;
    EngineLayout strict, selected;
    Check(!ResolveEngineLayout(candidate.data(), strict, base),
        "protected runtime without executable section flags fails strict layout identification");
    Check(ResolveVersionEngineLayout(candidate.data(), candidate.size(), profile, selected, base) &&
        selected.plan.status == EnginePlanStatus::VersionSelected &&
        selected.plan.sites[SiteDialog].rawFileOffset == SIZE_MAX &&
        selected.plan.sites[SiteDialog].original[0] == 0x90 &&
        selected.plan.sites[SiteWaitCall].original[0] == 0x90,
        "version template accepts expanded virtual bytes and captures the actual restoration baseline");
    Check(!ResolveVersionEngineLayout(candidate.data(), 0x1000, profile, selected, base) &&
        !ResolveVersionEngineLayout(candidate.data(), candidate.size(), EngineProfileId::Unknown, selected, base),
        "version template still rejects truncated images and an unspecified engine family");
    gGameBase = reinterpret_cast<uintptr_t>(candidate.data());
    Check(EngineTestInitializeVersionImage(base, profile) && candidate == baseline &&
        RuntimeEnginePatchPlan().status == EnginePlanStatus::VersionSelected,
        "version initialization keeps divergent bytes intact until settings are committed");
    Settings settings;
    settings.superEnabled = true;
    for (bool& flag : settings.superFlags) flag = false;
    settings.superFlags[0] = true;
    EngineTestFailWrite(0);
    Check(!ApplyEngineState(settings) && candidate == baseline,
        "version-template write failure rolls back without losing captured bytes");
    EngineTestFailWrite(-1);
    Check(ApplyEngineState(settings) && candidate[0x2D5B0] == 0xC2,
        "version-selected injection applies despite a mismatched original signature");
    RestoreEngine();
    Check(candidate == baseline && !EngineError()[0],
        "version-template uninstall restores all actual bytes including the original modifier and native state");
}

void ExerciseRuntimeView(const std::vector<BYTE>& original, DWORD base, const EngineLayout& layout) {
    auto candidate = original;
    gGameBase = reinterpret_cast<uintptr_t>(candidate.data());
    Check(!EngineTestInitializeRuntimeImage(base) && candidate == original,
        "strict runtime initialization rejects a readable non-executable heap image without writing");
    BYTE* live = static_cast<BYTE*>(VirtualAlloc(nullptr, original.size(), MEM_RESERVE | MEM_COMMIT,
        PAGE_EXECUTE_READWRITE));
    Check(live != nullptr, "allocate isolated executable runtime image");
    if (!live) return;
    memcpy(live, original.data(), original.size());
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(live);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(live + dos->e_lfanew);
    auto sections = IMAGE_FIRST_SECTION(nt);
    // A packed section may store far more raw container data than belongs in
    // its virtual image. Runtime resolution must not demand disk raw offsets.
    sections[nt->FileHeader.NumberOfSections - 1].SizeOfRawData = 0x0AF58000;
    std::vector<BYTE> baseline(live, live + original.size());
    gGameBase = reinterpret_cast<uintptr_t>(live);
    const bool initialized = EngineTestInitializeRuntimeImage(base);
    Check(initialized && RuntimeEnginePatchPlan().runtimeValidated &&
        RuntimeEnginePatchPlan().sites[SiteDialog].byteSource == EngineSiteByteSource::RuntimeOnly &&
        RuntimeEnginePatchPlan().sites[SiteDialog].rawFileOffset == SIZE_MAX,
        "runtime plan verifies live executable pages and ignores oversized packed-container raw extents");
    if (initialized) {
        const uintptr_t code = layout.sites[SiteDialog] - 0x400000;
        DWORD oldProtection = 0;
        const bool inaccessible = VirtualProtect(live + code, 1, PAGE_NOACCESS, &oldProtection) != 0;
        Settings settings;
        settings.superEnabled = true;
        for (bool& flag : settings.superFlags) flag = false;
        settings.superFlags[0] = true;
        Check(inaccessible && !ApplyEngineState(settings),
            "live page becoming inaccessible after resolution prevents a patch write");
        DWORD ignored = 0;
        if (inaccessible) VirtualProtect(live + code, 1, oldProtection, &ignored);
        RestoreEngine();
        Check(memcmp(live, baseline.data(), baseline.size()) == 0,
            "runtime page failure leaves every original byte intact after restoration");
    }
    VirtualFree(live, 0, MEM_RELEASE);
    gGameBase = 0;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    for (int index = 1; index < argc; ++index) {
        std::vector<BYTE> image;
        DWORD base = 0;
        if (!LoadImage(argv[index], image, base)) { ++failures; continue; }
        EngineLayout layout;
        const bool found = ResolveEngineLayout(image.data(), layout, base);
        wprintf(L"%s: %s\n", found ? L"MATCH" : L"NO MATCH", argv[index]);
        Check(found, "identify complete acceleration layout without an executable hash");
        if (!found) continue;
        wprintf(L"sites=%08X/%08X/%08X/%08X/%08X/%08X stage=%08X phase=%08X speed=%08X\n",
            static_cast<unsigned>(layout.sites[0]), static_cast<unsigned>(layout.sites[1]),
            static_cast<unsigned>(layout.sites[2]), static_cast<unsigned>(layout.sites[3]),
            static_cast<unsigned>(layout.sites[4]), static_cast<unsigned>(layout.sites[5]),
            static_cast<unsigned>(layout.nativeStage), static_cast<unsigned>(layout.phase),
            static_cast<unsigned>(layout.speed));
        const bool is64 = layout.profile == EngineProfileId::Ccz64;
        const bool is65 = layout.profile == EngineProfileId::Ccz65;
        Check((is64 && layout.nativeSuper == 0 && layout.nativeSuperState == 0 &&
            layout.waitWrapper == 0x42C4E0 && layout.timerContext == 0x4B1B18 &&
            layout.stageUsesThis && layout.phaseOwnedByPatch &&
            layout.sites[SiteMapText] == 0x4327EF && !layout.dialog3Applicable) ||
            (is65 && layout.nativeSuper == 0x4250B3 && layout.nativeSuperState == 0x4CE7BF &&
            layout.waitWrapper == 0x42C4E0 && layout.timerContext == 0x4B1B18 && layout.stageUsesThis &&
            layout.sites[SiteMapText] == 0x4327EF && !layout.dialog3Applicable) ||
            (!is64 && !is65 && layout.nativeSuper == 0x4250E6 && layout.nativeSuperState == 0x500ADB &&
            layout.waitWrapper == 0x42C4E5 && !layout.stageUsesThis && layout.dialog3Applicable),
            "identify independent profile, native owner and timer ABI");
        Check(layout.sites[SiteExtraDelay] == 0x415B40 && layout.sites[SiteWaitBranch] == 0x42C4EF &&
            layout.sites[SiteDialogDisplay] == 0x42C812 && layout.sites[SiteExtendedDisplay] == 0x455CE5 &&
            (is64 || is65 ? layout.sites[SiteDialog3] == 0 : layout.sites[SiteDialog3] == 0x42DB2C),
            "resolve every acceleration site implemented by this engine profile");
        if (is65) {
            DWORD dialog3, next;
            memcpy(&dialog3, image.data() + 0x10C18, 4);
            memcpy(&next, image.data() + 0x10C1C, 4);
            Check(dialog3 == 0x4D0000 && next == dialog3,
                "6.5 dialog3 dispatch slot is an unimplemented command, not a missed patch site");
        }
        const unsigned requiredCapabilities = CapabilitySuperDialog|CapabilitySuperMapText|
            CapabilitySuperDelay|CapabilitySuperR|CapabilitySuperS|CapabilityPhase|CapabilityInput|
            CapabilityExtraDelay|CapabilityDialogDisplay|CapabilityExtendedDisplay|
            (is64 ? 0u : CapabilityNativeSuper) |
            (is64 || is65 ? 0u : CapabilityDialog3);
        wprintf(L"capabilities=%08X expected=%08X callers=%u nativeCall=%08X\n", layout.capabilities, requiredCapabilities, static_cast<unsigned>(layout.waitCallerCount), static_cast<unsigned>(layout.nativeSuperCall));
        const bool complete = (layout.capabilities & requiredCapabilities) == requiredCapabilities;
        Check(complete, "all acceleration capabilities implemented by this profile pass semantic checks");
        // Never index a missing site after a failed resolver assertion.
        if (!complete) continue;
        bool safeCallers = is64 ? layout.waitCallerCount > 0 :
            layout.waitCallerCount == (is65 ? 16u : 17u);
        for (size_t i = 0; i < layout.waitCallerCount; ++i)
            safeCallers = safeCallers && layout.waitCallers[i] != layout.sites[SiteRWait] + 5;
        Check(safeCallers, "profile-specific S wait whitelist excludes the separately controlled R wait");
        const auto original = image;
        ExerciseRuntimeView(original, base, layout);
        ExerciseVersionFallback(original, base, layout.profile);
        if (is64) {
            // Exercise the same public settings transaction without indexing
            // native Z/phase-table fields that do not exist in 6.4.
            gGameBase = reinterpret_cast<uintptr_t>(image.data());
            Check(EngineTestInitializeImage(base), "initialize 6.4 mapped base without its original hash");
            Settings ownedSettings;
            ownedSettings.superEnabled = true;
            bool allRestored = true;
            for (unsigned mask = 0; mask < 32; ++mask) {
                for (unsigned flag = 0; flag < 5; ++flag)
                    ownedSettings.superFlags[flag] = (mask & (1u << flag)) != 0;
                const bool applied = ApplyEngineState(ownedSettings);
                if (mask & 1u) {
                    allRestored = allRestored &&
                        memcmp(image.data() + layout.sites[SiteDialog] - 0x400000,
                            layout.siteEnabled[SiteDialog], 4) == 0 &&
                        memcmp(image.data() + layout.sites[SiteDialogDisplay] - 0x400000,
                            layout.siteEnabled[SiteDialogDisplay], 2) == 0;
                }
                allRestored = applied && !EngineError()[0] && allRestored;
                RestoreEngine();
                allRestored = image == original && allRestored;
            }
            Check(allRestored, "6.4 all 32 super combinations use profile bytes and restore exactly");
            ownedSettings = Settings();
            ownedSettings.phaseEnabled = true;
            ownedSettings.phase[0] = 60;
            ownedSettings.phase[1] = 61;
            ownedSettings.phase[2] = 62;
            int value = -1, counter = -1;
            const bool ownedApplied = ApplyEngineState(ownedSettings);
            Check(ownedApplied && !EngineError()[0] &&
                EngineTestOwnedPhaseState(1, value, counter) && value == 61 && counter == 0 &&
                memcmp(image.data() + layout.sites[SiteWaitCall] - 0x400000,
                    original.data() + layout.sites[SiteWaitCall] - 0x400000, 5) != 0,
                "6.4 phase acceleration installs a bridge and keeps parameters in the proxy");
            ownedSettings.phaseFlags[1] = false;
            Check(ApplyEngineState(ownedSettings) && EngineTestOwnedPhaseState(1, value, counter) &&
                value == 50, "6.4 excluded phase retains the established fixed-50 behavior");
            RestoreEngine();
            Check(image == original && !EngineTestOwnedPhaseState(1, value, counter),
                "6.4 phase uninstall restores every byte and clears proxy parameters");
            EngineTestFailWrite(0);
            Check(!ApplyEngineState(ownedSettings) && image == original &&
                !EngineTestOwnedPhaseState(1, value, counter),
                "6.4 phase bridge write failure keeps original bytes and inactive proxy state");
            EngineTestFailWrite(-1);
            ownedSettings = Settings();
            ownedSettings.superEnabled = true;
            for (bool& flag : ownedSettings.superFlags) flag = false;
            ownedSettings.superFlags[0] = true;
            Check(ApplyEngineState(ownedSettings), "6.4 dialog applies before conflict test");
            image[layout.sites[SiteDialog] - 0x400000] ^= 1;
            const BYTE conflict = image[layout.sites[SiteDialog] - 0x400000];
            RestoreEngine();
            Check(image[layout.sites[SiteDialog] - 0x400000] == conflict &&
                wcsstr(EngineError(), L"运行时恢复冲突") != nullptr,
                "6.4 recovery journal protects a dialog site changed by another patch");
            image = original;
            Check(EngineTestInitializeImage(base), "6.4 reinitializes after the protected conflict");
            RestoreEngine();
            Check(image == original, "6.4 initialization leaves the mapped image unchanged on restore");
            continue;
        }
        gGameBase = reinterpret_cast<uintptr_t>(image.data());
        const bool initialized = EngineTestInitializeImage(base);
        Check(initialized, "initialize real mapped base without its original hash");
        if (!initialized) continue;
        Settings settings;
        settings.superEnabled = true;
        for (bool& flag : settings.superFlags) flag = false;
        settings.superFlags[0] = true;
        Check(ApplyEngineState(settings) && (!layout.dialog3Applicable || image[layout.sites[SiteDialog3] - 0x400000] == 0xC2) &&
            image[layout.sites[SiteDialogDisplay] - 0x400000] == 0xEB &&
            image[layout.nativeSuper - 0x400000] == 0xE9 &&
            image[layout.nativeSuperState - 0x400000] == 1,
            "dialog switch covers this profile's dialogue paths and owns native Z");
        RestoreEngine();
        Check(image == original, "dialog3 and native Z routing restore byte-for-byte");
        Check(ApplyEngineState(settings), "apply dialog state before restore rollback test");
        const auto beforeRestoreFailure = image;
        EngineTestFailWrite(1);
        RestoreEngine();
        Check(image == beforeRestoreFailure && wcsstr(EngineError(), L"写入失败") != nullptr,
            "failed multi-site restore rolls back every earlier write and preserves installed state");
        EngineTestFailWrite(-1);
        RestoreEngine();
        Check(image == original, "restoration can retry after its failed transaction rolled back");
        Check(ApplyEngineState(settings), "apply dialog state before already-restored site test");
        memcpy(image.data() + layout.sites[SiteDialog] - 0x400000,
            original.data() + layout.sites[SiteDialog] - 0x400000, 4);
        RestoreEngine();
        Check(image == original && !EngineError()[0],
            "journal accepts a site already restored to its captured original bytes");
        Check(ApplyEngineState(settings), "reapply dialog state before conflict journal test");
        image[layout.sites[SiteDialog] - 0x400000] ^= 0x01;
        const BYTE conflicting = image[layout.sites[SiteDialog] - 0x400000];
        RestoreEngine();
        Check(image[layout.sites[SiteDialog] - 0x400000] == conflicting &&
            wcsstr(EngineError(), L"运行时恢复冲突") != nullptr,
            "runtime journal refuses to overwrite a site changed by another patch");
        image = original;
        Check(EngineTestInitializeImage(base), "reinitialize after a protected restore conflict");
        EngineTestFailWrite(1);
        Check(!ApplyEngineState(settings) && image == original,
            "multi-site dialog transaction restores earlier writes after an injected failure");
        EngineTestFailWrite(-1);
        settings = Settings();
        settings.superEnabled = true;
        for (bool& flag : settings.superFlags) flag = false;
        settings.superFlags[1] = true;
        Check(ApplyEngineState(settings) && image[layout.sites[SiteMapText] - 0x400000] == 0xC2 &&
            image[layout.sites[SiteDialogDisplay] - 0x400000] == 0xEB,
            "map text switch owns the shared display fast path");
        RestoreEngine();
        settings = Settings();
        settings.superEnabled = true;
        for (bool& flag : settings.superFlags) flag = false;
        settings.superFlags[2] = true;
        Check(ApplyEngineState(settings) && image[layout.sites[SiteDelay] - 0x400000] == 0xC2 &&
            image[layout.sites[SiteExtraDelay] - 0x400000] == 0xEB,
            "delay switch covers both native delay paths");
        RestoreEngine();
        settings = Settings();
        settings.superEnabled = true;
        for (bool& flag : settings.superFlags) flag = false;
        settings.superFlags[4] = true;
        Check(ApplyEngineState(settings) &&
            image[layout.sites[SiteWaitCall] - 0x400000] == 0xE8 &&
            image[layout.sites[SiteWaitBranch] - 0x400000] == 0x75 &&
            image[layout.sites[SiteExtendedDisplay] - 0x400000] == 0xEB &&
            image[layout.sites[SiteExtendedDisplay] - 0x400000 + 1] ==
                layout.siteEnabled[SiteExtendedDisplay][1],
            "S switch keeps the native completion branch and accelerates extended display");
        RestoreEngine();
        settings = Settings();
        settings.superEnabled = true;
        for (bool& flag : settings.superFlags) flag = false;
        settings.superFlags[3] = true;
        bool rNops = ApplyEngineState(settings);
        for (size_t i = 0; i < 5; ++i)
            rNops = rNops && image[layout.sites[SiteRWait] - 0x400000 + i] == 0x90;
        Check(rNops && image[layout.sites[SiteWaitCall] - 0x400000] == 0xE8,
            "R switch skips only its dedicated wait and does not install the S helper");
        RestoreEngine();
        settings = Settings();
        settings.superEnabled = true;
        bool restored = true;
        for (unsigned mask = 0; mask < 32; ++mask) {
            for (unsigned flag = 0; flag < 5; ++flag) settings.superFlags[flag] = (mask & (1u << flag)) != 0;
            restored = ApplyEngineState(settings) && restored;
            RestoreEngine();
            restored = image == original && restored;
        }
        Check(restored, "all 32 super combinations restore this base's actual original instructions");
        settings = Settings();
        settings.phaseEnabled = true;
        settings.phase[0] = 60;
        settings.phase[1] = 61;
        settings.phase[2] = 62;
        const bool stageApplied = ApplyEngineState(settings);
        Check(stageApplied && image[layout.phaseTable - 0x400000] == 60 &&
            image[layout.phaseTable - 0x400000 + 1] == 61 &&
            image[layout.phaseTable - 0x400000 + 2] == 62,
            "phase acceleration writes the selected native phase table");
        Check(stageApplied && image[layout.sites[4] - 0x400000] == 0xE8 &&
            image[layout.sites[4] - 0x400000 + 5] == original[layout.sites[4] - 0x400000 + 5] &&
            image[layout.sites[4] - 0x400000 + 6] == original[layout.sites[4] - 0x400000 + 6] &&
            image[layout.sites[4] - 0x400000 + 7] == original[layout.sites[4] - 0x400000 + 7],
            "phase acceleration changes only the native wait CALL target");
        settings.phaseFlags[1] = false;
        const bool fixedDefault = ApplyEngineState(settings);
        Check(fixedDefault && image[layout.phaseTable - 0x400000] == 60 &&
            image[layout.phaseTable - 0x400000 + 1] == 50 &&
            image[layout.phaseTable - 0x400000 + 2] == 62,
            "an unchecked phase uses the legacy fixed 50 value");
        image[layout.phaseCounter - 0x400000] = 7;
        image[layout.nativeSuperState - 0x400000] ^= 1;
        image[layout.nativeSuperBusy - 0x400000] = 1;
        RestoreEngine();
        Check(stageApplied && image == original && !EngineError()[0],
            "restore ignores live phase counter and native state changes while restoring the initial snapshot");
        image[layout.phase - 0x400000] = 2;
        Check(CurrentGamePhase() == 2, "wheel phase reads the address resolved from this base");
        image = original;
        image[layout.sites[5] - 0x400000] = VK_CONTROL;
        const auto ctrlBase = image;
        gGameBase = reinterpret_cast<uintptr_t>(image.data());
        Check(EngineTestInitializeImage(base) && ApplyEngineState(Settings()) &&
            image[layout.sites[5] - 0x400000] == VK_SHIFT,
            "apply Shift preference on a base originally using Ctrl");
        RestoreEngine();
        Check(image == ctrlBase, "closing patch restores original Ctrl rather than reference-base Shift");
        image = original;
        const DWORD relocatedBase = base + 0x1000000;
        std::vector<size_t> absoluteOperands = {layout.nativeStage + (is65 ? 7 : 6) - 0x400000,
            layout.nativeStage + (is65 ? 40 : 39) - 0x400000,
            layout.nativeStage + (is65 ? 45 : 44) - 0x400000,
            layout.nativeStage + (is65 ? 51 : 50) - 0x400000,
            layout.nativeSuper + 4 - 0x400000, layout.nativeSuper + 9 - 0x400000};
        const size_t nativeStart = layout.nativeSuper - 0x400000;
        for (size_t offset = nativeStart + 12; offset + 10 < nativeStart + (is65 ? 152 : 172); ++offset)
            if (image[offset] == 0xC7 && image[offset + 1] == 0x05)
                absoluteOperands.push_back(offset + 2);
        if (is65) {
            absoluteOperands.push_back(layout.waitWrapper + 4 - 0x400000);
            absoluteOperands.push_back(0x10C18);
            absoluteOperands.push_back(0x10C1C);
        }
        for (size_t operand : absoluteOperands) {
            DWORD pointer;
            memcpy(&pointer, image.data() + operand, sizeof(pointer));
            pointer += relocatedBase - base;
            memcpy(image.data() + operand, &pointer, sizeof(pointer));
        }
        EngineLayout relocated;
        Check(ResolveEngineLayout(image.data(), relocated, relocatedBase) &&
            relocated.phase == layout.phase && relocated.speed == layout.speed &&
            relocated.nativeStage == layout.nativeStage && relocated.sleep == layout.sleep &&
            relocated.nativeSuperState == layout.nativeSuperState &&
            relocated.sites[SiteDialog3] == layout.sites[SiteDialog3],
            "relocated image preserves absolute data and relative helper mappings");
        image = original;

        std::vector<size_t> nativeIndices;
        const size_t lengths[EngineSiteCount] = {4,4,3,5,5,1,4,2,2,2,2};
        for (size_t site = 0; site < EngineSiteCount; ++site) {
            if (!(layout.nativeSiteMask & (1u << site))) continue;
            nativeIndices.push_back(site);
            memcpy(image.data() + layout.sites[site] - 0x400000,
                layout.siteEnabled[site], lengths[site]);
        }
        image[layout.nativeSuperState - 0x400000] = 1;
        gGameBase = reinterpret_cast<uintptr_t>(image.data());
        Check(EngineTestInitializeImage(base) && image == original,
            "complete native-on layout normalizes to the canonical off baseline");
        image = original;
        memcpy(image.data() + layout.sites[SiteDialog] - 0x400000, layout.siteEnabled[SiteDialog], 4);
        gGameBase = reinterpret_cast<uintptr_t>(image.data());
        Check(EngineTestInitializeImage(base) && wcsstr(EngineError(), L"未知") != nullptr,
            "mixed native-toggle bytes are rejected instead of captured as originals");
        image = original;
        image[0x40] ^= 0x71;
        EngineLayout altered;
        Check(ResolveEngineLayout(image.data(), altered, base), "accept unrelated EXE customization");
        const size_t key = layout.sites[5] - 0x400000;
        image[key] = VK_CONTROL;
        Check(ResolveEngineLayout(image.data(), altered, base), "accept a base using native Ctrl acceleration");
        const size_t delay = layout.sites[2] - 0x400000;
        std::vector<BYTE> backup(is65 ? 47u : 36u);
        memcpy(backup.data(), image.data() + delay + 0x100, backup.size());
        memcpy(image.data() + delay + 0x100, image.data() + delay, backup.size());
        Check(ResolveEngineLayout(image.data(), altered, base) &&
            !(altered.capabilities & CapabilitySuperDelay) &&
            altered.plan.status == EnginePlanStatus::Rejected &&
            altered.plan.evidence.find(L"Resolution=Ambiguous") != std::wstring::npos,
            "ambiguous delay signatures disable only the affected feature");
        memset(image.data() + delay, 0x90, backup.size());
        const size_t callOffsets[] = {is65 ? 12u : 6u, is65 ? 20u : 11u, is65 ? 34u : 25u};
        for (size_t offset : callOffsets) {
            int32_t displacement;
            memcpy(&displacement, image.data() + delay + 0x100 + offset + 1, sizeof(displacement));
            displacement -= 0x100;
            memcpy(image.data() + delay + 0x100 + offset + 1, &displacement, sizeof(displacement));
        }
        Check(ResolveEngineLayout(image.data(), altered, base) && altered.sites[2] == layout.sites[2] + 0x100,
            "relocated delay function resolves its native call targets");
        memcpy(image.data() + delay, image.data() + delay + 0x100, backup.size());
        for (size_t offset : callOffsets) {
            int32_t displacement;
            memcpy(&displacement, image.data() + delay + offset + 1, sizeof(displacement));
            displacement += 0x100;
            memcpy(image.data() + delay + offset + 1, &displacement, sizeof(displacement));
        }
        memcpy(image.data() + delay + 0x100, backup.data(), backup.size());
        image[delay + (is65 ? 32 : 23)] = 0x90;
        Check(ResolveEngineLayout(image.data(), altered, base) &&
            !(altered.capabilities & CapabilitySuperDelay) &&
            altered.plan.status == EnginePlanStatus::Rejected &&
            altered.plan.evidence.find(L"Resolution=Missing") != std::wstring::npos,
            "unrecognized wait semantics disable only the affected feature");
    }
    wprintf(L"LayoutTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
