#include "BattleInfo.h"
#include "BattleStats.h"
#include "Features.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <string>
#include <vector>

HMODULE gInstance = nullptr;
FeatureSettings gFeatureSettings;

namespace {
BattleStatsSnapshot gFixture;
int gFailures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++gFailures;
    }
}

void Pump() {
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

std::wstring Cell(HWND list, int row, int column) {
    wchar_t buffer[256] = {};
    LVITEMW item = {};
    item.iSubItem = column;
    item.pszText = buffer;
    item.cchTextMax = static_cast<int>(sizeof(buffer) / sizeof(buffer[0]));
    SendMessageW(list, LVM_GETITEMTEXTW, row, reinterpret_cast<LPARAM>(&item));
    return buffer;
}

void Refresh(HWND window) {
    SendMessageW(window, WM_TIMER, 0x6810, 0);
    Pump();
}

BOOL CALLBACK FindTips(HWND window, LPARAM parameter) {
    wchar_t name[64] = {};
    GetClassNameW(window, name, 64);
    if (std::wstring(name) == TOOLTIPS_CLASSW &&
        SendMessageW(window, TTM_GETTOOLCOUNT, 0, 0) == 4)
        *reinterpret_cast<HWND*>(parameter) = window;
    return TRUE;
}

bool SaveOwnWindowBitmap(HWND window, const wchar_t* path) {
    RECT rectangle = {};
    if (!GetWindowRect(window, &rectangle)) return false;
    const int width = rectangle.right - rectangle.left;
    const int height = rectangle.bottom - rectangle.top;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = static_cast<DWORD>(width * height * 4);
    HDC screen = GetDC(window);
    HDC memory = CreateCompatibleDC(screen);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ previous = bitmap ? SelectObject(memory, bitmap) : nullptr;
    const bool rendered = bitmap && pixels && PrintWindow(window, memory, 0);
    bool written = false;
    if (rendered) {
        BITMAPFILEHEADER header = {};
        header.bfType = 0x4D42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + info.bmiHeader.biSizeImage;
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD bytes = 0;
            written = WriteFile(file, &header, sizeof(header), &bytes, nullptr) &&
                bytes == sizeof(header) &&
                WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &bytes, nullptr) &&
                bytes == sizeof(info.bmiHeader) &&
                WriteFile(file, pixels, info.bmiHeader.biSizeImage, &bytes, nullptr) &&
                bytes == info.bmiHeader.biSizeImage;
            CloseHandle(file);
        }
    }
    if (previous) SelectObject(memory, previous);
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, screen);
    return written;
}
using Metric = long long BattleUnitTotals::*;
const Metric kMetrics[] = {
    &BattleUnitTotals::damageDealt, &BattleUnitTotals::damageReceived,
    &BattleUnitTotals::recoveryDone, &BattleUnitTotals::criticalCount,
    &BattleUnitTotals::comboExtraHitCount, &BattleUnitTotals::attackBlockCount,
    &BattleUnitTotals::spellBlockCount, &BattleUnitTotals::physicalReceivedCount,
    &BattleUnitTotals::strategyReceivedCount
};

int Person(HWND list, int row) {
    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    return SendMessageW(list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) ?
        static_cast<int>(item.lParam) : -1;
}

int SelectedPerson(HWND list) {
    const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    return row < 0 ? -1 : Person(list, row);
}

void Click(HWND window, HWND list, int column) {
    NMLISTVIEW clicked = {};
    clicked.hdr.hwndFrom = list;
    clicked.hdr.idFrom = 0x6812;
    clicked.hdr.code = LVN_COLUMNCLICK;
    clicked.iSubItem = column;
    SendMessageW(window, WM_NOTIFY, clicked.hdr.idFrom, reinterpret_cast<LPARAM>(&clicked));
    Pump();
}

void SelectPage(HWND window, HWND tabs, int page) {
    TabCtrl_SetCurSel(tabs, page);
    NMHDR changed = {tabs, 0x6813, TCN_SELCHANGE};
    SendMessageW(window, WM_NOTIFY, changed.idFrom, reinterpret_cast<LPARAM>(&changed));
    Pump();
}

