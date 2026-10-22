#include "ScenarioLifecycle.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {
int failures = 0;
struct Event { int ordinal; bool newCampaign; bool forceNewInstance; ULONGLONG now; };
std::vector<Event> events;
uintptr_t bridge = 0;

void Check(bool okay, const char* message) {
    printf("%s %s\n", okay ? "PASS" : "FAIL", message);
    if (!okay) ++failures;
}

bool ReadImage(const std::filesystem::path& path, std::vector<BYTE>& image) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") || !file) return false;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 256 || size > 64 * 1024 * 1024) { fclose(file); return false; }
    std::vector<BYTE> raw(static_cast<size_t>(size));
    const bool read = fread(raw.data(), 1, raw.size(), file) == raw.size();
    fclose(file);
    if (!read) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > raw.size()) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage > 128 * 1024 * 1024 ||
        nt->OptionalHeader.SizeOfHeaders > raw.size()) return false;
    image.assign(nt->OptionalHeader.SizeOfImage, 0);
    memcpy(image.data(), raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sections[i].PointerToRawData > raw.size() || sections[i].SizeOfRawData > raw.size() - sections[i].PointerToRawData ||
            sections[i].VirtualAddress > image.size() || sections[i].SizeOfRawData > image.size() - sections[i].VirtualAddress)
            return false;
        memcpy(image.data() + sections[i].VirtualAddress, raw.data() + sections[i].PointerToRawData, sections[i].SizeOfRawData);
    }
    return true;
}

