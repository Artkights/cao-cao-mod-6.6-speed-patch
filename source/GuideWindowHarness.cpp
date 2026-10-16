#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <string>
#include <vector>

namespace {

struct SearchContext {
    DWORD process = 0;
    std::wstring title;
    bool requireMenu = false;
    HWND found = nullptr;
};

BOOL CALLBACK FindWindowForProcess(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<SearchContext*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != context->process || !IsWindowVisible(window)) return TRUE;
    if (context->requireMenu && !GetMenu(window)) return TRUE;
    wchar_t title[256] = {};
    GetWindowTextW(window, title, static_cast<int>(_countof(title)));
    if (!context->title.empty() && context->title != title) return TRUE;
    context->found = window;
    return FALSE;
}

HWND FindWindowForProcess(DWORD process, const wchar_t* title, bool requireMenu = false) {
    SearchContext context;
    context.process = process;
    if (title) context.title = title;
    context.requireMenu = requireMenu;
    EnumWindows(FindWindowForProcess, reinterpret_cast<LPARAM>(&context));
    return context.found;
}

std::wstring MenuText(HMENU menu, int position) {
    wchar_t text[128] = {};
    GetMenuStringW(menu, static_cast<UINT>(position), text, static_cast<int>(_countof(text)), MF_BYPOSITION);
    return text;
}

BOOL CALLBACK CountChild(HWND window, LPARAM parameter) {
    auto* counts = reinterpret_cast<int*>(parameter);
    wchar_t className[64] = {};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    if (!_wcsicmp(className, L"ComboBox")) ++counts[0];
    else if (!_wcsicmp(className, L"Button")) ++counts[1];
    else if (!_wcsicmp(className, L"Edit") || !_wcsicmp(className, RICHEDIT_CLASSW)) ++counts[2];
    return TRUE;
}

HWND FindChildByClass(HWND parent, const wchar_t* className) {
    return FindWindowExW(parent, nullptr, className, nullptr);
}

std::wstring ReadControlText(HWND control) {
    DWORD_PTR length = 0;
    if (!SendMessageTimeoutW(control, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 1000, &length)) return {};
    std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
    DWORD_PTR ignored = 0;
    if (!SendMessageTimeoutW(control, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()),
        SMTO_ABORTIFHUNG, 1000, &ignored)) return {};
    return text.data();
}

int FindComboItemPrefix(HWND combo, const wchar_t* prefix) {
    const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < count; ++i) {
        const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0);
        if (length < 0 || length > 1024) continue;
        std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
        if (SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(i),
            reinterpret_cast<LPARAM>(text.data())) < 0) continue;
        if (wcsncmp(text.data(), prefix, wcslen(prefix)) == 0) return static_cast<int>(i);
    }
    return -1;
}

bool SelectScenario(HWND guide, HWND combo, HWND textControl, int index, const wchar_t* expectedTitle) {
    if (index < 0 || SendMessageW(combo, CB_SETCURSEL, index, 0) == CB_ERR) return false;
    SendMessageW(guide, WM_COMMAND, MAKEWPARAM(0x6703, CBN_SELCHANGE),
        reinterpret_cast<LPARAM>(combo));
    for (int i = 0; i < 200; ++i) {
        const std::wstring text = ReadControlText(textControl);
        if (text.find(L"正在整理") == std::wstring::npos && text.find(expectedTitle) != std::wstring::npos)
            return true;
        Sleep(25);
    }
    return false;
}

bool ContainsForbiddenText(const std::wstring& text) {
    static const wchar_t* words[] = {
        L"来源", L"未确认", L"未完全解析", L"命令编号", L"偏移", L"[loose@", L".eex"
    };
    for (const wchar_t* word : words) if (text.find(word) != std::wstring::npos) return true;
    return false;
}

uintptr_t ProcessImageBase(DWORD process) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W module = {sizeof(module)};
    const uintptr_t result = Module32FirstW(snapshot, &module) ?
        reinterpret_cast<uintptr_t>(module.modBaseAddr) : 0;
    CloseHandle(snapshot);
    return result;
}

