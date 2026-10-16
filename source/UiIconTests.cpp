#include "Patch.h"

#include <stdio.h>

Settings gSettings;
HMODULE gInstance = nullptr;
uintptr_t gGameBase = 0;
HWND gMainWindow = nullptr;
HWND gDialog = nullptr;

void CommitSettings() { SyncConfig(); }
bool SaveSettings() { return true; }

int wmain() {
    gInstance = GetModuleHandleW(nullptr);
    const INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_WIN95_CLASSES};
    if (!InitCommonControlsEx(&controls)) {
        fprintf(stderr, "FAIL: InitCommonControlsEx (%lu)\n", GetLastError());
        return 1;
    }
    if (!OpenConfig(nullptr) || !gDialog) {
        fprintf(stderr, "FAIL: configuration dialog could not be created (%lu)\n", GetLastError());
        return 1;
    }

    const HICON largeIcon = reinterpret_cast<HICON>(
        SendMessageW(gDialog, WM_GETICON, ICON_BIG, 0));
    const HICON smallIcon = reinterpret_cast<HICON>(
        SendMessageW(gDialog, WM_GETICON, ICON_SMALL, 0));
    const bool passed = largeIcon != nullptr && smallIcon != nullptr;
    printf("UiIconTests: large=%p small=%p failures=%d\n",
        largeIcon, smallIcon, passed ? 0 : 1);
    DestroyWindow(gDialog);
    return passed ? 0 : 1;
}
