#include "EngineLayout.h"
#include <stdio.h>
#include <vector>
#include <string.h>

HMODULE gInstance = nullptr;
uintptr_t gGameBase = 0;
bool EngineTestInitializeImage(uintptr_t virtualBase);
void EngineTestFailWrite(int index);

namespace {
int failures = 0;
void Check(bool condition, const char* name) {
    wprintf(L"%ls %hs\n", condition ? L"PASS" : L"FAIL", name);
    fflush(stdout);
    if (!condition) ++failures;
}
bool LoadImage(const wchar_t* path, std::vector<BYTE>& image, DWORD& base) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb")) return false;
    _fseeki64(file, 0, SEEK_END);
    const __int64 length = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    if (length < 256 || length > 64 * 1024 * 1024) { fclose(file); return false; }
    std::vector<BYTE> raw(static_cast<size_t>(length));
    const bool read = fread(raw.data(), 1, raw.size(), file) == raw.size();
    fclose(file);
    if (!read) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > raw.size()) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage > 64 * 1024 * 1024 ||
        nt->OptionalHeader.SizeOfHeaders > raw.size()) return false;
    image.resize(nt->OptionalHeader.SizeOfImage);
    memcpy(image.data(), raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        if (section.PointerToRawData + static_cast<size_t>(section.SizeOfRawData) > raw.size() ||
            section.VirtualAddress + static_cast<size_t>(section.SizeOfRawData) > image.size()) return false;
        memcpy(image.data() + section.VirtualAddress, raw.data() + section.PointerToRawData, section.SizeOfRawData);
    }
    base = nt->OptionalHeader.ImageBase;
    return true;
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
        const bool is65 = layout.profile == EngineProfileId::Ccz65;
        Check((is65 && layout.nativeSuper == 0x4250B3 && layout.nativeSuperState == 0x4CE7BF &&
            layout.waitWrapper == 0x42C4E0 && layout.timerContext == 0x4B1B18 && layout.stageUsesThis &&
            layout.sites[SiteMapText] == 0x4327EF && !layout.dialog3Applicable) ||
            (!is65 && layout.nativeSuper == 0x4250E6 && layout.nativeSuperState == 0x500ADB &&
            layout.waitWrapper == 0x42C4E5 && !layout.stageUsesThis && layout.dialog3Applicable),
            "identify independent profile, native owner and timer ABI");
        Check(layout.sites[SiteExtraDelay] == 0x415B40 && layout.sites[SiteWaitBranch] == 0x42C4EF &&
            layout.sites[SiteDialogDisplay] == 0x42C812 && layout.sites[SiteExtendedDisplay] == 0x455CE5 &&
            (is65 ? layout.sites[SiteDialog3] == 0 : layout.sites[SiteDialog3] == 0x42DB2C),
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
            CapabilityExtraDelay|CapabilityDialogDisplay|CapabilityExtendedDisplay|CapabilityNativeSuper|
            (is65 ? 0u : CapabilityDialog3);
        wprintf(L"capabilities=%08X expected=%08X callers=%u nativeCall=%08X\n", layout.capabilities, requiredCapabilities, static_cast<unsigned>(layout.waitCallerCount), static_cast<unsigned>(layout.nativeSuperCall));
        const bool complete = (layout.capabilities & requiredCapabilities) == requiredCapabilities;
        Check(complete, "all acceleration capabilities implemented by this profile pass semantic checks");
        // Never index a missing site after a failed resolver assertion.
        if (!complete) continue;
        bool safeCallers = layout.waitCallerCount == (is65 ? 16u : 17u);
        for (size_t i = 0; i < layout.waitCallerCount; ++i)
            safeCallers = safeCallers && layout.waitCallers[i] != layout.sites[SiteRWait] + 5;
        Check(safeCallers, "profile-specific S wait whitelist excludes the separately controlled R wait");
        const auto original = image;
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
        RestoreEngine();
        Check(stageApplied && image == original, "stage call and native modifier restore this base exactly");
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
            !(altered.capabilities & CapabilitySuperDelay),
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
            !(altered.capabilities & CapabilitySuperDelay),
            "unrecognized wait semantics disable only the affected feature");
    }
    wprintf(L"LayoutTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