bool ReadBytes(HANDLE process, uintptr_t address, BYTE (&bytes)[3]) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), bytes,
        sizeof(bytes), &read) != FALSE && read == sizeof(bytes);
}

bool ReadParagraphFormat(HANDLE process, HWND control, PARAFORMAT2& format) {
    void* remote = VirtualAllocEx(process, nullptr, sizeof(format), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    SIZE_T transferred = 0;
    DWORD_PTR messageResult = 0;
    const bool result = WriteProcessMemory(process, remote, &format, sizeof(format), &transferred) != FALSE &&
        transferred == sizeof(format) &&
        SendMessageTimeoutW(control, EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(remote),
            SMTO_ABORTIFHUNG, 1000, &messageResult) != 0 &&
        ReadProcessMemory(process, remote, &format, sizeof(format), &transferred) != FALSE &&
        transferred == sizeof(format);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return result;
}

bool ReadCharacterFormat(HANDLE process, HWND control, CHARFORMAT2W& format) {
    void* remote = VirtualAllocEx(process, nullptr, sizeof(format), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    SIZE_T transferred = 0;
    DWORD_PTR messageResult = 0;
    const bool result = WriteProcessMemory(process, remote, &format, sizeof(format), &transferred) != FALSE &&
        transferred == sizeof(format) &&
        SendMessageTimeoutW(control, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(remote),
            SMTO_ABORTIFHUNG, 1000, &messageResult) != 0 &&
        ReadProcessMemory(process, remote, &format, sizeof(format), &transferred) != FALSE &&
        transferred == sizeof(format);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return result;
}

bool FocusWindow(HWND window) {
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    for (int i = 0; i < 40; ++i) {
        if (GetForegroundWindow() == window) return true;
        Sleep(25);
    }
    return false;
}

bool PressZ() {
    INPUT input[2] = {};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = 'Z';
    input[1] = input[0];
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2, input, sizeof(INPUT)) == 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        fwprintf(stderr, L"Usage: GuideWindowHarness <game-pid>\n");
        return 2;
    }
    const DWORD process = wcstoul(argv[1], nullptr, 10);
    HWND mainWindow = FindWindowForProcess(process, nullptr, true);
    if (!mainWindow) return 3;
    HANDLE gameProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, process);
    const uintptr_t imageBase = ProcessImageBase(process);
    if (!gameProcess || !imageBase) {
        if (gameProcess) CloseHandle(gameProcess);
        return 3;
    }
    const uintptr_t dialogSite = imageBase + (0x42D5B0u - 0x400000u);
    BYTE originalDialog[3] = {};
    if (!ReadBytes(gameProcess, dialogSite, originalDialog)) {
        CloseHandle(gameProcess);
        return 3;
    }
    HMENU menu = GetMenu(mainWindow);
    const int count = menu ? GetMenuItemCount(menu) : 0;
    int patchPosition = -1, guidePosition = -1, statisticsPosition = -1;
    for (int i = 0; i < count; ++i) {
        const std::wstring text = MenuText(menu, i);
        if (text == L"加速补丁") patchPosition = i;
        if (text == L"简易攻略") guidePosition = i;
        if (text == L"战斗统计") statisticsPosition = i;
    }
    if (patchPosition < 0 || guidePosition != patchPosition + 1 ||
        statisticsPosition != guidePosition + 1) return 4;
    if (!PostMessageW(mainWindow, WM_COMMAND, 0xEE73, 0)) return 5;

    HWND guide = nullptr;
    for (int i = 0; i < 100 && !guide; ++i) {
        Sleep(50);
        guide = FindWindowForProcess(process, L"简易攻略");
    }
    if (!guide || GetWindow(guide, GW_OWNER) != mainWindow || !IsWindowEnabled(mainWindow)) return 6;
    const LONG_PTR style = GetWindowLongPtrW(guide, GWL_STYLE);
    if (!(style & WS_THICKFRAME) || !(style & WS_MINIMIZEBOX)) return 7;

    int childCounts[3] = {};
    EnumChildWindows(guide, CountChild, reinterpret_cast<LPARAM>(childCounts));
    if (childCounts[0] != 2 || childCounts[1] != 1 || childCounts[2] != 1 ||
        FindChildByClass(guide, WC_TABCONTROLW) ||
        FindChildByClass(guide, L"XiaoAn66GuideMap")) return 8;
    HWND scenarioCombo = FindChildByClass(guide, L"ComboBox");
    HWND textControl = FindChildByClass(guide, RICHEDIT_CLASSW);
    if (!textControl) textControl = FindChildByClass(guide, L"Edit");
    if (!scenarioCombo || !textControl) return 8;
    wchar_t textClass[64] = {};
    GetClassNameW(textControl, textClass, static_cast<int>(_countof(textClass)));
    if (_wcsicmp(textClass, RICHEDIT_CLASSW)) return 15;
    const LONG_PTR textStyle = GetWindowLongPtrW(textControl, GWL_STYLE);
    if (textStyle & (WS_HSCROLL | ES_AUTOHSCROLL)) return 16;
    RECT clientRect = {};
    RECT textRect = {};
    GetClientRect(textControl, &clientRect);
    SendMessageW(textControl, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&textRect));
    if (textRect.left < 8 || clientRect.right - textRect.right < 8) return 17;
    PARAFORMAT2 paragraph = {};
    paragraph.cbSize = sizeof(paragraph);
    paragraph.dwMask = PFM_SPACEAFTER;
    if (!ReadParagraphFormat(gameProcess, textControl, paragraph) ||
        !(paragraph.dwMask & PFM_SPACEAFTER) || paragraph.dySpaceAfter != 45) return 18;
    CHARFORMAT2W character = {};
    character.cbSize = sizeof(character);
    if (!ReadCharacterFormat(gameProcess, textControl, character) ||
        !(character.dwMask & CFM_SIZE) || character.yHeight != 220) return 19;
    std::wstring text;
    for (int i = 0; i < 200; ++i) {
        text = ReadControlText(textControl);
        if (text.find(L"正在整理") == std::wstring::npos) break;
        Sleep(50);
    }
    if (text.find(L"简易攻略") == std::wstring::npos || ContainsForbiddenText(text) ||
        text.find(L"\r\n\r\n") != std::wstring::npos) return 9;
    const LRESULT scenarioCount = SendMessageW(scenarioCombo, CB_GETCOUNT, 0, 0);
    if (scenarioCount < 2) return 9;
    const int s10 = FindComboItemPrefix(scenarioCombo, L"S_10 ");
    const int r10 = FindComboItemPrefix(scenarioCombo, L"R_10 ");
    if (!SelectScenario(guide, scenarioCombo, textControl, s10, L"第 10 关简易攻略") ||
        !SelectScenario(guide, scenarioCombo, textControl, r10, L"第 10 关简易攻略")) return 9;
    DWORD_PTR response = 0;
    if (!SendMessageTimeoutW(mainWindow, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &response)) return 10;

    BYTE guideFocusedDialog[3] = {};
    if (!FocusWindow(guide) || !PressZ()) return 12;
    Sleep(300);
    if (!ReadBytes(gameProcess, dialogSite, guideFocusedDialog) ||
        memcmp(originalDialog, guideFocusedDialog, sizeof(originalDialog)) != 0) return 12;

    PostMessageW(guide, WM_CLOSE, 0, 0);
    for (int i = 0; i < 100 && IsWindow(guide); ++i) Sleep(25);
    if (IsWindow(guide) || !IsWindow(mainWindow)) return 11;
    BYTE enabledDialog[3] = {};
    BYTE restoredDialog[3] = {};
    if (!FocusWindow(mainWindow) || !PressZ()) return 13;
    Sleep(300);
    if (!ReadBytes(gameProcess, dialogSite, enabledDialog) ||
        memcmp(originalDialog, enabledDialog, sizeof(originalDialog)) == 0) return 13;
    if (!PressZ()) return 14;
    Sleep(300);
    if (!ReadBytes(gameProcess, dialogSite, restoredDialog) ||
        memcmp(originalDialog, restoredDialog, sizeof(originalDialog)) != 0) return 14;
    CloseHandle(gameProcess);
    wprintf(L"GuideWindowHarness: PASS\n");
    return 0;
}
