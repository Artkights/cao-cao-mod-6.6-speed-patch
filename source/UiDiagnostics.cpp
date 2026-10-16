#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

namespace {
DWORD targetProcess = 0;
HWND previousDialog = nullptr;
BOOL CALLBACK InspectWindow(HWND window, LPARAM) {
    wchar_t title[256] = {}, className[128] = {};
    GetWindowTextW(window, title, _countof(title));
    GetClassNameW(window, className, _countof(className));
    RECT rect = {}, dirty = {};
    GetWindowRect(window, &rect);
    const BOOL needsPaint = GetUpdateRect(window, &dirty, FALSE);
    wprintf(L"hwnd=%p parent=%p id=%d class=%ls visible=%d enabled=%d rect=(%ld,%ld,%ld,%ld) dirty=%d style=%08lx ex=%08lx title=%ls\n",
        window, GetParent(window), GetDlgCtrlID(window), className,
        IsWindowVisible(window), IsWindowEnabled(window), rect.left, rect.top,
        rect.right, rect.bottom, needsPaint, GetWindowLongW(window, GWL_STYLE),
        GetWindowLongW(window, GWL_EXSTYLE), title);
    return TRUE;
}

BOOL CALLBACK FindDialog(HWND window, LPARAM) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != targetProcess || window == previousDialog) return TRUE;
    wchar_t className[128] = {};
    GetClassNameW(window, className, _countof(className));
    if (lstrcmpW(className, L"#32770")) return TRUE;
    wchar_t title[128] = {};
    GetWindowTextW(window, title, _countof(title));
    if (lstrcmpW(title, L"加速补丁")) return TRUE;
    previousDialog = window;
    InspectWindow(window, 0);
    EnumChildWindows(window, InspectWindow, 0);
    fflush(stdout);
    return FALSE;
}
}

int wmain(int count, wchar_t** arguments) {
    if (count == 3 && !lstrcmpW(arguments[1], L"--watch")) {
        targetProcess = wcstoul(arguments[2], nullptr, 0);
        const ULONGLONG started = GetTickCount64();
        while (GetTickCount64() - started < 60000) {
            if (previousDialog && !IsWindow(previousDialog)) previousDialog = nullptr;
            EnumWindows(FindDialog, 0);
            Sleep(20);
        }
        return 0;
    }
    if (count != 2) {
        wprintf(L"argument count=%d expected 2\n", count);
        return 1;
    }
    HWND window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(wcstoul(arguments[1], nullptr, 0)));
    if (!IsWindow(window)) {
        wprintf(L"invalid window=%p argument=%ls\n", window, arguments[1]);
        return 2;
    }
    InspectWindow(window, 0);
    EnumChildWindows(window, InspectWindow, 0);
    return 0;
}
