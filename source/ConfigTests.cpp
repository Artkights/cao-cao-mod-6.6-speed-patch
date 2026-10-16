#include "Proxy.cpp"
#include <richedit.h>
#include <string.h>

namespace {
std::wstring ColumnText(HWND list, int index) {
    wchar_t text[64] = {};
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT;
    column.pszText = text;
    column.cchTextMax = static_cast<int>(_countof(text));
    return SendMessageW(list, LVM_GETCOLUMNW, index, reinterpret_cast<LPARAM>(&column)) ? text : L"";
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
    GetCurrentDirectoryW(_countof(gDirectory), gDirectory);
    wcscat_s(gDirectory, L"\\config-test");
    CreateDirectoryW(gDirectory, nullptr);
    swprintf_s(gConfigPath, L"%s\\patch\\xiaoan66\\pl_06.ini", gDirectory);
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
    HWND guide = FindWindowW(L"XiaoAn66GuideWindow", L"简易攻略");
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
    HWND battle = FindWindowW(L"XiaoAn66BattleInfoWindow", L"战斗统计");
    HWND battleTabs = battle ? FindWindowExW(battle, nullptr, WC_TABCONTROLW, nullptr) : nullptr;
    HWND battleList = battle ? FindWindowExW(battle, nullptr, WC_LISTVIEWW, nullptr) : nullptr;
    HWND battleDetail = battle ? FindWindowExW(battle, nullptr, WC_EDITW, nullptr) : nullptr;
    check(battle && !battleTabs && battleList && !battleDetail && IsWindowUnicode(battleList) &&
        ColumnText(battleList, 0) == L"单位" && ColumnText(battleList, 1) == L"造成伤害" &&
        ColumnText(battleList, 2) == L"承受伤害" && ColumnText(battleList, 3) == L"回复量" &&
        ColumnText(battleList, 4) == L"暴击次数" && ColumnText(battleList, 5) == L"连击次数",
        "battle statistics exposes the six requested Unicode columns");
    CloseBattleInfo();
    if (owner) DestroyWindow(owner);
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
