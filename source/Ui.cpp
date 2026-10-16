#include "Patch.h"
#include "ResourceIds.h"
#include "UiIds.h"
#include <wchar.h>
#include <stdlib.h>

namespace {
bool synchronizing = false;
HWND tooltip = nullptr;
int editingNumeric = 0;
constexpr UINT_PTR kHotkeySubclass = 0xCC660703;

const int speedControls[] = {
    IDC_GROUP_SUPER, IDC_GROUP_PHASE, IDC_GROUP_GEAR,
    IDC_SUPER, IDC_SUPER_DIALOG, IDC_SUPER_MAPTEXT, IDC_SUPER_DELAY, IDC_SUPER_R, IDC_SUPER_S,
    IDC_PHASE, IDC_PHASE_OWN, IDC_PHASE_FRIEND, IDC_PHASE_ENEMY, IDC_WHEEL,
    IDC_PHASE_VALUE_OWN, IDC_PHASE_VALUE_FRIEND, IDC_PHASE_VALUE_ENEMY, IDC_WHEEL_STEP,
    IDC_SPIN_OWN, IDC_SPIN_FRIEND, IDC_SPIN_ENEMY, IDC_SPIN_WHEEL,
    IDC_GEAR, IDC_GEAR_SLIDER, IDC_GEAR_MINUS, IDC_GEAR_VALUE, IDC_GEAR_PLUS, IDC_SHIFT
};
const int hotkeyControls[] = {
    IDC_LABEL_HOTKEY_SUPER, IDC_LABEL_HOTKEY_PHASE, IDC_LABEL_HOTKEY_GEAR,
    IDC_LABEL_HOTKEY_PLUS, IDC_LABEL_HOTKEY_MINUS,
    IDC_HOTKEY_SUPER, IDC_HOTKEY_PHASE, IDC_HOTKEY_GEAR, IDC_HOTKEY_PLUS, IDC_HOTKEY_MINUS
};

bool NumericField(int id, int*& value, int& low, int& high) {
    low = 0;
    high = 200;
    if (id >= IDC_PHASE_VALUE_OWN && id <= IDC_PHASE_VALUE_ENEMY) {
        value = &gSettings.phase[id - IDC_PHASE_VALUE_OWN];
        return true;
    }
    if (id == IDC_WHEEL_STEP) {
        value = &gSettings.wheelStep;
        low = 1;
        return true;
    }
    if (id == IDC_GEAR_VALUE) {
        value = &gSettings.gearRaw;
        low = -128;
        high = 128;
        return true;
    }
    return false;
}

bool ReadInteger(int id, int low, int high, int& result) {
    wchar_t text[32];
    GetDlgItemTextW(gDialog, id, text, ARRAYSIZE(text));
    if (!text[0]) return false;
    const wchar_t* digit = text;
    if (*digit == L'-' || *digit == L'+') ++digit;
    if (!*digit) return false;
    for (; *digit; ++digit) {
        if (*digit < L'0' || *digit > L'9') return false;
    }
    wchar_t* end = nullptr;
    const long parsed = wcstol(text, &end, 10);
    if (*end || parsed < low || parsed > high) return false;
    result = static_cast<int>(parsed);
    return true;
}

void WriteInteger(int id, int value) {
    wchar_t text[32];
    _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, L"%d", value);
    wchar_t current[32];
    GetDlgItemTextW(gDialog, id, current, ARRAYSIZE(current));
    if (wcscmp(text, current)) SetDlgItemTextW(gDialog, id, text);
}

void UpdateNumeric(int id, bool finalize) {
    int* value = nullptr;
    int low, high, parsed;
    if (!NumericField(id, value, low, high)) return;
    if (ReadInteger(id, low, high, parsed) && parsed != *value) {
        *value = parsed;
        editingNumeric = id;
        CommitSettings();
        editingNumeric = 0;
    }
    if (finalize) {
        synchronizing = true;
        WriteInteger(id, *value);
        synchronizing = false;
    }
}