void CheckOrder(HWND list, const std::vector<BattleUnitTotals>& source,
    const std::vector<int>& indices, const char* message) {
    bool matches = ListView_GetItemCount(list) == static_cast<int>(indices.size());
    for (size_t row = 0; matches && row < indices.size(); ++row)
        matches = Person(list, static_cast<int>(row)) == source[indices[row]].personId;
    Check(matches, message);
}

void CheckDefaultOrder(HWND list, const std::vector<BattleUnitTotals>& source) {
    std::vector<int> indices;
    for (size_t index = 0; index < source.size(); ++index)
        indices.push_back(static_cast<int>(index));
    CheckOrder(list, source, indices, "default display preserves snapshot row order");
}

void CheckWholeRows(HWND list, const std::vector<BattleUnitTotals>& source) {
    bool matches = ListView_GetItemCount(list) == static_cast<int>(source.size());
    for (int row = 0; matches && row < ListView_GetItemCount(list); ++row) {
        const int person = Person(list, row);
        const auto found = std::find_if(source.begin(), source.end(),
            [person](const BattleUnitTotals& unit) { return unit.personId == person; });
        matches = found != source.end() && Cell(list, row, 0) == found->label;
        for (int column = 1; matches && column <= 9; ++column)
            matches = Cell(list, row, column) == std::to_wstring((*found).*kMetrics[column - 1]);
    }
    Check(matches, "sorting moves the name and all nine metrics together by person ID");
}

void CheckArrow(HWND header, int activeColumn, bool descending = true) {
    bool matches = true;
    for (int column = 0; column < 10; ++column) {
        HDITEMW item = {};
        item.mask = HDI_FORMAT;
        matches = Header_GetItem(header, column, &item) && matches;
        const int expected = column == activeColumn ?
            (descending ? HDF_SORTDOWN : HDF_SORTUP) : 0;
        matches = matches && (item.fmt & (HDF_SORTUP | HDF_SORTDOWN)) == expected;
    }
    Check(matches, "only the active metric has the matching native sort arrow");
}

void CheckFit(HWND list) {
    RECT client = {};
    GetClientRect(list, &client);
    int total = 0, smallest = INT_MAX, largest = 0;
    for (int column = 0; column < 10; ++column) {
        const int width = ListView_GetColumnWidth(list, column);
        total += width;
        smallest = std::min(smallest, width);
        largest = std::max(largest, width);
    }
    Check(total == client.right - client.left,
        "column widths exactly fill the client viewport without a four-pixel gap");
    Check(smallest > 0 && largest - smallest <= 1,
        "all ten column widths differ by at most one pixel");
    const HWND header = ListView_GetHeader(list);
    RECT last = {};
    Check(Header_GetItemRect(header, 9, &last) != FALSE, "last header rectangle exists");
    MapWindowPoints(header, list, reinterpret_cast<POINT*>(&last), 2);
    Check(last.right == client.right, "last header ends exactly at the visible right edge");
    Check((GetWindowLongPtrW(list, GWL_STYLE) & WS_HSCROLL) == 0,
        "table does not expose a horizontal scrollbar");
}

BattleStatsSnapshot MakeFixture(size_t count) {
    BattleStatsSnapshot fixture;
    fixture.knownMetrics = 0x3FF;
    fixture.active = true;
    fixture.physicalReceivedAvailable = true;
    fixture.strategyReceivedAvailable = true;
    const wchar_t* names[] = {L"曹操", L"夏侯惇", L"荀彧", L"典韦", L"许褚", L"郭嘉"};
    for (size_t index = 0; index < count; ++index) {
        BattleUnitTotals unit;
        unit.personId = 100 + static_cast<int>(index);
        unit.label = std::wstring(names[index % 6]) + L"（验证" + std::to_wstring(index + 1) + L"）";
        for (int metric = 0; metric < 9; ++metric)
            unit.*kMetrics[metric] = static_cast<long long>((index + 1) * 1000) + metric * 13;
        fixture.units.push_back(unit);
        unit.damageDealt += 100000;
        unit.physicalReceivedCount += 1234567890123LL;
        unit.strategyReceivedCount += 2345678901234LL;
        fixture.campaignUnits.push_back(unit);
    }
    return fixture;
}