void ValidateImage(const std::filesystem::path& path, bool engine65) {
    std::vector<BYTE> image;
    Check(ReadImage(path, image), "map the real native executable without executing it");
    if (image.empty()) return;
    ScenarioLifecycleImageLayout layout;
    constexpr uintptr_t base = 0x400000;
    const bool resolved = ResolveScenarioLifecycleForTest(image.data(), base, layout);
    Check(resolved, "resolve one verified scenario installer, ordinal accessor and all three caller causes");
    if (!resolved) return;
    Check(layout.nativeInstall == 0x417EEA && layout.ordinalAccessor == 0x43AD32 &&
        layout.ordinalAddress == 0x4B0776 && layout.installedSite == 0x417FDE &&
        layout.progressCall == 0x418141 && layout.initialCall == (engine65 ? 0x42BFE8u : 0x42BFD8u) &&
        layout.reloadCall == (engine65 ? 0x42C056u : 0x42C031u),
        "real-sample addresses agree with independently inspected native call graphs");
    ScenarioLifecycleImageLayout relocated;
    constexpr uintptr_t rebased = 0x17400000;
    Check(ResolveScenarioLifecycleForTest(image.data(), rebased, relocated) &&
        relocated.nativeInstall - rebased == layout.nativeInstall - base &&
        relocated.ordinalAddress - rebased == layout.ordinalAddress - base &&
        relocated.initialCall - rebased == layout.initialCall - base,
        "resolve the same graph at a different virtual base without fixed runtime addresses");
    auto changed = image;
    changed[layout.ordinalAccessor - base + 6] ^= 1;
    Check(!ResolveScenarioLifecycleForTest(changed.data(), base, relocated),
        "reject a changed authoritative ordinal accessor");
    changed = image;
    changed[layout.installedSite - base + 2] ^= 1;
    Check(!ResolveScenarioLifecycleForTest(changed.data(), base, relocated),
        "reject a changed success-result frame before installing any hook");
    changed = image;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(changed.data());
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(changed.data() + dos->e_lfanew);
    const auto sections = IMAGE_FIRST_SECTION(nt);
    DWORD duplicateRva = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (!duplicateRva && (sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
            duplicateRva = sections[i].VirtualAddress + sections[i].Misc.VirtualSize - 0x200;
    BYTE* duplicate = changed.data() + duplicateRva;
    const BYTE* original = image.data() + (layout.nativeInstall - base);
    memcpy(duplicate, original, 0x105);
    const int32_t duplicateCall = static_cast<int32_t>(layout.ordinalAccessor - (base + duplicateRva + 44));
    memcpy(duplicate + 40, &duplicateCall, sizeof(duplicateCall));
    Check(!ResolveScenarioLifecycleForTest(changed.data(), base, relocated),
        "reject an ambiguous second installer even when its accessor graph is valid");
}

void ValidateLifecycle() {
    WORD ordinal = 3;
    constexpr uintptr_t initial = 0x1005, progress = 0x2005, reload = 0x3005;
    DWORD caller[2] = {0, 0};
    const uintptr_t frame = reinterpret_cast<uintptr_t>(&caller[1]);
    ConfigureScenarioLifecycleForTest(&ordinal, initial, progress, reload);
    events.clear();
    Check(CurrentScenarioOrdinal() == 3 && !ScenarioLoadInProgress(), "read the native ordinal independently of resource filenames");
    CompleteScenarioInstallForTest(false, initial, frame, 1);
    Check(events.empty(), "failed native installation does not advance either ledger");
    CompleteScenarioInstallForTest(true, initial, frame, 2);
    Check(events.size() == 1 && events.back().newCampaign && !events.back().forceNewInstance,
        "confirmed main-menu new-game cause resets the campaign even for a repeated ordinal");
    events.clear();
    BeginScenarioLoad();
    Check(ScenarioLoadInProgress(), "load guard begins before native scene mutation");
    CompleteScenarioInstallForTest(true, reload, frame, 3);
    Check(events.empty(), "scene installation during native load preserves the saved ledgers");
    EndScenarioLoad(true);
    Check(!ScenarioLoadInProgress(), "load guard ends synchronously after restoration");
    CompleteScenarioInstallForTest(true, reload, frame, 4);
    Check(events.empty(), "one delayed installation of the loaded ordinal consumes the restoration token");
    CompleteScenarioInstallForTest(true, reload, frame, 5);
    Check(events.size() == 1 && events.back().forceNewInstance,
        "a later genuine same-S main-flow reinstall starts a fresh battle instance");
    events.clear();
    BeginScenarioLoad(); EndScenarioLoad(true);
    BeginScenarioLoad(); EndScenarioLoad(false);
    CompleteScenarioInstallForTest(true, reload, frame, 6);
    Check(events.empty(), "failed later load preserves the preceding successful load generation token");
    BeginScenarioLoad();
    BeginScenarioLoad(); EndScenarioLoad(true);
    Check(ScenarioLoadInProgress(), "nested completion keeps the outer native load guarded");
    EndScenarioLoad(false);
    CompleteScenarioInstallForTest(true, reload, frame, 6);
    Check(events.empty(), "failed outer load cannot invalidate a nested successful load generation");
    BeginScenarioLoad(); EndScenarioLoad(true);
    ordinal = 5;
    CompleteScenarioInstallForTest(true, progress, frame, 7);
    Check(events.size() == 1 && events.back().ordinal == 5 && events.back().forceNewInstance,
        "verified normal progression overrides a stale load token");
    events.clear();
    BeginScenarioLoad(); EndScenarioLoad(true);
    CompleteScenarioInstallForTest(true, initial, frame, 8);
    Check(events.size() == 1 && events.back().newCampaign,
        "confirmed new game overrides a preceding load token without guessing S00");
    events.clear();
    ordinal = 6;
    CompleteScenarioInstallForTest(true, progress, frame, 9);
    Check(events.size() == 1 && events.back().ordinal == 6,
        "R installation is reported by its even ordinal so the model can pause collection");
    ShutdownScenarioLifecycle();
    Check(CurrentScenarioOrdinal() == -1 && !ScenarioLoadInProgress(), "shutdown closes the lifecycle observation gate");
}

__declspec(naked) void NativeContinuation() {
    __asm {
        mov esp, ebp
        pop ebp
        ret
    }
}

__declspec(naked) void NativeInstallFrame() {
    __asm {
        push ebp
        mov ebp, esp
        sub esp, 14h
        mov dword ptr [ebp - 14h], 12340001h
        mov dword ptr [ebp - 0Ch], 23456789h
        mov edx, 3456789Ah
        stc
        jmp dword ptr [bridge]
    }
}

void ValidateBridge() {
    WORD ordinal = 7;
    ConfigureScenarioLifecycleForTest(&ordinal, 0x1005, 0x2005, 0x3005);
    bridge = ScenarioInstalledBridgeForTest(reinterpret_cast<uintptr_t>(&NativeContinuation));
    events.clear();
    uintptr_t before = 0, after = 0, resultEax = 0, resultEcx = 0, resultEdx = 0, flags = 0;
    __asm {
        mov before, esp
        call NativeInstallFrame
        pushfd
        pop flags
        mov resultEax, eax
        mov resultEcx, ecx
        mov resultEdx, edx
        mov after, esp
    }
    Check(before == after && resultEax == 0x12340001 && resultEcx == 0x23456789 &&
        resultEdx == 0x3456789A && (flags & 1) && events.size() == 1 && events[0].ordinal == 7,
        "execute the real x86 bridge and preserve native result, registers, carry flag and stack");
    ShutdownScenarioLifecycle();
}
} // namespace

void NotifyBattleStatsScenarioInstalled(int ordinal, bool newCampaign, bool forceNewInstance, ULONGLONG now) {
    events.push_back({ordinal, newCampaign, forceNewInstance, now});
}

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path root = std::filesystem::current_path();
    while (!root.empty() && !std::filesystem::exists(root / L"基底")) {
        const auto parent = root.parent_path();
        if (parent == root) break;
        root = parent;
    }
    ValidateImage(argc > 1 ? std::filesystem::path(argv[1]) : root / L"基底/加强版6.5未加密版/Ekd5.exe", true);
    ValidateImage(argc > 2 ? std::filesystem::path(argv[2]) : root / L"基底/复刻版6.6山贼 国庆/Ekd5.exe", false);
    ValidateLifecycle();
    ValidateBridge();
    printf("ScenarioLifecycleTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