void FinalizeNumbers() {
    const int ids[] = {IDC_PHASE_VALUE_OWN, IDC_PHASE_VALUE_FRIEND,
        IDC_PHASE_VALUE_ENEMY, IDC_WHEEL_STEP, IDC_GEAR_VALUE};
    for (int id : ids) UpdateNumeric(id, true);
}

void SelectPage(int page) {
    for (int id : speedControls) ShowWindow(GetDlgItem(gDialog, id), page == 0 ? SW_SHOW : SW_HIDE);
    for (int id : hotkeyControls) ShowWindow(GetDlgItem(gDialog, id), page == 1 ? SW_SHOW : SW_HIDE);
    InvalidateRect(gDialog, nullptr, TRUE);
}

void AddTooltip(int id, const wchar_t* text) {
    TOOLINFOW tool = {};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = gDialog;
    tool.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(gDialog, id));
    tool.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

LRESULT CALLBACK HotkeyProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR index) {
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, HotkeyProcedure, kHotkeySubclass);
        return DefSubclassProc(window, message, wParam, lParam);
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_KILLFOCUS && !LOBYTE(SendMessageW(window, HKM_GETHOTKEY, 0, 0))) {
        synchronizing = true;
        SendMessageW(window, HKM_SETHOTKEY, gSettings.hotkeys[index], 0);
        synchronizing = false;
    }
    return result;
}

void SetUpControls() {
    HWND tabs = GetDlgItem(gDialog, IDC_PAGE_TAB);
    TCITEMW item = {};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(L"加速配置");
    SendMessageW(tabs, TCM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    item.pszText = const_cast<wchar_t*>(L"快捷键");
    SendMessageW(tabs, TCM_INSERTITEMW, 1, reinterpret_cast<LPARAM>(&item));
    const int edits[] = {IDC_PHASE_VALUE_OWN, IDC_PHASE_VALUE_FRIEND, IDC_PHASE_VALUE_ENEMY, IDC_WHEEL_STEP};
    const int spins[] = {IDC_SPIN_OWN, IDC_SPIN_FRIEND, IDC_SPIN_ENEMY, IDC_SPIN_WHEEL};
    synchronizing = true;
    for (int i = 0; i < 4; ++i) {
        SendDlgItemMessageW(gDialog, edits[i], EM_SETLIMITTEXT, 3, 0);
        SendDlgItemMessageW(gDialog, spins[i], UDM_SETBUDDY,
            reinterpret_cast<WPARAM>(GetDlgItem(gDialog, edits[i])), 0);
        SendDlgItemMessageW(gDialog, spins[i], UDM_SETRANGE32, i == 3 ? 1 : 0, 200);
    }
    SendDlgItemMessageW(gDialog, IDC_GEAR_VALUE, EM_SETLIMITTEXT, 4, 0);
    SendDlgItemMessageW(gDialog, IDC_GEAR_SLIDER, TBM_SETRANGE, FALSE, MAKELPARAM(0, 16));
    SendDlgItemMessageW(gDialog, IDC_GEAR_SLIDER, TBM_SETTICFREQ, 1, 0);
    SendDlgItemMessageW(gDialog, IDC_GEAR_SLIDER, TBM_SETPAGESIZE, 0, 1);
    for (int i = 0; i < 5; ++i) {
        SendDlgItemMessageW(gDialog, IDC_HOTKEY_SUPER + i, HKM_SETRULES, 0, 0);
        SetWindowSubclass(GetDlgItem(gDialog, IDC_HOTKEY_SUPER + i), HotkeyProcedure, kHotkeySubclass, i);
    }
    synchronizing = false;
    tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        gDialog, nullptr, gInstance, nullptr);
    if (tooltip) {
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 320);
        for (int i = 0; i < 3; ++i) AddTooltip(edits[i], L"阶段参数：0–200");
        AddTooltip(IDC_WHEEL_STEP, L"滚轮步进：1–200");
        AddTooltip(IDC_GEAR_VALUE, L"齿轮数值：−128–128");
        AddTooltip(IDC_GEAR_MINUS, L"调小齿轮数值");
        AddTooltip(IDC_GEAR_PLUS, L"调大齿轮数值");
    }
    SelectPage(0);
}