void ResetCurrentPage(HWND window, HWND tabs) {
    SelectPage(window, tabs, 1);
    SelectPage(window, tabs, 0);
    Refresh(window);
}

void RunExtendedTests(HWND window, HWND list, HWND tabs, const std::wstring& capture) {
    const HWND header = ListView_GetHeader(list);
    MINMAXINFO minimum = {};
    SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
    Check(minimum.ptMinTrackSize.x > 0 && minimum.ptMinTrackSize.y > 0,
        "statistics supplies a minimum size for the current font");
    gFixture = MakeFixture(6);
    ResetCurrentPage(window, tabs);
    bool nonDivisible = false;
    for (int extra = 0; extra <= 10; ++extra) {
        SetWindowPos(window, nullptr, 0, 0, minimum.ptMinTrackSize.x + extra,
            minimum.ptMinTrackSize.y, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Pump();
        RECT client = {};
        GetClientRect(list, &client);
        nonDivisible = nonDivisible || (client.right - client.left) % 10 != 0;
        CheckFit(list);
    }
    Check(nonDivisible, "layout exercises widths that do not divide evenly into ten columns");
    NMHEADERW tracking = {};
    tracking.hdr.hwndFrom = header;
    tracking.hdr.code = HDN_BEGINTRACKW;
    tracking.iItem = 2;
    Check(SendMessageW(list, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&tracking)) != FALSE,
        "header divider tracking is rejected so columns remain fitted");

    const HFONT originalFont = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0));
    LOGFONTW originalDescription = {};
    Check(originalFont && GetObjectW(originalFont, sizeof(originalDescription), &originalDescription),
        "current list font is available for font-scale simulation");
    for (int percent : {100, 125, 150, 200}) {
        LOGFONTW scaled = originalDescription;
        scaled.lfHeight = MulDiv(originalDescription.lfHeight, percent, 100);
        HFONT font = CreateFontIndirectW(&scaled);
        Check(font != nullptr, "scaled font is created without changing system DPI");
        if (!font) continue;
        SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        MINMAXINFO scaledMinimum = {};
        SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&scaledMinimum));
        Check(scaledMinimum.ptMinTrackSize.x >= minimum.ptMinTrackSize.x,
            "minimum width grows with the simulated font size");
        SetWindowPos(window, nullptr, 0, 0, scaledMinimum.ptMinTrackSize.x,
            scaledMinimum.ptMinTrackSize.y, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Pump();
        CheckFit(list);
        if (!capture.empty()) Check(SaveOwnWindowBitmap(window,
            (capture + L"-font" + std::to_wstring(percent) + L".bmp").c_str()),
            "font-scaled minimum-size table renders for review");
        SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(originalFont), TRUE);
        DeleteObject(font);
    }
    SetWindowPos(window, nullptr, 0, 0, minimum.ptMinTrackSize.x,
        minimum.ptMinTrackSize.y, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Pump();

    const long long values[] = {2, 100, 10, 0, 100, LLONG_MAX};
    const std::vector<int> descending = {5, 1, 4, 2, 0, 3};
    const std::vector<int> ascending = {3, 0, 2, 1, 4, 5};
    for (int column = 1; column <= 9; ++column) {
        gFixture = MakeFixture(6);
        for (size_t row = 0; row < gFixture.units.size(); ++row)
            gFixture.units[row].*kMetrics[column - 1] = values[row];
        ResetCurrentPage(window, tabs);
        CheckDefaultOrder(list, gFixture.units);
        CheckArrow(header, -1);
        Click(window, list, column);
        CheckOrder(list, gFixture.units, descending, "first metric click sorts descending with stable ties");
        CheckArrow(header, column, true);
        CheckWholeRows(list, gFixture.units);
        Click(window, list, column);
        CheckOrder(list, gFixture.units, ascending, "second metric click sorts ascending including zero and LLONG_MAX");
        CheckArrow(header, column, false);
        CheckWholeRows(list, gFixture.units);
        CheckFit(list);
    }
    gFixture = MakeFixture(6);
    for (size_t row = 0; row < gFixture.units.size(); ++row) {
        gFixture.units[row].damageDealt = values[row];
        gFixture.units[row].recoveryDone = values[row];
    }
    ResetCurrentPage(window, tabs);
    ListView_SetItemState(list, 2, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    const int selected = gFixture.units[2].personId;
    Click(window, list, 1);
    Check(SelectedPerson(list) == selected, "sorting preserves selection by person ID");
    if (!capture.empty()) Check(SaveOwnWindowBitmap(window, (capture + L"-sorted.bmp").c_str()),
        "active descending native sort arrow renders for visual review");
    Click(window, list, 1);
    Click(window, list, 3);
    CheckOrder(list, gFixture.units, descending, "switching columns starts the new column descending");
    Click(window, list, 1);
    CheckOrder(list, gFixture.units, descending, "returning to a previous column restarts descending");
    Click(window, list, 0);
    CheckOrder(list, gFixture.units, descending, "unit header click leaves numeric ordering unchanged");
    CheckArrow(header, 1, true);
    Check(OpenBattleInfo(nullptr), "opening an existing statistics window succeeds");
    CheckArrow(header, 1, true);
    gFixture.units[3].damageDealt = LLONG_MAX;
    Refresh(window);
    CheckOrder(list, gFixture.units, {3, 5, 1, 4, 2, 0}, "timer reorders changed totals in descending direction");
    Check(SelectedPerson(list) == selected, "timer refresh preserves selection after reordering");
    CheckWholeRows(list, gFixture.units);
    Click(window, list, 1);
    gFixture.units[0].damageDealt = 200;
    Refresh(window);
    CheckOrder(list, gFixture.units, {2, 1, 4, 0, 3, 5}, "timer preserves ascending direction and stable equal values");
    CheckArrow(header, 1, false);
    SelectPage(window, tabs, 1);
    CheckDefaultOrder(list, gFixture.campaignUnits);
    CheckArrow(header, -1);
    CheckWholeRows(list, gFixture.campaignUnits);
    Click(window, list, 8);
    CheckArrow(header, 8, true);
    SelectPage(window, tabs, 0);
    CheckDefaultOrder(list, gFixture.units);
    CheckArrow(header, -1);
    Click(window, list, 1);
    CheckArrow(header, 1, true);

    gFixture.physicalReceivedAvailable = false;
    Refresh(window);
    Click(window, list, 8);
    CheckDefaultOrder(list, gFixture.units);
    Check(Cell(list, 0, 8) == L"--" && Cell(list, 0, 9) != L"--",
        "unavailable physical received metric is independent of strategy received");
    CheckArrow(header, 8, true);
    Click(window, list, 8);
    CheckDefaultOrder(list, gFixture.units);
    CheckArrow(header, 8, false);
    gFixture.physicalReceivedAvailable = true;
    gFixture.units[0].physicalReceivedCount = LLONG_MAX;
    Refresh(window);
    Check(Person(list, ListView_GetItemCount(list) - 1) == gFixture.units[0].personId,
        "newly available metric applies the remembered ascending direction");
    gFixture.strategyReceivedAvailable = false;
    Refresh(window);
    Click(window, list, 9);
    CheckDefaultOrder(list, gFixture.units);
    Check(Cell(list, 0, 8) != L"--" && Cell(list, 0, 9) == L"--",
        "unavailable strategy received metric is independent of physical received");
    gFixture.strategyReceivedAvailable = true;
    gFixture.knownMetrics &= ~BattleStatsDamageDealt;
    Refresh(window);
    Click(window, list, 1);
    CheckDefaultOrder(list, gFixture.units);
    Check(Cell(list, 0, 1) == L"--", "unknown historical metric cannot sort hidden totals");
    CheckArrow(header, 1, true);
    gFixture.knownMetrics |= BattleStatsDamageDealt;
    Refresh(window);
    Check(Person(list, 0) == gFixture.units[3].personId,
        "restored historical availability applies remembered descending direction");

    gFixture = MakeFixture(120);
    ResetCurrentPage(window, tabs);
    Check((GetWindowLongPtrW(list, GWL_STYLE) & WS_VSCROLL) != 0, "many rows expose the vertical scrollbar");
    CheckFit(list);
    ListView_EnsureVisible(list, 45, FALSE);
    const int beforeRefresh = ListView_GetTopIndex(list);
    Check(beforeRefresh > 0, "scroll fixture reaches a nonzero top row");
    Refresh(window);
    Check(ListView_GetTopIndex(list) == beforeRefresh, "timer preserves numeric vertical scroll position");
    Click(window, list, 2);
    Check(ListView_GetTopIndex(list) == 0, "a deliberate sort returns the viewport to the first row");
    CheckFit(list);
    gFixture = MakeFixture(1);
    Refresh(window);
    Check((GetWindowLongPtrW(list, GWL_STYLE) & WS_VSCROLL) == 0, "one row removes the vertical scrollbar");
    CheckFit(list);
    gFixture.units[0].physicalReceivedCount = 0;
    Refresh(window);
    Click(window, list, 8);
    Click(window, list, 8);
    Check(Cell(list, 0, 8) == L"0", "available zero remains real in a single-row sort");
    gFixture.units.clear();
    Refresh(window);
    Click(window, list, 3);
    Check(ListView_GetItemCount(list) == 1 && Cell(list, 0, 0).find(L"暂无") != std::wstring::npos,
        "sorting an empty table preserves its placeholder row");
    CheckFit(list);
    gFixture.loading = true;
    Refresh(window);
    Check(Cell(list, 0, 0).find(L"恢复") != std::wstring::npos, "loading placeholder is independent of sorting");
    gFixture = MakeFixture(6);
    Refresh(window);
    SelectPage(window, tabs, 1);
    Click(window, list, 4);
    Click(window, list, 4);
}
} // namespace

