#include "BattleInfo.h"

#include "BattleStats.h"
#include "Features.h"
#include "ResourceIds.h"

#include <algorithm>
#include <commctrl.h>
#include <string>
#include <vector>

namespace {
class TableControlsContext {
public:
    explicit TableControlsContext(HMODULE) {
        const INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
        InitCommonControlsEx(&controls);
    }
};
constexpr UINT_PTR kRefreshTimer = 0x6810;
constexpr UINT kUnitList = 0x6812;
constexpr UINT kStatsTabs = 0x6813;
constexpr UINT_PTR kListSubclass = 0x6814;
constexpr UINT kFitColumns = WM_APP + 0x681;
constexpr UINT kDpiChanged = 0x02E0;

HWND gWindow = nullptr;
HWND gList = nullptr;
HWND gTabs = nullptr;
HWND gHeaderTips = nullptr;
HFONT gFont = nullptr;
BattleStatsSnapshot gStats;
int gSortColumn = -1;
bool gDescending = true;
bool gFitPending = false;
bool gFitting = false;

UINT WindowDpi(HWND window) {
    using GetDpi = UINT (WINAPI*)(HWND);
    static const auto getDpi = reinterpret_cast<GetDpi>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (getDpi && window) {
        const UINT dpi = getDpi(window);
        if (dpi) return dpi;
    }
    HDC dc = GetDC(window);
    const UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(window, dc);
    return dpi ? dpi : 96;
}

int Scale(int value) { return MulDiv(value, static_cast<int>(WindowDpi(gWindow)), 96); }

int Metric(int index) {
    using GetMetric = int (WINAPI*)(int, UINT);
    static const auto getMetric = reinterpret_cast<GetMetric>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    return getMetric ? getMetric(index, WindowDpi(gWindow)) : GetSystemMetrics(index);
}

void RequestColumnFit() {
    if (!gWindow || gFitting || gFitPending) return;
    gFitPending = PostMessageW(gWindow, kFitColumns, 0, 0) != FALSE;
}

void UpdateFont(HWND window) {
    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    using GetMetrics = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static const auto getMetrics = reinterpret_cast<GetMetrics>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"));
    const BOOL obtained = getMetrics ? getMetrics(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
        &metrics, 0, WindowDpi(window)) :
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    HFONT next = obtained ? CreateFontIndirectW(&metrics.lfMessageFont) : nullptr;
    if (!next) return;
    HFONT previous = gFont;
    gFont = next;
    if (gList) SendMessageW(gList, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
    if (gTabs) SendMessageW(gTabs, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
    if (previous) DeleteObject(previous);
}

struct StatsColumn {
    const wchar_t* heading;
    uint32_t metric;
    long long BattleUnitTotals::* value;
    bool BattleStatsSnapshot::* available;
    const wchar_t* tooltip;
};

constexpr StatsColumn kColumns[] = {
    {L"单位", 0, nullptr, nullptr, nullptr},
    {L"造成伤害", BattleStatsDamageDealt, &BattleUnitTotals::damageDealt, nullptr, nullptr},
    {L"承受伤害", BattleStatsDamageReceived, &BattleUnitTotals::damageReceived, nullptr, nullptr},
    {L"回复量", BattleStatsRecovery, &BattleUnitTotals::recoveryDone, nullptr, nullptr},
    {L"暴击次数", BattleStatsCritical, &BattleUnitTotals::criticalCount, nullptr, nullptr},
    {L"连击次数", BattleStatsCombo, &BattleUnitTotals::comboExtraHitCount, nullptr, nullptr},
    {L"攻击闪避", BattleStatsAttackDodge, &BattleUnitTotals::attackBlockCount, nullptr,
        L"攻击闪避：我方单位避开敌方攻击的真实命中判定次数。每次独立判定计一次；免疫和无效目标不计。"},
    {L"策略闪避", BattleStatsStrategyDodge, &BattleUnitTotals::spellBlockCount, nullptr,
        L"策略闪避：我方单位避开敌方有害策略的真实命中判定次数。同一策略若独立判定状态和伤害，可分别计数；免疫、无效目标、回复和友方增益不计。"},
    {L"物理受击", BattleStatsPhysicalReceived, &BattleUnitTotals::physicalReceivedCount,
        &BattleStatsSnapshot::physicalReceivedAvailable,
        L"物理受击：我方单位每承接敌方一个实际物理攻击段计一次，包含闪避、免疫和 0 伤害。双击、三连击及范围攻击按实际攻击段和承受者分别计数。"},
    {L"策略受击", BattleStatsStrategyReceived, &BattleUnitTotals::strategyReceivedCount,
        &BattleStatsSnapshot::strategyReceivedAvailable,
        L"策略受击：我方单位每承接敌方一段有害策略作用计一次，包含伤害、异常、减益以及闪避、免疫和 0 伤害。同段伤害＋附带状态合计一次；再次执行的策略段和散射逐击计数。治疗、增益、再行动、净化和普通道具不计。"}
};
constexpr int kColumnCount = static_cast<int>(sizeof(kColumns) / sizeof(kColumns[0]));
constexpr int kInitialColumnWidth = 120;

bool MetricVisible(const StatsColumn& column) {
    return (gStats.knownMetrics & column.metric) &&
        (!column.available || gStats.*(column.available));
}

SIZE MinimumWindowSize(HWND window) {
    const HWND header = gList ? ListView_GetHeader(gList) : nullptr;
    HDC dc = GetDC(header ? header : window);
    HFONT font = header ? reinterpret_cast<HFONT>(SendMessageW(header, WM_GETFONT, 0, 0)) : gFont;
    if (!font) font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ previous = dc ? SelectObject(dc, font) : nullptr;
    int columnWidth = Scale(80);
    if (dc) {
        columnWidth = 0;
        for (const auto& column : kColumns) {
            SIZE text = {};
            GetTextExtentPoint32W(dc, column.heading, lstrlenW(column.heading), &text);
            // Native header padding plus a sort glyph, reserved on every column.
            columnWidth = std::max(columnWidth, static_cast<int>(text.cx) + Scale(20) + Metric(SM_CXVSCROLL));
        }
        SelectObject(dc, previous);
        ReleaseDC(header ? header : window, dc);
    }
    RECT frame = {};
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    using AdjustFrame = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto adjustFrame = reinterpret_cast<AdjustFrame>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
    if (adjustFrame) adjustFrame(&frame, style, FALSE, WS_EX_TOOLWINDOW, WindowDpi(window));
    else AdjustWindowRectEx(&frame, style, FALSE, WS_EX_TOOLWINDOW);
    return {columnWidth * kColumnCount + Scale(20) + 2 * Metric(SM_CXEDGE) +
        Metric(SM_CXVSCROLL) + frame.right - frame.left,
        std::max(Scale(300) + frame.bottom - frame.top, 340L)};
}

void EnsureMinimumSize(HWND window) {
    RECT current = {};
    if (!GetWindowRect(window, &current) || IsIconic(window)) return;
    const SIZE minimum = MinimumWindowSize(window);
    const int width = std::max(minimum.cx, current.right - current.left);
    const int height = std::max(minimum.cy, current.bottom - current.top);
    if (width != current.right - current.left || height != current.bottom - current.top)
        SetWindowPos(window, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void UpdateSortHeader() {
    const HWND header = gList ? ListView_GetHeader(gList) : nullptr;
    if (!header) return;
    for (int column = 0; column < kColumnCount; ++column) {
        HDITEMW item = {};
        item.mask = HDI_FORMAT;
        if (!Header_GetItem(header, column, &item)) continue;
        const int format = (item.fmt & ~(HDF_SORTUP | HDF_SORTDOWN)) |
            (column == gSortColumn ? (gDescending ? HDF_SORTDOWN : HDF_SORTUP) : 0);
        if (item.fmt != format) {
            item.fmt = format;
            Header_SetItem(header, column, &item);
        }
    }
}

void UpdateHeaderToolRects() {
    const HWND header = gList ? ListView_GetHeader(gList) : nullptr;
    if (!header || !gHeaderTips) return;
    for (int column = 0; column < kColumnCount; ++column) {
        if (!kColumns[column].tooltip) continue;
        TOOLINFOW tool = {TTTOOLINFOW_V2_SIZE};
        tool.hwnd = header;
        tool.uId = static_cast<UINT_PTR>(column);
        Header_GetItemRect(header, column, &tool.rect);
        SendMessageW(gHeaderTips, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

LRESULT CALLBACK ListSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclass, DWORD_PTR) {
    if (message == WM_NOTIFY && lParam) {
        const auto* notice = reinterpret_cast<const NMHDR*>(lParam);
        if (notice->hwndFrom == ListView_GetHeader(window) &&
            (notice->code == HDN_BEGINTRACKW || notice->code == HDN_BEGINTRACKA ||
             notice->code == HDN_DIVIDERDBLCLICKW || notice->code == HDN_DIVIDERDBLCLICKA)) return TRUE;
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    // Header rectangles follow the native horizontal viewport and resizing.
    // Refresh them after the list has processed the scroll/size notification.
    if (message == WM_HSCROLL || message == WM_MOUSEHWHEEL || message == WM_SIZE ||
        message == WM_NOTIFY) UpdateHeaderToolRects();
    if (message == WM_SIZE || message == WM_STYLECHANGED || message == WM_SETFONT)
        RequestColumnFit();
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ListSubclassProc, subclass);
    return result;
}

void ConfigureColumns() {
    if (!gList || gFitting) return;
    gFitting = true;
    // GetClientRect already excludes the client edge and vertical scrollbar.
    // Shrink first so intermediate column widths cannot create a horizontal bar.
    for (int pass = 0; pass < 2; ++pass) {
        RECT client = {};
        GetClientRect(gList, &client);
        const int usable = std::max(0L, client.right - client.left);
        for (int expanding = 0; expanding < 2; ++expanding) {
            for (int column = 0; column < kColumnCount; ++column) {
                const int width = usable / kColumnCount + (column < usable % kColumnCount ? 1 : 0);
                const int current = ListView_GetColumnWidth(gList, column);
                if ((expanding ? width > current : width < current))
                    SendMessageW(gList, LVM_SETCOLUMNWIDTH, column, width);
            }
        }
        RECT after = {};
        GetClientRect(gList, &after);
        if (after.right - after.left == usable) break;
    }
    gFitting = false;
    UpdateHeaderToolRects();
}

void InsertText(int row, int column, const std::wstring& value) {
    LVITEMW item = {};
    item.iSubItem = column;
    item.pszText = const_cast<wchar_t*>(value.c_str());
    SendMessageW(gList, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&item));
}

void InsertRow(int row, const std::wstring& label, const BattleUnitTotals* totals) {
    LVITEMW item = {};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(label.c_str());
    item.lParam = totals ? totals->personId : -1;
    SendMessageW(gList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    if (!totals) return;
    for (int column = 1; column < kColumnCount; ++column) {
        const StatsColumn& description = kColumns[column];
        InsertText(row, column, MetricVisible(description) ?
            std::to_wstring(totals->*(description.value)) : L"--");
    }
}

void Render(bool resetTop = false, bool preserveSelection = true) {
    if (!gList) return;
    const bool campaign = gTabs && TabCtrl_GetCurSel(gTabs) == 1;
    const auto& units = campaign ? gStats.campaignUnits : gStats.units;
    const int top = resetTop ? 0 : ListView_GetTopIndex(gList);
    LVITEMW selected = {};
    selected.mask = LVIF_PARAM;
    selected.iItem = ListView_GetNextItem(gList, -1, LVNI_SELECTED);
    const bool restoreSelection = preserveSelection && selected.iItem >= 0 &&
        SendMessageW(gList, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&selected)) && selected.lParam >= 0;
    std::vector<const BattleUnitTotals*> rows;
    rows.reserve(units.size());
    for (const auto& unit : units) rows.push_back(&unit);
    if (gSortColumn > 0 && gSortColumn < kColumnCount && MetricVisible(kColumns[gSortColumn])) {
        const auto value = kColumns[gSortColumn].value;
        std::stable_sort(rows.begin(), rows.end(), [value](const auto* left, const auto* right) {
            return gDescending ? left->*value > right->*value : left->*value < right->*value;
        });
    }
    SendMessageW(gList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(gList, LVM_DELETEALLITEMS, 0, 0);
    if (units.empty())
        InsertRow(0, gStats.loading ? L"正在恢复统计数据。" :
            (campaign ? L"当前存档暂无全局统计数据。" : L"当前战场暂无统计数据。"), nullptr);
    for (size_t row = 0; row < rows.size(); ++row) {
        const BattleUnitTotals& totals = *rows[row];
        const std::wstring label = totals.label.empty() ?
            L"我方角色 " + std::to_wstring(totals.personId) : totals.label;
        InsertRow(static_cast<int>(row), label, &totals);
        if (restoreSelection && totals.personId == selected.lParam)
            ListView_SetItemState(gList, static_cast<int>(row), LVIS_SELECTED | LVIS_FOCUSED,
                LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageW(gList, WM_SETREDRAW, TRUE, 0);
    // A populated list may gain a vertical scrollbar, reducing the actual
    // horizontal viewport after WM_SIZE has already run.  Re-read the list
    // client width so the columns still end at the visible right edge.
    ConfigureColumns();
    RECT rowRect = {};
    if (top > 0 && ListView_GetItemRect(gList, 0, &rowRect, LVIR_BOUNDS))
        ListView_Scroll(gList, 0, top * (rowRect.bottom - rowRect.top));
    UpdateSortHeader();
    RequestColumnFit();
    UpdateHeaderToolRects();
    InvalidateRect(gList, nullptr, FALSE);
}

void Refresh() {
    ReadBattleStatsSnapshot(gStats);
    Render();
}

void Layout(HWND window) {
    RECT client = {};
    GetClientRect(window, &client);
    const int margin = Scale(10);
    const int width = std::max(100, static_cast<int>(client.right - client.left) - margin * 2);
    const int height = std::max(80, static_cast<int>(client.bottom - client.top) - margin * 2);
    MoveWindow(gTabs, margin, margin, width, Scale(28), TRUE);
    const int listTop = Scale(34);
    MoveWindow(gList, margin, margin + listTop, width, std::max(30, height - listTop), TRUE);
    ConfigureColumns();
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        const TableControlsContext controls(gInstance);
        gWindow = window;
        gSortColumn = -1;
        gDescending = true;
        gFitPending = false;
        gFitting = false;
        HICON icon = static_cast<HICON>(LoadImageW(gInstance, MAKEINTRESOURCEW(IDI_DORO_WHITE),
            IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        if (icon) SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        UpdateFont(window);
        gTabs = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(kStatsTabs), gInstance, nullptr);
        for (int index = 0; index < 2; ++index) {
            TCITEMW item = {};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(index ? L"全局统计" : L"当前战场");
            SendMessageW(gTabs, TCM_INSERTITEMW, index, reinterpret_cast<LPARAM>(&item));
        }
        gList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(kUnitList), gInstance, nullptr);
        if (gList) SetWindowSubclass(gList, ListSubclassProc, kListSubclass, 0);
        SendMessageW(gList, LVM_SETUNICODEFORMAT, TRUE, 0);
        SendMessageW(gList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        for (int index = 0; index < kColumnCount; ++index) {
            LVCOLUMNW column = {};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            column.pszText = const_cast<wchar_t*>(kColumns[index].heading);
            column.cx = kInitialColumnWidth;
            column.fmt = index ? LVCFMT_RIGHT : LVCFMT_LEFT;
            SendMessageW(gList, LVM_INSERTCOLUMNW, index, reinterpret_cast<LPARAM>(&column));
        }
        gHeaderTips = CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,
            CW_USEDEFAULT,CW_USEDEFAULT,window,nullptr,gInstance,nullptr);
        const HWND header = ListView_GetHeader(gList);
        if (gHeaderTips && header) {
            SendMessageW(gHeaderTips,TTM_SETMAXTIPWIDTH,0,420);
            for (int column = 0; column < kColumnCount; ++column) {
                if (!kColumns[column].tooltip) continue;
                TOOLINFOW tool = {TTTOOLINFOW_V2_SIZE};
                tool.uFlags = TTF_SUBCLASS;
                tool.hwnd = header; tool.uId = static_cast<UINT_PTR>(column);
                Header_GetItemRect(header,column,&tool.rect);
                tool.lpszText = const_cast<wchar_t*>(kColumns[column].tooltip);
                SendMessageW(gHeaderTips,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
            }
        }
        if (gList && gFont)
            SendMessageW(gList, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
        if (gFont) {
            SendMessageW(gTabs, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
        }
        SetTimer(window, kRefreshTimer,
            static_cast<UINT>(gFeatureSettings.battleRefreshMilliseconds), nullptr);
        Refresh();
        EnsureMinimumSize(window);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        const SIZE minimum = MinimumWindowSize(window);
        limits->ptMinTrackSize.x = minimum.cx;
        limits->ptMinTrackSize.y = minimum.cy;
        return 0;
    }
    case WM_SIZE:
        Layout(window);
        return 0;
    case kFitColumns:
        gFitPending = false;
        EnsureMinimumSize(window);
        ConfigureColumns();
        return 0;
    case kDpiChanged:
    case WM_SETTINGCHANGE:
        // The host owns process DPI awareness. Use its current coordinate space.
        UpdateFont(window);
        if (message == kDpiChanged && lParam) {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RequestColumnFit();
        Layout(window);
        return 0;
    case WM_TIMER:
        if (wParam == kRefreshTimer) Refresh();
        return 0;
    case WM_NOTIFY: {
        const auto* notice = reinterpret_cast<const NMHDR*>(lParam);
        if (!notice) break;
        if (notice->hwndFrom == gTabs && notice->code == TCN_SELCHANGE) {
            gSortColumn = -1;
            gDescending = true;
            Render(true, false);
            return 0;
        }
        if (notice->hwndFrom == gList && notice->code == LVN_COLUMNCLICK) {
            const int column = reinterpret_cast<const NMLISTVIEW*>(lParam)->iSubItem;
            if (column > 0 && column < kColumnCount) {
                gDescending = column == gSortColumn ? !gDescending : true;
                gSortColumn = column;
                Render(true);
            }
            return 0;
        }
        if (gList && notice->hwndFrom == ListView_GetHeader(gList))
            UpdateHeaderToolRects();
        break;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kRefreshTimer);
        if (gHeaderTips) DestroyWindow(gHeaderTips);
        gHeaderTips = nullptr;
        if (gFont) DeleteObject(gFont);
        gFont = nullptr;
        gList = nullptr;
        gTabs = nullptr;
        gWindow = nullptr;
        gSortColumn = -1;
        gDescending = true;
        gFitPending = false;
        gFitting = false;
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
    constexpr int width = 1160;
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
    EnsureMinimumSize(gWindow);
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