bool Checked(int id) { return IsDlgButtonChecked(gDialog, id) == BST_CHECKED; }

INT_PTR CALLBACK DialogProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        gDialog = window;
        HICON largeIcon = static_cast<HICON>(LoadImageW(gInstance,
            MAKEINTRESOURCEW(IDI_DORO_WHITE), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR | LR_SHARED));
        HICON smallIcon = static_cast<HICON>(LoadImageW(gInstance,
            MAKEINTRESOURCEW(IDI_DORO_WHITE), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR | LR_SHARED));
        if (largeIcon) {
            SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
        }
        if (smallIcon) {
            SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        }
        SetUpControls();
        SyncConfig();
        RECT owner, dialog;
        if (GetWindowRect(GetWindow(window, GW_OWNER), &owner) && GetWindowRect(window, &dialog)) {
            HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info = {sizeof(info)};
            if (GetMonitorInfoW(monitor, &info)) {
                int width = dialog.right - dialog.left, height = dialog.bottom - dialog.top;
                int x = Bound(owner.left + (owner.right - owner.left - width) / 2,
                    info.rcWork.left, info.rcWork.right - width);
                int y = Bound(owner.top + (owner.bottom - owner.top - height) / 2,
                    info.rcWork.top, info.rcWork.bottom - height);
                SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
        }
        return TRUE;
    }
    case WM_COMMAND: {
        if (synchronizing) return TRUE;
        const int id = LOWORD(wParam), event = HIWORD(wParam);
        if (id == IDCANCEL) { DestroyWindow(window); return TRUE; }
        if (id == IDOK) { FinalizeNumbers(); return TRUE; }
        if (id == IDC_SAVE && event == BN_CLICKED) {
            FinalizeNumbers();
            if (!SaveSettings()) MessageBoxW(window, L"配置保存失败。", L"加速补丁", MB_OK | MB_ICONERROR);
            return TRUE;
        }
        if (event == EN_CHANGE || event == EN_KILLFOCUS) {
            int* value = nullptr; int low, high;
            if (NumericField(id, value, low, high)) {
                UpdateNumeric(id, event == EN_KILLFOCUS);
                return TRUE;
            }
            if (id >= IDC_HOTKEY_SUPER && id <= IDC_HOTKEY_MINUS) {
                const WORD hotkey = static_cast<WORD>(SendDlgItemMessageW(window, id, HKM_GETHOTKEY, 0, 0));
                if (LOBYTE(hotkey) && hotkey != gSettings.hotkeys[id - IDC_HOTKEY_SUPER]) {
                    gSettings.hotkeys[id - IDC_HOTKEY_SUPER] = hotkey;
                    CommitSettings();
                }
                if (event == EN_KILLFOCUS && !LOBYTE(hotkey)) {
                    synchronizing = true;
                    SendDlgItemMessageW(window, id, HKM_SETHOTKEY, gSettings.hotkeys[id - IDC_HOTKEY_SUPER], 0);
                    synchronizing = false;
                }
                return TRUE;
            }
        }
        if (event != BN_CLICKED) break;
        if (id == IDC_SUPER) {
            gSettings.superEnabled = Checked(id);
            if (gSettings.superEnabled) gSettings.phaseEnabled = false;
        } else if (id >= IDC_SUPER_DIALOG && id <= IDC_SUPER_S) {
            gSettings.superFlags[id - IDC_SUPER_DIALOG] = Checked(id);
        } else if (id == IDC_PHASE) {
            gSettings.phaseEnabled = Checked(id);
            if (gSettings.phaseEnabled) gSettings.superEnabled = false;
        } else if (id >= IDC_PHASE_OWN && id <= IDC_PHASE_ENEMY) {
            gSettings.phaseFlags[id - IDC_PHASE_OWN] = Checked(id);
        } else if (id == IDC_WHEEL) {
            gSettings.wheelEnabled = Checked(id);
        } else if (id == IDC_GEAR) {
            gSettings.gearEnabled = Checked(id);
        } else if (id == IDC_GEAR_MINUS || id == IDC_GEAR_PLUS) {
            gSettings.gearRaw = GearStep(gSettings.gearRaw, id == IDC_GEAR_PLUS ? 1 : -1);
        } else if (id == IDC_SHIFT) {
            gSettings.shiftKey = Checked(id);
        } else break;
        CommitSettings();
        return TRUE;
    }
    case WM_HSCROLL:
        if (!synchronizing && reinterpret_cast<HWND>(lParam) == GetDlgItem(window, IDC_GEAR_SLIDER)) {
            const int value = GearValue(static_cast<int>(SendDlgItemMessageW(window, IDC_GEAR_SLIDER, TBM_GETPOS, 0, 0)));
            if (value != gSettings.gearRaw) {
                gSettings.gearRaw = value;
                CommitSettings();
            }
            return TRUE;
        }
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->idFrom == IDC_PAGE_TAB &&
            reinterpret_cast<NMHDR*>(lParam)->code == TCN_SELCHANGE) {
            FinalizeNumbers();
            SelectPage(static_cast<int>(SendDlgItemMessageW(window, IDC_PAGE_TAB, TCM_GETCURSEL, 0, 0)));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return TRUE;
    case WM_DESTROY:
        if (tooltip) DestroyWindow(tooltip);
        tooltip = nullptr;
        gDialog = nullptr;
        return TRUE;
    }
    return FALSE;
}
}

