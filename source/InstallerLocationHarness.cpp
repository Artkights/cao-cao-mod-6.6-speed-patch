#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace {
DWORD targetProcess = 0;
std::wstring expected;
int buttonId = 0;
HWND matched = nullptr;

BOOL CALLBACK ReadText(HWND window, LPARAM parameter) {
    wchar_t text[1024] = {};
    GetWindowTextW(window, text, static_cast<int>(_countof(text)));
    reinterpret_cast<std::wstring*>(parameter)->append(text).append(L"\n");
    return TRUE;
}

BOOL CALLBACK FindDialog(HWND window, LPARAM) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != targetProcess || !IsWindowVisible(window)) return TRUE;
    wchar_t className[64] = {};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    if (wcscmp(className, L"#32770")) return TRUE;
    std::wstring text;
    ReadText(window, reinterpret_cast<LPARAM>(&text));
    EnumChildWindows(window, ReadText, reinterpret_cast<LPARAM>(&text));
    if (text.find(expected) == std::wstring::npos || !GetDlgItem(window, buttonId)) return TRUE;
    matched = window;
    return FALSE;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 && argc != 6) {
        fprintf(stderr, "Usage: InstallerLocationHarness <installer> <expected-text> <button-id> [expected-text-2 button-id-2]\n");
        return 2;
    }
    expected = argv[2];
    buttonId = _wtoi(argv[3]);
    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(0);
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(argv[1], mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process)) return 3;
    CloseHandle(process.hThread);
    targetProcess = process.dwProcessId;
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (!matched && GetTickCount64() < deadline) {
        EnumWindows(FindDialog, 0);
        if (!matched) Sleep(50);
    }
    if (!matched) {
        TerminateProcess(process.hProcess, 9);
        CloseHandle(process.hProcess);
        fprintf(stderr, "expected dialog text not found\n");
        return 4;
    }
    PostMessageW(GetDlgItem(matched, buttonId), BM_CLICK, 0, 0);
    if (argc == 6) {
        expected = argv[4];
        buttonId = _wtoi(argv[5]);
        matched = nullptr;
        const ULONGLONG secondDeadline = GetTickCount64() + 15000;
        while (!matched && GetTickCount64() < secondDeadline) {
            EnumWindows(FindDialog, 0);
            if (!matched) Sleep(50);
        }
        if (!matched) {
            TerminateProcess(process.hProcess, 9);
            CloseHandle(process.hProcess);
            fprintf(stderr, "second expected dialog text not found\n");
            return 6;
        }
        PostMessageW(GetDlgItem(matched, buttonId), BM_CLICK, 0, 0);
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0) return 5;
    printf("PASS installer displayed all expected prompts\n");
    return 0;
}
