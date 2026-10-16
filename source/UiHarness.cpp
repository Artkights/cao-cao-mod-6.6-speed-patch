#include "Patch.h"

Settings gSettings;
HMODULE gInstance = nullptr;
uintptr_t gGameBase = 0;
HWND gMainWindow = nullptr;
HWND gDialog = nullptr;

void CommitSettings() { SyncConfig(); }
bool SaveSettings() { return true; }

namespace {
LRESULT CALLBACK HostProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TIMER && !gDialog) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    gInstance = instance;
    if (lstrcmpiW(commandLine, L"--dpi-aware") == 0) SetProcessDPIAware();
    const INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_WIN95_CLASSES};
    if (!InitCommonControlsEx(&controls)) return 1;
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = HostProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = L"XiaoAn66UiHarness";
    if (!RegisterClassW(&windowClass)) return 2;
    gMainWindow = CreateWindowExW(0, windowClass.lpszClassName, L"加速补丁 界面测试",
        WS_OVERLAPPEDWINDOW, 100, 100, 900, 700, nullptr, nullptr, instance, nullptr);
    if (!gMainWindow || !OpenConfig(gMainWindow)) return 3;
    SetTimer(gMainWindow, 1, 100, nullptr);
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (HandleConfigMessage(&message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