void SyncConfig() {
    if (!gDialog || !IsWindow(gDialog)) return;
    synchronizing = true;
    CheckDlgButton(gDialog, IDC_SUPER, gSettings.superEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(gDialog, IDC_PHASE, gSettings.phaseEnabled ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(gDialog, IDC_SUPER), !gSettings.phaseEnabled);
    EnableWindow(GetDlgItem(gDialog, IDC_PHASE), !gSettings.superEnabled);
    for (int i = 0; i < 5; ++i)
        CheckDlgButton(gDialog, IDC_SUPER_DIALOG + i, gSettings.superFlags[i] ? BST_CHECKED : BST_UNCHECKED);
    for (int i = 0; i < 3; ++i) {
        CheckDlgButton(gDialog, IDC_PHASE_OWN + i, gSettings.phaseFlags[i] ? BST_CHECKED : BST_UNCHECKED);
        if (editingNumeric != IDC_PHASE_VALUE_OWN + i) WriteInteger(IDC_PHASE_VALUE_OWN + i, gSettings.phase[i]);
    }
    CheckDlgButton(gDialog, IDC_WHEEL, gSettings.wheelEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(gDialog, IDC_GEAR, gSettings.gearEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(gDialog, IDC_SHIFT, gSettings.shiftKey ? BST_CHECKED : BST_UNCHECKED);
    if (editingNumeric != IDC_WHEEL_STEP) WriteInteger(IDC_WHEEL_STEP, gSettings.wheelStep);
    if (editingNumeric != IDC_GEAR_VALUE) WriteInteger(IDC_GEAR_VALUE, gSettings.gearRaw);
    SendDlgItemMessageW(gDialog, IDC_GEAR_SLIDER, TBM_SETPOS, TRUE, GearPosition(gSettings.gearRaw));
    for (int i = 0; i < 5; ++i) {
        if (GetFocus() != GetDlgItem(gDialog, IDC_HOTKEY_SUPER + i))
            SendDlgItemMessageW(gDialog, IDC_HOTKEY_SUPER + i, HKM_SETHOTKEY, gSettings.hotkeys[i], 0);
    }
    synchronizing = false;
}

bool OpenConfig(HWND owner) {
    if (gDialog && IsWindow(gDialog)) {
        ShowWindow(gDialog, SW_RESTORE);
        SetForegroundWindow(gDialog);
        return true;
    }
    HWND window = CreateDialogParamW(gInstance, MAKEINTRESOURCEW(IDD_SPEED), owner, DialogProcedure, 0);
    if (!window) return false;
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    return true;
}

bool HandleConfigMessage(MSG* message) {
    return gDialog && IsWindow(gDialog) && IsDialogMessageW(gDialog, message);
}
