#include "BattleInfo.h"

#include "BattleStats.h"
#include "Features.h"
#include "ResourceIds.h"

#include <algorithm>
#include <commctrl.h>
#include <string>

namespace {
constexpr UINT_PTR kRefreshTimer = 0x6810;
constexpr UINT kUnitList = 0x6812;

HWND gWindow = nullptr;
HWND gList = nullptr;
HFONT gFont = nullptr;
BattleStatsSnapshot gStats;

void ConfigureColumns(int listWidth) {
    if (!gList) return;
    const int usable = std::max(700, listWidth - 4);
    const int unitWidth = std::max(170, usable * 27 / 100);
    const int metricWidth = (usable - unitWidth) / 5;
    SendMessageW(gList, LVM_SETCOLUMNWIDTH, 0, unitWidth);
    for (int column = 1; column < 6; ++column)
        SendMessageW(gList, LVM_SETCOLUMNWIDTH, column,
            column == 5 ? usable - unitWidth - metricWidth * 4 : metricWidth);
}

void InsertText(int row, int column, const std::wstring& value) {
    LVITEMW item = {};
    item.iSubItem = column;
    item.pszText = const_cast<wchar_t*>(value.c_str());
    SendMessageW(gList, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&item));
}

void InsertRow(int row, const std::wstring& label, const BattleUnitTotals* totals) {
    LVITEMW item = {};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(label.c_str());
    SendMessageW(gList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    if (!totals) return;
    const bool damageAvailable = gStats.physicalSettlementAvailable &&
        gStats.strategyDamageAvailable;
    InsertText(row, 1, damageAvailable ? std::to_wstring(totals->damageDealt) : L"--");
    InsertText(row, 2, damageAvailable ? std::to_wstring(totals->damageReceived) : L"--");
    InsertText(row, 3, gStats.recoveryAvailable ?
        std::to_wstring(totals->recoveryDone) : L"--");
    InsertText(row, 4, gStats.criticalAvailable ?
        std::to_wstring(totals->criticalCount) : L"--");
    InsertText(row, 5, gStats.comboAvailable ?
        std::to_wstring(totals->comboExtraHitCount) : L"--");
}

void Render() {
    if (!gList) return;
    SendMessageW(gList, LVM_DELETEALLITEMS, 0, 0);
    if (!gStats.hookAvailable) {
        InsertRow(0, L"当前版本不支持精确统计。", nullptr);
        return;
    }
    if (gStats.units.empty()) {
        InsertRow(0, gStats.active ? L"暂无统计数据。" : L"进入战场后开始统计。", nullptr);
        return;
    }
    for (size_t row = 0; row < gStats.units.size(); ++row) {
        const BattleUnitTotals& totals = gStats.units[row];
        const std::wstring label = totals.label.empty() ?
            L"我方单位 " + std::to_wstring(totals.slot + 1) : totals.label;
        InsertRow(static_cast<int>(row), label, &totals);
    }
}

void Refresh() {
    ReadBattleStatsSnapshot(gStats);
    Render();
}

void Layout(HWND window) {
    RECT client = {};
    GetClientRect(window, &client);
    constexpr int margin = 10;
    const int width = std::max(100, static_cast<int>(client.right - client.left) - margin * 2);
    const int height = std::max(80, static_cast<int>(client.bottom - client.top) - margin * 2);
    MoveWindow(gList, margin, margin, width, height, TRUE);
    ConfigureColumns(width);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        gWindow = window;
        HICON icon = static_cast<HICON>(LoadImageW(gInstance, MAKEINTRESOURCEW(IDI_DORO_WHITE),
            IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        if (icon) SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        NONCLIENTMETRICSW metrics = {sizeof(metrics)};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            gFont = CreateFontIndirectW(&metrics.lfMessageFont);
        gList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(kUnitList), gInstance, nullptr);
        SendMessageW(gList, LVM_SETUNICODEFORMAT, TRUE, 0);
        SendMessageW(gList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        const wchar_t* headings[] = {
            L"单位", L"造成伤害", L"承受伤害", L"回复量", L"暴击次数", L"连击次数"
        };
        for (int index = 0; index < 6; ++index) {
            LVCOLUMNW column = {};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            column.pszText = const_cast<wchar_t*>(headings[index]);
            column.cx = index ? 110 : 220;
            column.fmt = index ? LVCFMT_RIGHT : LVCFMT_LEFT;
            SendMessageW(gList, LVM_INSERTCOLUMNW, index, reinterpret_cast<LPARAM>(&column));
        }
        if (gList && gFont)
            SendMessageW(gList, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
        SetTimer(window, kRefreshTimer,
            static_cast<UINT>(gFeatureSettings.battleRefreshMilliseconds), nullptr);
        Refresh();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = 760;
        limits->ptMinTrackSize.y = 260;
        return 0;
    }
    case WM_SIZE:
        Layout(window);
        return 0;
    case WM_TIMER:
        if (wParam == kRefreshTimer) Refresh();
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kRefreshTimer);
        if (gFont) DeleteObject(gFont);
        gFont = nullptr;
        gList = nullptr;
        gWindow = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterWindowClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW klass = {sizeof(klass)};
    klass.lpfnWndProc = WindowProc;
    klass.hInstance = gInstance;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = static_cast<HICON>(LoadImageW(gInstance, MAKEINTRESOURCEW(IDI_DORO_WHITE),
        IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    klass.hIconSm = static_cast<HICON>(LoadImageW(gInstance, MAKEINTRESOURCEW(IDI_DORO_WHITE),
        IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    klass.lpszClassName = L"XiaoAn66BattleInfoWindow";
    if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    registered = true;
    return true;
}
} // namespace

bool OpenBattleInfo(HWND owner) {
    if (gWindow && IsWindow(gWindow)) {
        ShowWindow(gWindow, SW_RESTORE);
        SetForegroundWindow(gWindow);
        return true;
    }
    if (!RegisterWindowClass()) return false;
    RECT ownerRect = {};
    if (!GetWindowRect(owner, &ownerRect)) SetRect(&ownerRect, 100, 100, 900, 700);
    constexpr int width = 920;
    constexpr int height = 460;
    int x = ownerRect.right + 8;
    int y = ownerRect.top;
    HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO information = {sizeof(information)};
    if (GetMonitorInfoW(monitor, &information)) {
        if (x + width > information.rcWork.right)
            x = std::max(information.rcWork.left, ownerRect.left - width - 8);
        if (y + height > information.rcWork.bottom) y = information.rcWork.bottom - height;
    }
    gWindow = CreateWindowExW(WS_EX_TOOLWINDOW, L"XiaoAn66BattleInfoWindow", L"战斗统计",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width, height, owner, nullptr, gInstance, nullptr);
    if (!gWindow) return false;
    ShowWindow(gWindow, SW_SHOW);
    UpdateWindow(gWindow);
    return true;
}

void CloseBattleInfo() {
    if (gWindow && IsWindow(gWindow)) DestroyWindow(gWindow);
    gWindow = nullptr;
}

bool IsBattleInfoWindow(HWND window) {
    return window && gWindow && (window == gWindow || IsChild(gWindow, window));
}

bool HandleBattleInfoMessage(MSG* message) {
    if (!message || !IsBattleInfoWindow(message->hwnd)) return false;
    return IsDialogMessageW(gWindow, message) != FALSE;
}