bool ReadBattleStatsSnapshot(BattleStatsSnapshot& snapshot) {
    snapshot = gFixture;
    return true;
}

int wmain(int argc, wchar_t** argv) {
    bool interactive = false;
    std::wstring capture;
    for (int argument = 1; argument < argc; ++argument) {
        if (std::wstring(argv[argument]) == L"--interactive") interactive = true;
        else if (std::wstring(argv[argument]) == L"--capture" && argument + 1 < argc)
            capture = argv[++argument];
        else {
            std::printf("Usage: BattleInfoTests [--capture bitmap-path] [--interactive]\n");
            return 2;
        }
    }
    gInstance = GetModuleHandleW(nullptr);
    const INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_WIN95_CLASSES};
    Check(InitCommonControlsEx(&controls) != FALSE, "common controls initialize");
    gFixture.knownMetrics = 0x3FF;
    gFixture.active = true;
    gFixture.physicalReceivedAvailable = true;
    gFixture.strategyReceivedAvailable = true;
    BattleUnitTotals totals;
    totals.personId = 0;
    totals.label = L"曹操（受击统计验证）";
    totals.damageDealt = 325;
    totals.damageReceived = 86;
    totals.recoveryDone = 40;
    totals.criticalCount = 2;
    totals.comboExtraHitCount = 1;
    totals.attackBlockCount = 3;
    totals.spellBlockCount = 4;
    totals.physicalReceivedCount = 9;
    totals.strategyReceivedCount = 12;
    gFixture.units.push_back(totals);
    totals.physicalReceivedCount = 1234567890123LL;
    totals.strategyReceivedCount = 2345678901234LL;
    gFixture.campaignUnits.push_back(totals);
    if (interactive) {
        gFixture = MakeFixture(48);
        for (size_t row = 0; row < gFixture.units.size(); ++row)
            for (int metric = 0; metric < 9; ++metric)
                gFixture.units[row].*kMetrics[metric] =
                    static_cast<long long>(((row * 17 + metric * 29) % 113) * (metric < 3 ? 81 : 1));
    }
    Check(OpenBattleInfo(nullptr), "production statistics window opens");
    HWND window = FindWindowW(L"XiaoAn66BattleInfoWindow", nullptr);
    Check(window != nullptr, "statistics window registered");
    if (!window) return 1;
    HWND list = GetDlgItem(window, 0x6812);
    HWND tabs = GetDlgItem(window, 0x6813);
    HWND header = ListView_GetHeader(list);
    if (interactive) {
        const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        SetWindowLongPtrW(window, GWL_EXSTYLE, (extendedStyle & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ShowWindow(window, SW_SHOW);
        std::printf("Interactive production statistics fixture: close the window to exit.\n");
        if (!capture.empty()) Check(SaveOwnWindowBitmap(window, capture.c_str()), "interactive capture renders");
        MSG message = {};
        while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!HandleBattleInfoMessage(&message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        CloseBattleInfo();
        return gFailures ? 1 : 0;
    }
    Check(Header_GetItemCount(header) == 10, "all ten columns exist");
    HMODULE headerModule = nullptr;
    const LONG_PTR headerProcedure = GetClassLongPtrW(header, GCLP_WNDPROC);
    const BOOL foundHeaderModule = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(headerProcedure), &headerModule);
    wchar_t headerModulePath[MAX_PATH] = {};
    const bool readHeaderModule = foundHeaderModule &&
        GetModuleFileNameW(headerModule, headerModulePath, MAX_PATH) != 0;
    bool sideBySideControls = false;
    for (const wchar_t* component = headerModulePath; *component; ++component)
        if (_wcsnicmp(component, L"\\winsxs\\", 8) == 0) sideBySideControls = true;
    Check(readHeaderModule && sideBySideControls,
        "native header loads side-by-side common controls for visible v6 sort arrows");
    Check(Cell(list, 0, 6) == L"3" && Cell(list, 0, 7) == L"4",
        "existing dodge metrics retain their values");
    Check(Cell(list, 0, 8) == L"9" && Cell(list, 0, 9) == L"12",
        "received columns display independent totals");

    SetWindowPos(window, nullptr, 20, 20, 1540, 460, SWP_NOZORDER | SWP_NOACTIVATE);
    Pump();
    int totalWidth = 0;
    int firstWidth = 0;
    for (int column = 0; column < 10; ++column) {
        const int width = ListView_GetColumnWidth(list, column);
        if (!column) firstWidth = width;
        Check(width == firstWidth || width == firstWidth + 1 || width == firstWidth - 1,
            "wide columns have equal widths");
        totalWidth += width;
    }
    RECT client = {};
    GetClientRect(list, &client);
    Check(totalWidth == client.right - client.left, "wide window exactly fills available width");
    if (!capture.empty())
        Check(SaveOwnWindowBitmap(window, capture.c_str()), "review bitmap renders production UI");

    MINMAXINFO minimum = {};
    SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
    SetWindowPos(window, nullptr, 20, 20, minimum.ptMinTrackSize.x,
        minimum.ptMinTrackSize.y, SWP_NOZORDER | SWP_NOACTIVATE);
    Pump();
    firstWidth = ListView_GetColumnWidth(list, 0);
    totalWidth = 0;
    for (int column = 0; column < 10; ++column) {
        const int width = ListView_GetColumnWidth(list, column);
        Check(width == firstWidth || width == firstWidth + 1 || width == firstWidth - 1,
            "narrow columns have equal widths");
        totalWidth += width;
    }
    GetClientRect(list, &client);
    Check(totalWidth == client.right - client.left, "minimum-size window exactly fills available width");
    SCROLLINFO horizontal = {sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS};
    Check(GetScrollInfo(list, SB_HORZ, &horizontal) &&
        horizontal.nMax <= static_cast<int>(horizontal.nPage) + 1 && horizontal.nPos == 0,
        "narrow window keeps every column visible without horizontal scrolling");
    if (!capture.empty()) {
        const std::wstring path = capture + L"-narrow.bmp";
        Check(SaveOwnWindowBitmap(window, path.c_str()), "narrow full-width UI renders for review");
    }

    HWND tips = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindTips, reinterpret_cast<LPARAM>(&tips));
    if (!tips) EnumChildWindows(window, FindTips, reinterpret_cast<LPARAM>(&tips));
    Check(tips != nullptr, "all four explanatory header tooltips exist");
    for (int column = 8; tips && column < 10; ++column) {
        TOOLINFOW tool = {TTTOOLINFOW_V2_SIZE};
        tool.hwnd = header;
        tool.uId = static_cast<UINT_PTR>(column);
        Check(SendMessageW(tips, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool)) != FALSE,
            "received header tooltip remains registered after resize");
        RECT native = {};
        Header_GetItemRect(header, column, &native);
        Check(EqualRect(&tool.rect, &native) != FALSE,
            "received tooltip follows the native header rectangle");
        wchar_t text[1024] = {};
        tool.lpszText = text;
        SendMessageW(tips, TTM_GETTEXTW, 1024, reinterpret_cast<LPARAM>(&tool));
        Check(std::wstring(text).find(L"免疫") != std::wstring::npos &&
            std::wstring(text).find(L"0 伤害") != std::wstring::npos,
            "received tooltip explains immunity and zero damage");
    }

    gFixture.physicalReceivedAvailable = false;
    Refresh(window);
    SCROLLINFO afterRefresh = {sizeof(afterRefresh), SIF_POS};
    GetScrollInfo(list, SB_HORZ, &afterRefresh);
    Check(afterRefresh.nPos == 0,
        "periodic refresh keeps the fully visible table at the left edge");
    Check(Cell(list, 0, 8) == L"--" && Cell(list, 0, 9) == L"12",
        "physical capability degrades independently");
    if (!capture.empty()) {
        const std::wstring path = capture + L"-unavailable.bmp";
        Check(SaveOwnWindowBitmap(window, path.c_str()), "independent unavailable UI renders for review");
    }
    gFixture.physicalReceivedAvailable = true;
    gFixture.strategyReceivedAvailable = false;
    Refresh(window);
    Check(Cell(list, 0, 8) == L"9" && Cell(list, 0, 9) == L"--",
        "strategy capability degrades independently");
    gFixture.strategyReceivedAvailable = true;
    gFixture.units[0].physicalReceivedCount = 0;
    gFixture.units[0].strategyReceivedCount = 0;
    Refresh(window);
    Check(Cell(list, 0, 8) == L"0" && Cell(list, 0, 9) == L"0",
        "available zero remains distinct from unavailable");
    gFixture.knownMetrics &= ~BattleStatsPhysicalReceived;
    Refresh(window);
    Check(Cell(list, 0, 8) == L"--", "unknown historical metric does not invent zero");
    gFixture.knownMetrics |= BattleStatsPhysicalReceived;
    Refresh(window);
    TabCtrl_SetCurSel(tabs, 1);
    NMHDR changed = {tabs, 0x6813, TCN_SELCHANGE};
    SendMessageW(window, WM_NOTIFY, 0x6813, reinterpret_cast<LPARAM>(&changed));
    Check(Cell(list, 0, 8) == L"1234567890123" && Cell(list, 0, 9) == L"2345678901234",
        "campaign tab shows full 64-bit received totals");
    RunExtendedTests(window, list, tabs, capture);
    CloseBattleInfo();
    Check(!IsWindow(window), "statistics window closes cleanly");
    Check(OpenBattleInfo(nullptr), "statistics window reopens");
    window = FindWindowW(L"XiaoAn66BattleInfoWindow", nullptr);
    list = GetDlgItem(window, 0x6812);
    tabs = GetDlgItem(window, 0x6813);
    header = ListView_GetHeader(list);
    Check(TabCtrl_GetCurSel(tabs) == 0, "reopening selects current battlefield by default");
    CheckDefaultOrder(list, gFixture.units);
    CheckArrow(header, -1);
    Click(window, list, 4);
    CheckArrow(header, 4, true);
    Check(Person(list, 0) == gFixture.units.back().personId,
        "first click after reopening starts descending");
    CloseBattleInfo();
    std::printf("BattleInfoTests: %s (%d failures)\n", gFailures ? "FAIL" : "PASS", gFailures);
    return gFailures ? 1 : 0;
}
