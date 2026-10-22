#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string>

namespace {
DWORD targetProcess = 0;
int targetButton = 0;
HWND foundDialog = nullptr;

BOOL CALLBACK FindConfirmation(HWND window, LPARAM) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != targetProcess || !IsWindowVisible(window)) return TRUE;
    wchar_t className[64] = {}, title[128] = {};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    GetWindowTextW(window, title, static_cast<int>(_countof(title)));
    if (!wcscmp(className, L"#32770") && wcsstr(title, L"卸载加速补丁") &&
        GetDlgItem(window, targetButton)) {
        foundDialog = window;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK CloseWorkerResult(HWND window, LPARAM) {
    wchar_t className[64] = {}, title[128] = {};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    GetWindowTextW(window, title, static_cast<int>(_countof(title)));
    if (!wcscmp(className, L"#32770") && wcsstr(title, L"加速补丁卸载")) {
        HWND button = GetDlgItem(window, IDOK);
        if (button) PostMessageW(button, BM_CLICK, 0, 0);
    }
    return TRUE;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: MenuUninstallHarness <pid> <hwnd-hex> <yes|no> <root>\n");
        return 2;
    }
    targetProcess = wcstoul(argv[1], nullptr, 10);
    HWND mainWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(wcstoull(argv[2], nullptr, 16)));
    const bool confirm = !_wcsicmp(argv[3], L"yes");
    targetButton = confirm ? IDYES : IDNO;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, targetProcess);
    if (!process || !IsWindow(mainWindow)) return 3;
    if (!PostMessageW(mainWindow, WM_COMMAND, 0xEE72, 0)) return 4;
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline && !foundDialog) {
        EnumWindows(FindConfirmation, 0);
        if (!foundDialog) Sleep(50);
    }
    if (!foundDialog) {
        fprintf(stderr, "confirmation dialog not found\n");
        CloseHandle(process);
        return 5;
    }
    PostMessageW(GetDlgItem(foundDialog, targetButton), BM_CLICK, 0, 0);
    if (!confirm) {
        const DWORD wait = WaitForSingleObject(process, 1200);
        CloseHandle(process);
        if (wait != WAIT_TIMEOUT) return 6;
        printf("PASS cancellation leaves the game running\n");
        return 0;
    }
    const DWORD wait = WaitForSingleObject(process, 30000);
    CloseHandle(process);
    if (wait != WAIT_OBJECT_0) return 7;
    const std::wstring manifest65 = std::wstring(argv[4]) + L"\\patch\\xiaoan65\\install.ini";
    const std::wstring manifest66 = std::wstring(argv[4]) + L"\\patch\\xiaoan66\\install.ini";
    const auto manifestExists = [&]() {
        return GetFileAttributesW(manifest65.c_str()) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(manifest66.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    const ULONGLONG restoreDeadline = GetTickCount64() + 15000;
    while (GetTickCount64() < restoreDeadline && manifestExists())
        Sleep(100);
    EnumWindows(CloseWorkerResult, 0);
    if (manifestExists()) return 8;
    printf("PASS confirmed menu uninstall closed the game and removed the manifest\n");
    return 0;
}
