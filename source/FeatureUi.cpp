#include "FeatureUi.h"

#include "Features.h"
#include "ResourceIds.h"

#include <commctrl.h>
#include <string>

namespace {
constexpr UINT kCampFirst = 0x6900;
constexpr UINT kKindFirst = 0x6910;
constexpr UINT kTurnStart = 0x6920;
constexpr UINT kTurnEnd = 0x6921;
constexpr UINT kScreenshots = 0x6922;
constexpr UINT kSlots = 0x6923;
constexpr UINT kRefresh = 0x6924;
constexpr UINT kSave = 0x6925;
constexpr UINT kClose = 0x6926;

HWND gWindow = nullptr;
HWND gControls[32] = {};
HFONT gFont = nullptr;
std::wstring gRoot;

void Check(UINT id, bool value) {
    HWND control = GetDlgItem(gWindow, id);
    if (control) SendMessageW(control, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool Checked(UINT id) {
    HWND control = GetDlgItem(gWindow, id);
    return control && SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

int Number(UINT id, int fallback, int low, int high) {
    BOOL valid = FALSE;
    const UINT value = GetDlgItemInt(gWindow, id, &valid, FALSE);
    return valid ? Bound(static_cast<int>(value), low, high) : fallback;
}

void LoadControls() {
    for (int i = 0; i < 3; ++i) Check(kCampFirst + i, gFeatureSettings.animationCamps[i]);
    for (size_t i = 0; i < static_cast<size_t>(AnimationKind::Count); ++i)
        Check(kKindFirst + static_cast<UINT>(i), gFeatureSettings.animationKinds[i]);
    Check(kTurnStart, gFeatureSettings.autoSaveTurnStart);
    Check(kTurnEnd, gFeatureSettings.autoSaveTurnEnd);
    Check(kScreenshots, gFeatureSettings.saveScreenshots);
    SetDlgItemInt(gWindow, kSlots, static_cast<UINT>(gFeatureSettings.autoSaveSlots), FALSE);
    SetDlgItemInt(gWindow, kRefresh, static_cast<UINT>(gFeatureSettings.battleRefreshMilliseconds), FALSE);
}

bool SaveControls() {
    FeatureSettings next = gFeatureSettings;
    for (int i = 0; i < 3; ++i) next.animationCamps[i] = Checked(kCampFirst + i);
    for (size_t i = 0; i < static_cast<size_t>(AnimationKind::Count); ++i)
        next.animationKinds[i] = Checked(kKindFirst + static_cast<UINT>(i));
    next.autoSaveTurnStart = Checked(kTurnStart);
    next.autoSaveTurnEnd = Checked(kTurnEnd);
    next.saveScreenshots = Checked(kScreenshots);
    next.autoSaveSlots = Number(kSlots, gFeatureSettings.autoSaveSlots, 1, 100);
    next.battleRefreshMilliseconds = Number(kRefresh,
        gFeatureSettings.battleRefreshMilliseconds, 100, 2000);
    const FeatureSettings previous = gFeatureSettings;
    gFeatureSettings = next;
    if (!SaveFeatureSettings(gRoot.c_str())) {
        gFeatureSettings = previous;
        return false;
    }
    LoadControls();
    return true;
}

HWND Add(HWND parent, const wchar_t* klass, const wchar_t* text, DWORD style,
    int x, int y, int width, int height, UINT id) {
    HWND control = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), gInstance, nullptr);
    if (control && gFont) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
    return control;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        gWindow = window;
        NONCLIENTMETRICSW metrics = {sizeof(metrics)};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            gFont = CreateFontIndirectW(&metrics.lfMessageFont);
        HICON icon = static_cast<HICON>(LoadImageW(gInstance, MAKEINTRESOURCEW(IDI_DORO_WHITE),
            IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        if (icon) SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        Add(window, WC_BUTTONW, L"S 动画跳过范围", BS_GROUPBOX, 12, 10, 436, 128, 0);
        const wchar_t* camps[] = {L"我军", L"友军", L"敌军"};
        for (int i = 0; i < 3; ++i)
            Add(window, WC_BUTTONW, camps[i], BS_AUTOCHECKBOX | WS_TABSTOP,
                26 + i * 105, 34, 90, 22, kCampFirst + i);
        const wchar_t* kinds[] = {L"移动", L"物理攻击", L"策略", L"状态变化", L"数值变化", L"战场对白"};
        for (int i = 0; i < 6; ++i)
            Add(window, WC_BUTTONW, kinds[i], BS_AUTOCHECKBOX | WS_TABSTOP,
                26 + (i % 3) * 135, 67 + (i / 3) * 28, 125, 22, kKindFirst + i);

        Add(window, WC_BUTTONW, L"自动存档与旁路信息", BS_GROUPBOX, 12, 146, 436, 114, 0);
        Add(window, WC_BUTTONW, L"回合开始自动存档", BS_AUTOCHECKBOX | WS_TABSTOP,
            26, 170, 170, 22, kTurnStart);
        Add(window, WC_BUTTONW, L"回合结束自动存档", BS_AUTOCHECKBOX | WS_TABSTOP,
            218, 170, 170, 22, kTurnEnd);
        Add(window, WC_BUTTONW, L"保存截图", BS_AUTOCHECKBOX | WS_TABSTOP,
            26, 200, 95, 22, kScreenshots);
        Add(window, WC_STATICW, L"轮换槽位：", 0, 145, 202, 72, 20, 0);
        Add(window, WC_EDITW, nullptr, WS_BORDER | ES_NUMBER | ES_RIGHT | WS_TABSTOP,
            220, 199, 55, 22, kSlots);
        Add(window, WC_STATICW, L"（1 - 100）", 0, 282, 202, 90, 20, 0);
        Add(window, WC_STATICW, L"战斗统计刷新：", 0, 26, 231, 110, 20, 0);
        Add(window, WC_EDITW, nullptr, WS_BORDER | ES_NUMBER | ES_RIGHT | WS_TABSTOP,
            132, 228, 60, 22, kRefresh);
        Add(window, WC_STATICW, L"毫秒（100 - 2000）", 0, 200, 231, 150, 20, 0);
        Add(window, WC_BUTTONW, L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP,
            306, 272, 66, 26, kSave);
        Add(window, WC_BUTTONW, L"关闭", BS_PUSHBUTTON | WS_TABSTOP,
            382, 272, 66, 26, kClose);
        LoadControls();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kSave && HIWORD(wParam) == BN_CLICKED) {
            if (!SaveControls()) MessageBoxW(window, L"无法保存功能配置。", L"加速补丁", MB_ICONERROR);
            return 0;
        }
        if (LOWORD(wParam) == kClose && HIWORD(wParam) == BN_CLICKED) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (gFont) DeleteObject(gFont);
        gFont = nullptr;
        gWindow = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW klass = {sizeof(klass)};
    klass.lpfnWndProc = WindowProc;
    klass.hInstance = gInstance;
    klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    klass.lpszClassName = L"XiaoAn66FeatureWindow";
    klass.hIcon = static_cast<HICON>(LoadImageW(gInstance, MAKEINTRESOURCEW(IDI_DORO_WHITE),
        IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    registered = true;
    return true;
}
}

bool OpenFeatureSettings(HWND owner, const wchar_t* gameRoot) {
    if (gWindow && IsWindow(gWindow)) {
        ShowWindow(gWindow, SW_RESTORE);
        SetForegroundWindow(gWindow);
        return true;
    }
    if (!gameRoot || !*gameRoot || !RegisterClass()) return false;
    gRoot = gameRoot;
    RECT ownerRect = {};
    GetWindowRect(owner, &ownerRect);
    gWindow = CreateWindowExW(WS_EX_TOOLWINDOW, L"XiaoAn66FeatureWindow", L"功能设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        ownerRect.left + 40, ownerRect.top + 40, 476, 350, owner, nullptr, gInstance, nullptr);
    if (!gWindow) return false;
    ShowWindow(gWindow, SW_SHOW);
    UpdateWindow(gWindow);
    return true;
}

void CloseFeatureSettings() {
    if (gWindow && IsWindow(gWindow)) DestroyWindow(gWindow);
    gWindow = nullptr;
}

bool IsFeatureSettingsWindow(HWND window) {
    return window && gWindow && (window == gWindow || IsChild(gWindow, window));
}

bool HandleFeatureSettingsMessage(MSG* message) {
    if (!message || !IsFeatureSettingsWindow(message->hwnd)) return false;
    return IsDialogMessageW(gWindow, message) != FALSE;
}
