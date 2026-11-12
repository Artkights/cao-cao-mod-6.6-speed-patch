#include "Proxy.cpp"
#include <richedit.h>
#include <string.h>
#include <filesystem>

namespace {
HANDLE gForwardResult = reinterpret_cast<HANDLE>(1234);
DWORD gForwardError = ERROR_ALREADY_EXISTS;
int gForwardCalls = 0;
bool gForwardArguments = false;
int gWideForwardCalls = 0;
bool gWideForwardArguments = false;
int gDispatchACalls = 0, gDispatchWCalls = 0, gTranslateCalls = 0;
bool gMessageForwardArguments = false;
constexpr DWORD kMessageForwardError = ERROR_INVALID_ACCESS;
thread_local bool gTestLoaderCallout = false;
BOOLEAN NTAPI TestLoaderCallout() { return gTestLoaderCallout ? TRUE : FALSE; }
struct StartupProbe {
    HANDLE entered = nullptr;
    HANDLE finished = nullptr;
    bool loaderCallout = false;
    int operation = 0;
    bool ready = false;
    HANDLE result = INVALID_HANDLE_VALUE;
    LRESULT messageResult = 0;
    DWORD lastError = 0;
};
DWORD WINAPI RunStartupProbe(void* parameter) {
    auto& probe = *static_cast<StartupProbe*>(parameter);
    gTestLoaderCallout = probe.loaderCallout;
    SetEvent(probe.entered);
    SetLastError(ERROR_BAD_FORMAT);
    if (probe.operation == 1)
        probe.result = PatchedCreateFileA("RS\\S_17.eex", GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    else if (probe.operation == 2) {
        MSG message = {};
        PatchedDispatch(&message);
    } else if (probe.operation == 3) {
        MSG message = {};
        probe.messageResult = PatchedTranslate(&message);
    } else if (probe.operation == 4) {
        probe.result = PatchedNativeCreateFileW(L"RS\\S_18.eex", GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    } else if (probe.operation == 5) {
        probe.result = PatchedNativeCreateFileA("RS\\R_19.eex", GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    } else if (probe.operation == 6) {
        MSG message = {};
        probe.messageResult = PatchedNativeDispatchA(&message);
    } else if (probe.operation == 7) {
        MSG message = {};
        probe.messageResult = PatchedNativeDispatchW(&message);
    } else probe.ready = AwaitStartup();
    probe.lastError = GetLastError();
    SetEvent(probe.finished);
    return 0;
}
HANDLE WINAPI TestCreateFileA(LPCSTR fileName, DWORD desiredAccess, DWORD shareMode,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile) {
    ++gForwardCalls;
    gForwardArguments = fileName && desiredAccess == GENERIC_READ && shareMode == FILE_SHARE_READ &&
        !security && disposition == OPEN_EXISTING && flags == FILE_ATTRIBUTE_NORMAL && !templateFile;
    SetLastError(gForwardError);
    return gForwardResult;
}
HANDLE WINAPI TestCreateFileW(LPCWSTR fileName, DWORD desiredAccess, DWORD shareMode,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile) {
    ++gWideForwardCalls;
    gWideForwardArguments = fileName && !wcscmp(fileName, L"RS\\S_18.eex") &&
        desiredAccess == GENERIC_READ && shareMode == FILE_SHARE_READ && !security &&
        disposition == OPEN_EXISTING && flags == FILE_ATTRIBUTE_NORMAL && !templateFile;
    SetLastError(gForwardError);
    return gForwardResult;
}
LRESULT WINAPI TestNativeDispatchA(const MSG* message) {
    ++gDispatchACalls;
    gMessageForwardArguments = message && message->message == 0 && !message->hwnd;
    SetLastError(kMessageForwardError);
    return 101;
}
LRESULT WINAPI TestNativeDispatchW(const MSG* message) {
    ++gDispatchWCalls;
    gMessageForwardArguments = message && message->message == 0 && !message->hwnd;
    SetLastError(kMessageForwardError);
    return 202;
}
BOOL WINAPI TestNativeTranslate(const MSG* message) {
    ++gTranslateCalls;
    gMessageForwardArguments = message && message->message == 0 && !message->hwnd;
    SetLastError(kMessageForwardError);
    return TRUE;
}
HANDLE OpenScenarioForTest(const char* path) {
    return PatchedCreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
}
std::wstring ColumnText(HWND list, int index) {
    wchar_t text[64] = {};
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT;
    column.pszText = text;
    column.cchTextMax = static_cast<int>(_countof(text));
    return SendMessageW(list, LVM_GETCOLUMNW, index, reinterpret_cast<LPARAM>(&column)) ? text : L"";
}
std::wstring TabText(HWND tabs, int index) {
    wchar_t text[64] = {};
    TCITEMW item = {};
    item.mask = TCIF_TEXT;
    item.pszText = text;
    item.cchTextMax = static_cast<int>(_countof(text));
    return SendMessageW(tabs, TCM_GETITEMW, index, reinterpret_cast<LPARAM>(&item)) ? text : L"";
}

std::wstring CellText(HWND list, int row, int column) {
    wchar_t text[128] = {};
    LVITEMW item = {};
    item.iSubItem = column;
    item.pszText = text;
    item.cchTextMax = static_cast<int>(_countof(text));
    SendMessageW(list, LVM_GETITEMTEXTW, row, reinterpret_cast<LPARAM>(&item));
    return text;
}
struct TestWindowSearch { const wchar_t* className; HWND result; };
BOOL CALLBACK FindTestWindowCallback(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<TestWindowSearch*>(parameter);
    wchar_t name[128] = {};
    GetClassNameW(window, name, static_cast<int>(_countof(name)));
    if (!wcscmp(name, search.className)) { search.result = window; return FALSE; }
    return TRUE;
}
HWND FindTestWindow(const wchar_t* className) {
    TestWindowSearch search = {className, nullptr};
    EnumThreadWindows(GetCurrentThreadId(), &FindTestWindowCallback, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

std::vector<unsigned char> ResourceGateScenario() {
    // One 0x7B branch-2 condition (person 0 belongs to our army), guarding
    // a real 0x38 ability change. Use the production parser and renderer to
    // observe whether the Proxy has authorized the condition decoder.
    const std::vector<unsigned char> condition = {
        0x7B, 0, 0, 0, 2, 0, 0, 0, 0, 0, '0', ',', '1', 0
    };
    const std::vector<unsigned char> action = {
        0x38, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 25, 0, 0, 0
    };
    std::vector<unsigned char> bytes = {
        'E', 'E', 'X', 0, 0, 0, 0, 0, 0, 0, 14, 0, 0, 0, 1, 0,
        static_cast<unsigned char>(condition.size() + action.size() + 4), 0
    };
    bytes.insert(bytes.end(), condition.begin(), condition.end());
    bytes.insert(bytes.end(), {0, 0, static_cast<unsigned char>(action.size()), 0});
    bytes.insert(bytes.end(), action.begin(), action.end());
    return bytes;
}

bool GuideConditionalExtensionMatches(bool enabled) {
    std::wstring text, diagnostics;
    if (!BuildGuideScriptTextForTest(ResourceGateScenario(), {L"曹操"}, text, diagnostics) ||
        text.find(L"曹操攻击力提升 25 点") == std::wstring::npos) return false;
    if (enabled) return text.find(L"曹操已加入我军") != std::wstring::npos &&
        text.find(L"尚未解析的扩展条件（0x7B）") == std::wstring::npos &&
        diagnostics.find(L"condition-unresolved") == std::wstring::npos;
    return text.find(L"尚未解析的扩展条件（0x7B）") != std::wstring::npos &&
        text.find(L"曹操已加入我军") == std::wstring::npos &&
        diagnostics.find(L"condition-unresolved: id=123") != std::wstring::npos;
}
}

bool InitializeEngine(bool) { return true; }
bool ApplyEngineState(const Settings&) { return true; }
void RestoreEngine() {}
void SetNativeToggleHandler(void (__cdecl*)()) {}
const wchar_t* EngineError() { return L"test"; }
int CurrentGamePhase() { return 0; }
void SyncConfig() {}
bool OpenConfig(HWND) { return true; }
bool HandleConfigMessage(MSG*) { return false; }

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    int value = 0;
    check(ParseLine("-128\r\n", value) && value == -128, "signed CRLF config parser");
    check(!ParseLine("1x", value) && !ParseLine("999999999999", value) && !ParseLine("\n", value),
        "reject malformed and overflowing numeric lines");
    std::wstring observedBefore, observed;
    GetObservedGuideScenario(observedBefore);
    gTestCreateFileA = &TestCreateFileA;
    check(OpenScenarioForTest("RS\\r_010.EEX") == gForwardResult &&
        GetLastError() == ERROR_ALREADY_EXISTS && gForwardCalls == 1 && gForwardArguments,
        "early passive scenario hook forwards the complete call and preserves success LastError");
    GetObservedGuideScenario(observed);
    check(observed == observedBefore && !strcmp(gPendingScenario, "r_010.EEX"),
        "unverified executable only buffers the successful ASCII scenario basename");
    OpenScenarioForTest("RS/S_11.eex");
    gForwardResult = INVALID_HANDLE_VALUE;
    gForwardError = ERROR_FILE_NOT_FOUND;
    check(OpenScenarioForTest("RS/R_12.eex") == INVALID_HANDLE_VALUE &&
        GetLastError() == ERROR_FILE_NOT_FOUND && !strcmp(gPendingScenario, "S_11.eex"),
        "failed scenario open preserves the pending name and original failure LastError");
    CompleteScenarioCapture(true);
    check(GetObservedGuideScenario(observed) && observed == L"S_11.eex" && !gPendingScenario[0],
        "verified profile replays the latest pending scenario before activation");
    gForwardResult = reinterpret_cast<HANDLE>(1234);
    OpenScenarioForTest("RS/R_13.eex");
    CompleteScenarioCapture(true);
    check(GetObservedGuideScenario(observed) && observed == L"R_13.eex",
        "new active scenario cannot be overwritten by a stale pending replay");
    OpenScenarioForTest("RS/R_1000.eex");
    OpenScenarioForTest("RS/S_14.eex.bak");
    check(GetObservedGuideScenario(observed) && observed == L"R_13.eex",
        "passive capture ignores invalid numeric ranges and non-scenario suffixes");
    gScenarioCaptureState = ScenarioCaptureState::Pending;
    OpenScenarioForTest("RS/S_15.eex");
    CompleteScenarioCapture(false);
    gForwardError = ERROR_SHARING_VIOLATION;
    const int previousCalls = gForwardCalls;
    check(OpenScenarioForTest("RS/R_16.eex") == gForwardResult &&
        GetLastError() == ERROR_SHARING_VIOLATION && gForwardCalls == previousCalls + 1 &&
        gForwardArguments && !gPendingScenario[0] && GetObservedGuideScenario(observed) &&
        observed == L"R_13.eex",
        "rejected unknown profile discards pending capture and only forwards later opens");
    gTestCreateFileA = nullptr;
    gNativeCreateFile = &TestCreateFileA;
    const int beforeNativeCalls = gForwardCalls;
    const HANDLE nativeResult = PatchedNativeCreateFileA("RS/R_16.eex", GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(nativeResult == gForwardResult && gForwardCalls == beforeNativeCalls + 1 &&
        gForwardArguments && GetLastError() == gForwardError,
        "protected file hook forwards through the preserved VFS target with complete arguments and LastError");
    gNativeCreateFile = nullptr;
    gStartupThread = GetCurrentThreadId();
    gWithinLoaderCallout = &TestLoaderCallout;
    auto resetStartup = [&]() {
        if (gStartupEvent) CloseHandle(gStartupEvent);
        gStartupEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        InterlockedExchange(&gStartupFinished, FALSE);
        InterlockedExchange(&gStartupAccepted, FALSE);
    };
    auto probeStartup = [&](int operation, bool loaderCallout, bool accepted) {
        StartupProbe probe;
        probe.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        probe.finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        probe.operation = operation;
        probe.loaderCallout = loaderCallout;
        HANDLE thread = CreateThread(nullptr, 0, &RunStartupProbe, &probe, 0, nullptr);
        check(thread && WaitForSingleObject(probe.entered, 3000) == WAIT_OBJECT_0,
            "start an independent game-thread startup probe");
        const bool done = WaitForSingleObject(probe.finished, 3000) == WAIT_OBJECT_0;
        check(done, loaderCallout ? "loader callbacks bypass startup wait without deadlocking the loader" :
            "pending startup forwards immediately without waiting for unpacking or worker completion");
        if (!loaderCallout) {
            check(!InterlockedCompareExchange(&gStartupFinished, FALSE, FALSE),
                "game-thread request completed while initialization was still pending");
            InterlockedExchange(&gStartupAccepted, accepted ? TRUE : FALSE);
            CompleteStartup();
        }
        if (thread) {
            WaitForSingleObject(thread, 3000);
            CloseHandle(thread);
        }
        CloseHandle(probe.entered);
        CloseHandle(probe.finished);
        return probe;
    };
    resetStartup();
    SetLastError(ERROR_BAD_FORMAT);
    check(!AwaitStartup() && GetLastError() == ERROR_BAD_FORMAT,
        "initialization worker never waits for its own startup event and preserves LastError");
    StartupProbe startup = probeStartup(0, false, true);
    check(!startup.ready && startup.lastError == ERROR_BAD_FORMAT && StartupUiAvailable(),
        "pending startup reports unready transparently and enables UI only after worker completion");
    resetStartup();
    gTestCreateFileA = &TestCreateFileA;
    gForwardError = ERROR_SHARING_VIOLATION;
    startup = probeStartup(1, false, true);
    check(startup.result == gForwardResult && startup.lastError == gForwardError,
        "pending file wrapper forwards the original request and LastError before initialization");
    gTestCreateFileA = nullptr;
    resetStartup();
    startup = probeStartup(0, true, false);
    check(!startup.ready && startup.lastError == ERROR_BAD_FORMAT,
        "unready loader callback remains a transparent call instead of claiming initialized state");
    resetStartup();
    probeStartup(2, false, false);
    check(!StartupUiAvailable(), "failed initialization forwards dispatch without activating patch windows");
    resetStartup();
    probeStartup(3, false, true);
    check(StartupUiAvailable(), "input translation stays transparent until successful initialization");

    resetStartup();
    gNativeCreateFile = &TestCreateFileA;
    const int nativeBeforePending = gForwardCalls;
    startup = probeStartup(5, false, false);
    check(startup.result == gForwardResult && startup.lastError == gForwardError &&
        gForwardCalls == nativeBeforePending + 1 && gForwardArguments,
        "pending native ANSI file call preserves its VFS target, arguments and LastError");
    gNativeCreateFile = nullptr;
    resetStartup();
    gNativeCreateFileW = &TestCreateFileW;
    startup = probeStartup(4, false, false);
    check(startup.result == gForwardResult && startup.lastError == gForwardError &&
        gWideForwardCalls == 1 && gWideForwardArguments,
        "pending native Unicode file call preserves its VFS target, arguments and LastError");
    gNativeCreateFileW = nullptr;

    resetStartup();
    gNativeDispatchA = &TestNativeDispatchA;
    startup = probeStartup(6, false, false);
    check(startup.messageResult == 101 && startup.lastError == kMessageForwardError &&
        gDispatchACalls == 1 && gMessageForwardArguments,
        "pending ANSI message call preserves its original target, result and LastError");
    gNativeDispatchA = nullptr;
    resetStartup();
    gNativeDispatchW = &TestNativeDispatchW;
    startup = probeStartup(7, false, false);
    check(startup.messageResult == 202 && startup.lastError == kMessageForwardError &&
        gDispatchWCalls == 1 && gMessageForwardArguments,
        "pending Unicode message call preserves its original target, result and LastError");
    gNativeDispatchW = nullptr;
    resetStartup();
    gNativeTranslate = &TestNativeTranslate;
    startup = probeStartup(3, false, false);
    check(startup.messageResult == TRUE && startup.lastError == kMessageForwardError &&
        gTranslateCalls == 1 && gMessageForwardArguments,
        "pending translation preserves its original target, result and LastError");
    gNativeTranslate = nullptr;
    BYTE* gateImage = static_cast<BYTE*>(VirtualAlloc(nullptr, 0x1000,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(gateImage != nullptr, "allocate an isolated live import slot for restoration regression");
    if (gateImage) {
        gGameBase = reinterpret_cast<uintptr_t>(gateImage);
        gRuntimeImageSize = 0x1000;
        gRuntimeNativePlan = RuntimeNativeImportPlan();
        auto& gateSite = gRuntimeNativePlan.sites[static_cast<size_t>(RuntimeNativeImportKind::CreateFileA)];
        gateSite.slotOffset = 0x100;
        gateSite.original = reinterpret_cast<void*>(&TestCreateFileA);
        gateSite.replacement = reinterpret_cast<void*>(&PatchedNativeCreateFileA);
        gateSite.protection = PAGE_READWRITE;
        gateSite.present = gateSite.committed = true;
        *reinterpret_cast<void**>(gateImage + gateSite.slotOffset) = gateSite.replacement;
        gRuntimeNativeGate = true;
        gNativeCreateFile = &TestCreateFileA;
        gNativeCreateFileW = &TestCreateFileW;
        gNativeDispatchA = &TestNativeDispatchA;
        gNativeDispatchW = &TestNativeDispatchW;
        gNativeTranslate = &TestNativeTranslate;
        RestoreProtectedGate();
        const int beforeRestoredForward = gForwardCalls;
        const HANDLE restoredForward = PatchedNativeCreateFileA("RS/R_16.eex", GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(!gRuntimeNativeGate && !gateSite.committed &&
            *reinterpret_cast<void**>(gateImage + gateSite.slotOffset) == gateSite.original &&
            gNativeCreateFileW == &TestCreateFileW && gNativeDispatchA == &TestNativeDispatchA &&
            gNativeDispatchW == &TestNativeDispatchW && gNativeTranslate == &TestNativeTranslate &&
            restoredForward == gForwardResult && gForwardCalls == beforeRestoredForward + 1 &&
            GetLastError() == gForwardError,
            "restored import slots retain original forwarding targets for wrappers already in flight");
        VirtualFree(gateImage, 0, MEM_RELEASE);
        gGameBase = 0;
        gRuntimeImageSize = 0;
        gRuntimeNativePlan = RuntimeNativeImportPlan();
        gNativeCreateFile = nullptr;
        gNativeCreateFileW = nullptr;
        gNativeDispatchA = nullptr;
        gNativeDispatchW = nullptr;
        gNativeTranslate = nullptr;
    }
    CloseHandle(gStartupEvent);
    gStartupEvent = nullptr;
    gWithinLoaderCallout = nullptr;
    gStartupThread = 0;
    InterlockedExchange(&gStartupFinished, FALSE);
    InterlockedExchange(&gStartupAccepted, FALSE);
    wchar_t temporary[32768] = {};
    GetTempPathW(static_cast<DWORD>(_countof(temporary)), temporary);
    swprintf_s(gDirectory, L"%sCczConfigStartupTests-%u-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    std::filesystem::create_directories(gDirectory);
    swprintf_s(gConfigPath, L"%s\\pl_06.ini", PatchPrivatePath(gDirectory, CompiledEngineProfile()).c_str());
    check(LoadInstalledFeatures(gDirectory, CompiledEngineProfile()) &&
        gInstalledFeatures == XiaoAnInstall::FeatureAll,
        "a historical installation without feature-state retains all functions");
    const std::wstring featureState = PatchPrivatePath(gDirectory, CompiledEngineProfile()) + L"\\feature-state.ini";
    std::filesystem::create_directories(PatchPrivatePath(gDirectory, CompiledEngineProfile()));
    check(WritePrivateProfileStringW(L"Install", L"SchemaVersion", L"1", featureState.c_str()) &&
        WritePrivateProfileStringW(L"Install", L"FeatureMask", L"2", featureState.c_str()) &&
        LoadInstalledFeatures(gDirectory, CompiledEngineProfile()) &&
        HasFeature(XiaoAnInstall::FeatureGuide) && !HasFeature(XiaoAnInstall::FeatureAccelerator) &&
        ResolveKeyCommand(VK_TAB, 0) == -1,
        "a guide-only installation keeps accelerator shortcuts inactive");
    struct FeatureMode {
        unsigned mask;
        const wchar_t* state;
        bool resources;
        bool conditional;
        bool accelerator;
        const char* name;
    };
    const FeatureMode featureModes[] = {
        {XiaoAnInstall::FeatureAccelerator, L"1", false, false, true,
            "accelerator-only mode disables resources and conditions while retaining accelerator shortcuts"},
        {XiaoAnInstall::FeatureGuide, L"2", true, true, false,
            "guide-only mode retries resources and enables audited conditions without accelerator shortcuts"},
        {XiaoAnInstall::FeatureBattleStats, L"4", true, false, false,
            "statistics-only mode retries resources without granting guide conditions or accelerator shortcuts"},
        {XiaoAnInstall::FeatureAll, L"7", true, true, true,
            "all-features mode retries resources and independently enables audited conditions and accelerator shortcuts"}
    };
    for (const auto& mode : featureModes) {
        const bool loaded = WritePrivateProfileStringW(L"Install", L"FeatureMask", mode.state, featureState.c_str()) &&
            LoadInstalledFeatures(gDirectory, CompiledEngineProfile());
        // This harness has no game getter code: initialization genuinely
        // fails, exercising the same delayed-layout path as a packed game.
        const bool layoutReady = InitializeRuntimeResourceProviders(true, EngineProfileId::Ccz65);
        BattleResourceSnapshot pending;
        const bool contentReady = ReadBattleResourceSnapshot(pending);
        check(loaded && gInstalledFeatures == mode.mask && !layoutReady && !contentReady &&
            !pending.runtimeProfileReady && BattleRuntimeResourceNamesEnabled() == mode.resources &&
            GuideConditionalExtensionMatches(mode.conditional) &&
            ResolveKeyCommand(VK_TAB, 0) == (mode.accelerator ? static_cast<int>(CommandOpenConfig) : -1), mode.name);
    }
    check(!InitializeRuntimeResourceProviders(false, EngineProfileId::Ccz65) &&
        BattleRuntimeResourceNamesEnabled() && GuideConditionalExtensionMatches(false),
        "an unaudited 6.5 image retains resource discovery without enabling 0x7B");
    // Publish valid resource tables after the failed initialization. This
    // existing runtime fixture models delayed table availability; it does
    // not change the production Proxy's condition authorization.
    std::vector<BYTE> gatePeople(1024 * 0x48), gateItems(255 * 25);
    for (unsigned i = 0; i < 1024; ++i) {
        const uint16_t tag = static_cast<uint16_t>(0x1000 + i);
        memcpy(gatePeople.data() + i * 0x48, &tag, sizeof(tag));
    }
    const BYTE gatePersonName[] = {0xB2, 0xDC, 0xB2, 0xD9}; // 曹操 (CP936)
    const BYTE gateItemName[] = {0xB6, 0xCC, 0xBD, 0xA3}; // 短剑 (CP936)
    memcpy(gatePeople.data() + 8, gatePersonName, sizeof(gatePersonName));
    memcpy(gateItems.data(), gateItemName, sizeof(gateItemName));
    uintptr_t gatePersonPointer = reinterpret_cast<uintptr_t>(gatePeople.data());
    ConfigureBattleResourceLayoutForTest(reinterpret_cast<uintptr_t>(&gatePersonPointer),
        reinterpret_cast<uintptr_t>(gateItems.data()));
    BattleResourceSnapshot loadedResources;
    check(ReadBattleResourceSnapshot(loadedResources) && loadedResources.peopleReady &&
        loadedResources.itemsReady && loadedResources.people[0] == L"曹操" &&
        loadedResources.items[0] == L"短剑" && GuideConditionalExtensionMatches(false),
        "ready runtime resources do not authorize an unaudited condition extension");
    for (const auto profile : {EngineProfileId::Ccz64, EngineProfileId::Ccz66, EngineProfileId::Unknown}) {
        check(!InitializeRuntimeResourceProviders(true, profile) && BattleRuntimeResourceNamesEnabled() &&
            GuideConditionalExtensionMatches(false),
            "a non-6.5 profile cannot inherit Protected220 condition authorization");
    }
    check(!InitializeRuntimeResourceProviders(true, EngineProfileId::Ccz65) &&
        BattleRuntimeResourceNamesEnabled() && GuideConditionalExtensionMatches(true),
        "audited Protected220 conditions recover independently while resource layout remains pending");
    DisableRuntimeResourceProviders();
    check(!BattleRuntimeResourceNamesEnabled() && GuideConditionalExtensionMatches(false),
        "provider shutdown revokes condition authorization and resource retries together");
    check(WritePrivateProfileStringW(L"Install", L"FeatureMask", L"999", featureState.c_str()) &&
        !LoadInstalledFeatures(gDirectory, CompiledEngineProfile()) &&
        gInstalledFeatures == XiaoAnInstall::FeatureNone,
        "a damaged feature-state disables optional functions");
    DeleteFileW(featureState.c_str());
    gInstalledFeatures = XiaoAnInstall::FeatureAll;
    gSettings = Settings();
    gConfigLines[30] = 77;
    gSettings.superEnabled = true;
    gSettings.phaseEnabled = true;
    gSettings.gearEnabled = true;
    gSettings.gearRaw = -10;
    gSettings.phase[0] = 50;
    gSettings.phaseFlags[1] = false;
    gSettings.shiftKey = false;
    gSettings.hotkeys[0] = MAKEWORD('A', HOTKEYF_CONTROL | HOTKEYF_SHIFT);
    check(SaveSettings(), "atomic 74-line settings save");
    FILE* file = nullptr;
    _wfopen_s(&file, gConfigPath, L"rb");
    char line[128]; int count = 0;
    while (file && fgets(line, sizeof(line), file)) ++count;
    if (file) fclose(file);
    check(count == 74, "saved config contains exactly 74 numeric lines");
    gSettings = Settings();
    LoadSettings();
    check(!gSettings.superEnabled && !gSettings.phaseEnabled, "skip modes forced off on restart");
    check(gSettings.gearEnabled && gSettings.gearRaw == -10, "gear restores enabled signed speed");
    check(gSettings.phase[0] == 50 && !gSettings.phaseFlags[1] && !gSettings.shiftKey,
        "phase selections and native key choice persist");
    check(gSettings.hotkeys[0] == MAKEWORD('A', HOTKEYF_CONTROL | HOTKEYF_SHIFT),
        "modifier bits persist in legacy hotkey fields");
    gCompatible = true;
    check(ResolveKeyCommand('A', HOTKEYF_CONTROL | HOTKEYF_SHIFT) == CommandSuper &&
        ResolveKeyCommand('A', HOTKEYF_CONTROL) == -1,
        "hotkey dispatch requires an exact modifier match");
    gSettings.hotkeys[1] = gSettings.hotkeys[0];
    check(ResolveKeyCommand('A', HOTKEYF_CONTROL | HOTKEYF_SHIFT) == CommandSuper,
        "duplicate hotkeys preserve the legacy first-command priority");
    ReleaseKey('Z');
    check(LatchKey('Z') && !LatchKey('Z') && ReleaseKey('Z') && LatchKey('Z'),
        "keydown latch suppresses hook/native duplicates until keyup");
    ReleaseKey('Z');
    check(gConfigLines[30] == 77, "unrelated numeric fields preserved without enabling functionality");
    gConfigLines[11] = -1;
    gConfigLines[14] = 0;
    gConfigLines[16] = 999;
    _wfopen_s(&file, gConfigPath, L"wb");
    for (int n : gConfigLines) fprintf(file, "%d\r\n", n);
    fclose(file);
    gSettings = Settings(); LoadSettings();
    check(gSettings.phase[0] == 60 && gSettings.wheelStep == 5 && gSettings.gearRaw == 0,
        "invalid phase, step and gear values revert to compatible defaults");
    check(GearStep(3, 1) == 6 && GearStep(3, -1) == 1 && GearStep(-3, 1) == -1 &&
        GearStep(-3, -1) == -6 && GearStep(0, 1) == 1 && GearStep(0, -1) == -1,
        "old non-power-of-two and sign crossing step semantics");
    check(GearPosition(3) == 10 && GearPosition(-3) == 5 && GearValue(0) == -128 &&
        GearValue(16) == 128 && GearPosition(0) == 8, "old asymmetric slider mapping");

    gInstance = GetModuleHandleW(nullptr);
    const INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    HWND owner = CreateWindowExW(0, L"STATIC", L"test owner", WS_OVERLAPPEDWINDOW,
        0, 0, 640, 480, nullptr, nullptr, gInstance, nullptr);
    check(owner != nullptr, "create Unicode UI test owner");
    check(OpenGuide(owner, gDirectory), "open the modeless guide window for Unicode verification");
    HWND guide = FindTestWindow(L"XiaoAn66GuideWindow");
    HWND guideTabs = guide ? FindWindowExW(guide, nullptr, WC_TABCONTROLW, nullptr) : nullptr;
    HWND guideMap = guide ? FindWindowExW(guide, nullptr, L"XiaoAn66GuideMap", nullptr) : nullptr;
    HWND firstCombo = guide ? FindWindowExW(guide, nullptr, WC_COMBOBOXW, nullptr) : nullptr;
    HWND secondCombo = firstCombo ? FindWindowExW(guide, firstCombo, WC_COMBOBOXW, nullptr) : nullptr;
    HWND firstEdit = guide ? FindWindowExW(guide, nullptr, WC_EDITW, nullptr) : nullptr;
    HWND secondEdit = firstEdit ? FindWindowExW(guide, firstEdit, WC_EDITW, nullptr) : nullptr;
    HWND reload = guide ? FindWindowExW(guide, nullptr, WC_BUTTONW, L"重新读取") : nullptr;
    HWND exportButton = guide ? FindWindowExW(guide, nullptr, WC_BUTTONW, L"导出攻略") : nullptr;
    HWND guideText = guide ? FindWindowExW(guide, nullptr, RICHEDIT_CLASSW, nullptr) : nullptr;
    check(guide && !guideTabs && !guideMap && firstCombo && !secondCombo && firstEdit && secondEdit &&
        !reload && exportButton && guideText && IsWindowUnicode(firstCombo) && IsWindowUnicode(firstEdit) &&
        IsWindowUnicode(secondEdit) && IsWindowUnicode(guideText),
        "guide exposes one scenario selector and native display parameter inputs");
    CloseGuide();

    check(OpenBattleInfo(owner), "open the modeless battle statistics window for Unicode verification");
    HWND battle = FindTestWindow(L"XiaoAn66BattleInfoWindow");
    HWND battleTabs = battle ? FindWindowExW(battle, nullptr, WC_TABCONTROLW, nullptr) : nullptr;
    HWND battleList = battle ? FindWindowExW(battle, nullptr, WC_LISTVIEWW, nullptr) : nullptr;
    HWND battleDetail = battle ? FindWindowExW(battle, nullptr, WC_EDITW, nullptr) : nullptr;
    check(battle && battleTabs && battleList && !battleDetail && IsWindowUnicode(battleList) &&
        TabText(battleTabs, 0) == L"当前战场" && TabText(battleTabs, 1) == L"全局统计" &&
        ColumnText(battleList, 0) == L"单位" && ColumnText(battleList, 1) == L"造成伤害" &&
        ColumnText(battleList, 2) == L"承受伤害" && ColumnText(battleList, 3) == L"回复量" &&
        ColumnText(battleList, 4) == L"暴击次数" && ColumnText(battleList, 5) == L"连击次数" &&
        ColumnText(battleList, 6) == L"攻击闪避" && ColumnText(battleList, 7) == L"策略闪避" &&
        ColumnText(battleList, 8) == L"物理受击" && ColumnText(battleList, 9) == L"策略受击",
        "battle statistics exposes current and campaign tabs with ten Unicode columns");
    BattleStatsPersistedState historical;
    // This fixture models a pre-roster production save.  Schema 3 migration
    // intentionally reconstructs the legacy campaign rows and marks the
    // inferred membership as incomplete; a schema-4 state must carry its
    // explicit roster and participant sets.
    historical.schemaVersion = 3;
    historical.scenario = L"S_07.eex";
    historical.active = true;
    historical.scenarioOrdinal = 7;
    historical.knownMetrics = BattleStatsDamageDealt;
    BattleUnitTotals person;
    person.personId = 42;
    person.slot = 3;
    person.label = L"历史人物";
    person.damageDealt = 123;
    historical.units.push_back(person);
    person.damageDealt = 456;
    historical.campaignUnits.push_back(person);
    person.personId = 65;
    person.slot = -1;
    person.label = L"未出战人物";
    person.damageDealt = 789;
    historical.campaignUnits.push_back(person);
    ShutdownBattleStats();
    check(RestoreBattleStatsState(historical, GetTickCount64()), "restore history with collection hooks unavailable");
    SendMessageW(battle, WM_TIMER, 0x6810, 0);
    check(ListView_GetItemCount(battleList) == 1 && CellText(battleList, 0, 1) == L"123",
        "current tab shows saved history even when collection hooks are unavailable");
    TabCtrl_SetCurSel(battleTabs, 1);
    NMHDR changed = {};
    changed.hwndFrom = battleTabs;
    changed.idFrom = 0x6813;
    changed.code = TCN_SELCHANGE;
    SendMessageW(battle, WM_NOTIFY, changed.idFrom, reinterpret_cast<LPARAM>(&changed));
    check(ListView_GetItemCount(battleList) == 2 && CellText(battleList, 0, 1) == L"456" &&
        CellText(battleList, 1, 0) == L"未出战人物" && CellText(battleList, 1, 1) == L"789",
        "campaign tab includes saved people outside the current deployment with their own totals");
    CloseBattleInfo();
    if (owner) DestroyWindow(owner);
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
