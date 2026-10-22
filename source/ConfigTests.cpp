#include "Proxy.cpp"
#include <richedit.h>
#include <string.h>
#include <filesystem>

namespace {
HANDLE gForwardResult = reinterpret_cast<HANDLE>(1234);
DWORD gForwardError = ERROR_ALREADY_EXISTS;
int gForwardCalls = 0;
bool gForwardArguments = false;
thread_local bool gTestLoaderCallout = false;
BOOLEAN NTAPI TestLoaderCallout() { return gTestLoaderCallout ? TRUE : FALSE; }
struct StartupProbe {
    HANDLE entered = nullptr;
    HANDLE finished = nullptr;
    bool loaderCallout = false;
    int operation = 0;
    bool ready = false;
    HANDLE result = INVALID_HANDLE_VALUE;
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
        PatchedTranslate(&message);
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
}

bool InitializeEngine() { return true; }
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
        if (!loaderCallout) {
            check(WaitForSingleObject(probe.finished, 30) == WAIT_TIMEOUT,
                "hold game file, dispatch, or translation until model and save hooks are ready");
            InterlockedExchange(&gStartupAccepted, accepted ? TRUE : FALSE);
            CompleteStartup();
        }
        const bool done = WaitForSingleObject(probe.finished, 3000) == WAIT_OBJECT_0;
        check(done, loaderCallout ? "loader callbacks bypass startup wait without deadlocking the loader" :
            "completion releases the waiting game thread, including initialization failures");
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
    check(startup.ready && startup.lastError == ERROR_BAD_FORMAT && StartupUiAvailable(),
        "successful startup wait preserves the original LastError and enables patch UI");
    resetStartup();
    gTestCreateFileA = &TestCreateFileA;
    gForwardError = ERROR_SHARING_VIOLATION;
    startup = probeStartup(1, false, true);
    check(startup.result == gForwardResult && startup.lastError == gForwardError,
        "file wrapper resumes the original request and LastError after initialization");
    gTestCreateFileA = nullptr;
    resetStartup();
    startup = probeStartup(0, true, false);
    check(!startup.ready && startup.lastError == ERROR_BAD_FORMAT,
        "unready loader callback remains a transparent call instead of claiming initialized state");
    resetStartup();
    probeStartup(2, false, false);
    check(!StartupUiAvailable(), "failed initialization releases dispatch without activating patch windows");
    resetStartup();
    probeStartup(3, false, true);
    check(StartupUiAvailable(), "the first input translation is gated until successful initialization");
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
    HWND reload = guide ? FindWindowExW(guide, nullptr, WC_BUTTONW, L"重新读取") : nullptr;
    HWND guideText = guide ? FindWindowExW(guide, nullptr, RICHEDIT_CLASSW, nullptr) : nullptr;
    check(guide && !guideTabs && !guideMap && firstCombo && secondCombo && reload && guideText &&
        IsWindowUnicode(firstCombo) && IsWindowUnicode(secondCombo) && IsWindowUnicode(guideText),
        "guide directly exposes the Unicode text view without map or view tabs");
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
        ColumnText(battleList, 6) == L"攻击格挡" && ColumnText(battleList, 7) == L"法术格挡",
        "battle statistics exposes current and campaign tabs with eight Unicode columns");
    BattleStatsPersistedState historical;
    historical.scenario = L"S_07.eex";
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
