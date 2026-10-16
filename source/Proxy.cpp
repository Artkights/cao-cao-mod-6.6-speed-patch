#include "Patch.h"
#include "InstallCommon.h"
#include "ResourceIds.h"
#include "Guide.h"
#include "Features.h"
#include "BattleRuntime.h"
#include "BattleInfo.h"
#include "FeatureUi.h"
#include "SaveEnhancer.h"
#include "BattleStats.h"
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#pragma comment(linker, "/export:CDAudioClose=Koeicda_org.CDAudioClose,@1")
#pragma comment(linker, "/export:CDAudioEjectMedia=Koeicda_org.CDAudioEjectMedia,@2")
#pragma comment(linker, "/export:CDAudioGetDriveLetter=Koeicda_org.CDAudioGetDriveLetter,@3")
#pragma comment(linker, "/export:CDAudioGetNumDrives=Koeicda_org.CDAudioGetNumDrives,@4")
#pragma comment(linker, "/export:CDAudioGetNumTracks=Koeicda_org.CDAudioGetNumTracks,@5")
#pragma comment(linker, "/export:CDAudioGetPlayingPos=Koeicda_org.CDAudioGetPlayingPos,@6")
#pragma comment(linker, "/export:CDAudioGetStartPos=Koeicda_org.CDAudioGetStartPos,@7")
#pragma comment(linker, "/export:CDAudioGetTrackLength=Koeicda_org.CDAudioGetTrackLength,@8")
#pragma comment(linker, "/export:CDAudioInitialize=Koeicda_org.CDAudioInitialize,@9")
#pragma comment(linker, "/export:CDAudioIsInserted=Koeicda_org.CDAudioIsInserted,@10")
#pragma comment(linker, "/export:CDAudioIsPlaying=Koeicda_org.CDAudioIsPlaying,@11")
#pragma comment(linker, "/export:CDAudioNextPlay=Koeicda_org.CDAudioNextPlay,@12")
#pragma comment(linker, "/export:CDAudioNextPlayTrack=Koeicda_org.CDAudioNextPlayTrack,@13")
#pragma comment(linker, "/export:CDAudioOpen=Koeicda_org.CDAudioOpen,@14")
#pragma comment(linker, "/export:CDAudioPause=Koeicda_org.CDAudioPause,@15")
#pragma comment(linker, "/export:CDAudioPlay=Koeicda_org.CDAudioPlay,@16")
#pragma comment(linker, "/export:CDAudioPlayTrack=Koeicda_org.CDAudioPlayTrack,@17")
#pragma comment(linker, "/export:CDAudioResume=Koeicda_org.CDAudioResume,@18")
#pragma comment(linker, "/export:CDAudioStop=Koeicda_org.CDAudioStop,@19")
#pragma comment(linker, "/export:CDAudioTerminate=Koeicda_org.CDAudioTerminate,@20")

Settings gSettings;
HMODULE gInstance = nullptr;
uintptr_t gGameBase = 0;
HWND gMainWindow = nullptr;
HWND gDialog = nullptr;

namespace {
constexpr UINT kConfigure = 0xEE70;
constexpr UINT kSave = 0xEE71;
constexpr UINT kUninstall = 0xEE72;
constexpr UINT kGuide = 0xEE73;
constexpr UINT kBattleInfo = 0xEE74;
constexpr UINT kFeatures = 0xEE75;
constexpr UINT kSaveBrowser = 0xEE76;
constexpr UINT kAcceleratorCommand = WM_APP + 0x66;
constexpr UINT_PTR kSubclass = 0xCC660702;
enum AcceleratorCommand : WPARAM {
    CommandSuper,
    CommandPhase,
    CommandGear,
    CommandGearUp,
    CommandGearDown,
    CommandOpenConfig,
    CommandWheelUp,
    CommandWheelDown
};
using AddProcessFn = BOOL (__cdecl*)(HANDLE);
using SetSpeedFn = void (__cdecl*)(int);
using UnInterceptFn = void (__cdecl*)();
AddProcessFn gAddProcess = nullptr;
SetSpeedFn gSetSpeed = nullptr;
UnInterceptFn gUnIntercept = nullptr;
HMODULE gGearModule = nullptr;
HMENU gParentMenu = nullptr;
HMENU gPatchMenu = nullptr;
bool gCompatible = false;
bool gInitialized = false;
bool gReportedError = false;
Settings gAppliedSettings;
bool gHasAppliedSettings = false;
int gConfigLines[74] = {};
wchar_t gDirectory[32768] = {};
wchar_t gConfigPath[32768] = {};
ULONGLONG gLastModuleCheck = 0;
volatile LONG gUninstalling = FALSE;
HHOOK gKeyboardHook = nullptr;
DWORD gKeyboardThread = 0;
volatile LONG gCapturedKeys[256] = {};

void StopKeyboardHook();

BOOL CALLBACK ConfirmExitDialog(HWND window, LPARAM parameter) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != gMainWindow) return TRUE;
    wchar_t className[32] = {}, title[64] = {};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    GetWindowTextW(window, title, static_cast<int>(_countof(title)));
    HWND yes = GetDlgItem(window, IDYES);
    HWND no = GetDlgItem(window, IDNO);
    if (lstrcmpW(className, L"#32770") || !yes || !no || lstrcmpW(title, L"结束游戏")) return TRUE;
    PostMessageW(yes, BM_CLICK, 0, 0);
    *reinterpret_cast<bool*>(parameter) = true;
    return FALSE;
}

DWORD WINAPI ConfirmGameClose(void*) {
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (InterlockedCompareExchange(&gUninstalling, FALSE, FALSE) && GetTickCount64() < deadline) {
        bool confirmed = false;
        EnumWindows(ConfirmExitDialog, reinterpret_cast<LPARAM>(&confirmed));
        if (confirmed) return 0;
        Sleep(50);
    }
    return 0;
}

bool CreateFolder(const wchar_t* path) {
    return CreateDirectoryW(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

void BeginUninstall(HWND owner) {
    if (InterlockedCompareExchange(&gUninstalling, FALSE, FALSE)) return;
    XiaoAnInstall::Manifest manifest;
    std::wstring error;
    if (!XiaoAnInstall::ValidateUninstall(gDirectory, manifest, error, true)) {
        MessageBoxW(owner, (L"卸载预检失败，没有修改游戏文件。\r\n\r\n" + error).c_str(),
                    L"加速补丁", MB_ICONERROR);
        return;
    }
    std::vector<unsigned char> workerBytes;
    std::wstring workerHash;
    if (!XiaoAnInstall::ReadResourceBytes(gInstance, IDR_UNINSTALL_WORKER, workerBytes, error) ||
        !XiaoAnInstall::Sha256Bytes(workerBytes, workerHash, error)) {
        MessageBoxW(owner, (L"卸载助手预检失败，没有修改游戏文件。\r\n\r\n" + error).c_str(),
                    L"加速补丁", MB_ICONERROR);
        return;
    }
    if (MessageBoxW(owner,
        L"确认卸载加速补丁吗？\r\n\r\n游戏将关闭，未保存进度会丢失。游戏退出后，卸载助手会恢复安装前的原始文件。",
        L"卸载加速补丁", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;

    wchar_t temporaryRoot[32768] = {};
    const DWORD tempLength = GetTempPathW(static_cast<DWORD>(_countof(temporaryRoot)), temporaryRoot);
    if (!tempLength || tempLength >= _countof(temporaryRoot)) {
        MessageBoxW(owner, L"无法取得临时目录，没有修改游戏状态。", L"加速补丁", MB_ICONERROR);
        return;
    }
    wchar_t productFolder[32768] = {}, installFolder[32768] = {}, workerPath[32768] = {};
    swprintf_s(productFolder, L"%sXiaoAn66", temporaryRoot);
    swprintf_s(installFolder, L"%s\\%s", productFolder, manifest.installId.c_str());
    swprintf_s(workerPath, L"%s\\UninstallWorker.exe", installFolder);
    if (!CreateFolder(productFolder) || !CreateFolder(installFolder) ||
        !XiaoAnInstall::WriteBytes(workerPath, workerBytes, error)) {
        MessageBoxW(owner, (L"无法释放卸载助手，没有修改游戏状态。\r\n\r\n" + error).c_str(),
                    L"加速补丁", MB_ICONERROR);
        return;
    }
    std::wstring diskHash;
    if (!XiaoAnInstall::Sha256File(workerPath, diskHash, error) ||
        _wcsicmp(diskHash.c_str(), workerHash.c_str()) ||
        !XiaoAnInstall::ValidatePe32Executable(workerPath, error)) {
        DeleteFileW(workerPath);
        MessageBoxW(owner, (L"卸载助手写入后校验失败，没有修改游戏状态。\r\n\r\n" + error).c_str(),
                    L"加速补丁", MB_ICONERROR);
        return;
    }

    HANDLE inheritedProcess = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &inheritedProcess,
                         SYNCHRONIZE, TRUE, 0)) {
        const DWORD code = GetLastError();
        DeleteFileW(workerPath);
        MessageBoxW(owner, (L"无法创建可继承的游戏进程句柄，Windows 错误 " + std::to_wstring(code) +
                    L"。没有修改游戏状态。").c_str(), L"加速补丁", MB_ICONERROR);
        return;
    }
    std::wstring command = Quote(workerPath) + L" /root " + Quote(gDirectory) + L" /handle " +
        std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(inheritedProcess)));
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(0);
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    const BOOL started = CreateProcessW(workerPath, mutableCommand.data(), nullptr, nullptr, TRUE,
                                        CREATE_UNICODE_ENVIRONMENT, nullptr, installFolder,
                                        &startup, &process);
    const DWORD startError = started ? ERROR_SUCCESS : GetLastError();
    CloseHandle(inheritedProcess);
    if (!started) {
        DeleteFileW(workerPath);
        MessageBoxW(owner, (L"无法启动卸载助手，Windows 错误 " + std::to_wstring(startError) +
                    L"。没有修改游戏状态。").c_str(), L"加速补丁", MB_ICONERROR);
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    InterlockedExchange(&gUninstalling, TRUE);
    StopKeyboardHook();
    if (gSetSpeed) gSetSpeed(0);
    if (gUnIntercept) gUnIntercept();
    ShutdownBattleStats();
    RestoreEngine();
    RestoreSaveEnhancer();
    if (gDialog) DestroyWindow(gDialog);
    HANDLE closeThread = CreateThread(nullptr, 0, ConfirmGameClose, nullptr, 0, nullptr);
    if (closeThread) CloseHandle(closeThread);
    PostMessageW(owner, WM_CLOSE, 0, 0);
}

bool ParseLine(const char* text, int& value) {
    errno = 0;
    char* end = nullptr;
    const long result = strtol(text, &end, 10);
    if (end == text || errno == ERANGE) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end) return false;
    value = static_cast<int>(result);
    return true;
}

void LoadSettings() {
    int values[74] = {};
    bool valid[74] = {};
    FILE* file = nullptr;
    if (_wfopen_s(&file, gConfigPath, L"rb") == 0) {
        char line[128];
        for (int i = 0; i < 74 && fgets(line, sizeof(line), file); ++i) {
            valid[i] = ParseLine(line, values[i]);
            if (valid[i]) gConfigLines[i] = values[i];
        }
        fclose(file);
    }
    auto get = [&](int index, int fallback, int lo, int hi) {
        return valid[index] && values[index] >= lo && values[index] <= hi ? values[index] : fallback;
    };
    for (int i = 0; i < 5; ++i) gSettings.superFlags[i] = get(1 + i, 1, 0, 1) != 0;
    for (int i = 0; i < 3; ++i) {
        gSettings.phaseFlags[i] = get(7 + i, 1, 0, 1) != 0;
        gSettings.phase[i] = get(11 + i, 60, 0, 200);
    }
    gSettings.wheelEnabled = get(10, 0, 0, 1) != 0;
    gSettings.wheelStep = get(14, 5, 1, 200);
    gSettings.gearEnabled = get(15, 0, 0, 1) != 0;
    gSettings.gearRaw = get(16, 0, -128, 128);
    gSettings.shiftKey = get(35, 1, 0, 1) != 0;
    for (int i = 0; i < 5; ++i) {
        const int value = get(65 + i, gSettings.hotkeys[i], 1, 65535);
        const int key = value & 0xFF;
        const int modifiers = value >> 8;
        if (key && !(modifiers & ~(HOTKEYF_SHIFT | HOTKEYF_CONTROL | HOTKEYF_ALT | HOTKEYF_EXT)))
            gSettings.hotkeys[i] = static_cast<WORD>(value);
    }
    gSettings.superEnabled = false;
    gSettings.phaseEnabled = false;
}

bool GameForeground() {
    DWORD process = 0;
    HWND foreground = GetForegroundWindow();
    return foreground && !IsGuideWindow(foreground) && !IsBattleInfoWindow(foreground) &&
        !IsFeatureSettingsWindow(foreground) && !IsSaveBrowserWindow(foreground) &&
        GetWindowThreadProcessId(foreground, &process) && process == GetCurrentProcessId();
}

bool EditingShortcut() {
    if (!gDialog) return false;
    const HWND foreground = GetForegroundWindow();
    return foreground == gDialog || IsChild(gDialog, foreground);
}

BYTE CurrentModifiers() {
    BYTE modifiers = 0;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers |= HOTKEYF_SHIFT;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= HOTKEYF_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers |= HOTKEYF_ALT;
    return modifiers;
}

bool MatchesKey(WORD hotkey, UINT key, BYTE modifiers) {
    if (LOBYTE(hotkey) != key) return false;
    return (HIBYTE(hotkey) & (HOTKEYF_SHIFT | HOTKEYF_CONTROL | HOTKEYF_ALT)) == modifiers;
}

int ResolveKeyCommand(UINT key, BYTE modifiers) {
    if (key == VK_TAB && !(modifiers & (HOTKEYF_CONTROL | HOTKEYF_ALT))) return CommandOpenConfig;
    if (!gCompatible) return -1;
    for (int i = 0; i < 5; ++i) {
        if (MatchesKey(gSettings.hotkeys[i], key, modifiers)) return i;
    }
    return -1;
}

bool PostAcceleratorCommand(AcceleratorCommand command) {
    HWND target = gMainWindow;
    return target && IsWindow(target) && PostMessageW(target, kAcceleratorCommand, command, 0) != FALSE;
}

bool LatchKey(UINT key) {
    return key < ARRAYSIZE(gCapturedKeys) &&
        InterlockedCompareExchange(&gCapturedKeys[key], 1, 0) == 0;
}

bool ReleaseKey(UINT key) {
    return key < ARRAYSIZE(gCapturedKeys) && InterlockedExchange(&gCapturedKeys[key], 0) != 0;
}

bool CaptureKeyDown(UINT key, BYTE modifiers, bool nativeFallback) {
    if (InterlockedCompareExchange(&gUninstalling, FALSE, FALSE) || !GameForeground() || EditingShortcut())
        return false;
    const int command = ResolveKeyCommand(key, modifiers);
    const bool capture = command >= 0 || key == 'Z';
    if (!capture) return false;
    if (!LatchKey(key)) return true;
    if (command >= 0) PostAcceleratorCommand(static_cast<AcceleratorCommand>(command));
    if (nativeFallback && key == 'Z') {
        HANDLE release = CreateThread(nullptr, 0, [](void*) -> DWORD {
            while ((GetAsyncKeyState('Z') & 0x8000) &&
                   !InterlockedCompareExchange(&gUninstalling, FALSE, FALSE)) Sleep(10);
            ReleaseKey('Z');
            return 0;
        }, nullptr, 0, nullptr);
        if (release) CloseHandle(release);
    }
    return true;
}

void __cdecl NativeSuperCommand() {
    CaptureKeyDown('Z', CurrentModifiers(), true);
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && lParam) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (key->vkCode < ARRAYSIZE(gCapturedKeys)) {
            if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                const bool captured = ReleaseKey(key->vkCode);
                if (captured) return 1;
            } else if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                if (CaptureKeyDown(key->vkCode, CurrentModifiers(), false)) return 1;
            }
        }
    }
    return CallNextHookEx(gKeyboardHook, code, wParam, lParam);
}

void StopKeyboardHook() {
    const DWORD thread = gKeyboardThread;
    if (thread) PostThreadMessageW(thread, WM_QUIT, 0, 0);
}

void ExecuteAcceleratorCommand(AcceleratorCommand command) {
    if (InterlockedCompareExchange(&gUninstalling, FALSE, FALSE)) return;
    if (command == CommandOpenConfig) {
        if (!gDialog) OpenConfig(gMainWindow);
        return;
    }
    if (!gCompatible) return;
    if (command == CommandSuper && !gSettings.phaseEnabled)
        gSettings.superEnabled = !gSettings.superEnabled;
    else if (command == CommandPhase && !gSettings.superEnabled)
        gSettings.phaseEnabled = !gSettings.phaseEnabled;
    else if (command == CommandGear)
        gSettings.gearEnabled = !gSettings.gearEnabled;
    else if (command == CommandGearUp)
        gSettings.gearRaw = GearStep(gSettings.gearRaw, 1);
    else if (command == CommandGearDown)
        gSettings.gearRaw = GearStep(gSettings.gearRaw, -1);
    else if (command == CommandWheelUp || command == CommandWheelDown) {
        if (!gSettings.phaseEnabled || !gSettings.wheelEnabled) return;
        const int phase = CurrentGamePhase();
        if (phase < 0 || phase > 2 || !gSettings.phaseFlags[phase]) return;
        const int direction = command == CommandWheelUp ? 1 : -1;
        gSettings.phase[phase] = Bound(gSettings.phase[phase] + direction * gSettings.wheelStep, 0, 200);
    } else return;
    CommitSettings();
}

void WheelCommand(WPARAM wParam) {
    if (InterlockedCompareExchange(&gUninstalling, FALSE, FALSE) || !GameForeground() ||
        !gCompatible || !gSettings.phaseEnabled || !gSettings.wheelEnabled) return;
    const int phase = CurrentGamePhase();
    if (phase < 0 || phase > 2 || !gSettings.phaseFlags[phase]) return;
    const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    if (!delta) return;
    PostAcceleratorCommand(delta > 0 ? CommandWheelUp : CommandWheelDown);
}

LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == kAcceleratorCommand) {
        ExecuteAcceleratorCommand(static_cast<AcceleratorCommand>(wParam));
        return 0;
    }
    if (message == WM_COMMAND && HIWORD(wParam) == 0) {
        if (LOWORD(wParam) == kConfigure) { OpenConfig(window); return 0; }
        if (LOWORD(wParam) == kSave) {
            if (!SaveSettings()) MessageBoxW(window, L"无法保存加速配置。", L"加速补丁", MB_ICONERROR);
            return 0;
        }
        if (LOWORD(wParam) == kFeatures) { OpenFeatureSettings(window, gDirectory); return 0; }
        if (LOWORD(wParam) == kSaveBrowser) { OpenSaveBrowser(window, gDirectory); return 0; }
        if (LOWORD(wParam) == kUninstall) { BeginUninstall(window); return 0; }
        if (LOWORD(wParam) == kGuide) { OpenGuide(window, gDirectory); return 0; }
        if (LOWORD(wParam) == kBattleInfo) { OpenBattleInfo(window); return 0; }
    }
    if (message == WM_MOUSEWHEEL) WheelCommand(wParam);
    if (message == WM_NCDESTROY) {
        if (gDialog) DestroyWindow(gDialog);
        CloseGuide();
        CloseBattleInfo();
        CloseFeatureSettings();
        CloseSaveBrowser();
        RestoreSaveEnhancer();
        ShutdownBattleStats();
        RestoreEngine();
        if (gSetSpeed) gSetSpeed(0);
        if (gUnIntercept) gUnIntercept();
        StopKeyboardHook();
        RemoveWindowSubclass(window, MainProc, kSubclass);
        gMainWindow = nullptr;
        gParentMenu = gPatchMenu = nullptr;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void AttachWindow(HWND window) {
    if (!window) return;
    window = GetAncestor(window, GA_ROOT);
    if (!window || GetWindow(window, GW_OWNER) || !IsWindowVisible(window)) return;
    DWORD process = 0;
    if (GetWindowThreadProcessId(window, &process) != GetCurrentThreadId() || process != GetCurrentProcessId()) return;
    HMENU parent = GetMenu(window);
    if (!parent || (gMainWindow && window != gMainWindow)) return;
    if (window == gMainWindow && parent == gParentMenu && IsMenu(gPatchMenu)) return;
    HMENU popup = CreatePopupMenu();
    if (!popup) return;
    if (!AppendMenuW(popup, MF_STRING, kConfigure, L"配置补丁") ||
        !AppendMenuW(popup, MF_STRING, kSave, L"保存配置") ||
        !AppendMenuW(popup, MF_STRING, kFeatures, L"功能设置") ||
        !AppendMenuW(popup, MF_STRING, kSaveBrowser, L"存档浏览") ||
        !AppendMenuW(popup, MF_SEPARATOR, 0, nullptr) ||
        !AppendMenuW(popup, MF_STRING, kUninstall, L"卸载补丁")) {
        DestroyMenu(popup);
        return;
    }
    const int originalMenuCount = GetMenuItemCount(parent);
    if (!AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(popup), L"加速补丁")) {
        DestroyMenu(popup);
        return;
    }
    if (!AppendMenuW(parent, MF_STRING, kGuide, L"简易攻略")) {
        RemoveMenu(parent, originalMenuCount, MF_BYPOSITION);
        DestroyMenu(popup);
        return;
    }
    if (!AppendMenuW(parent, MF_STRING, kBattleInfo, L"战斗统计")) {
        RemoveMenu(parent, originalMenuCount + 1, MF_BYPOSITION);
        RemoveMenu(parent, originalMenuCount, MF_BYPOSITION);
        DestroyMenu(popup);
        return;
    }
    if (!SetWindowSubclass(window, MainProc, kSubclass, 0)) {
        RemoveMenu(parent, originalMenuCount + 2, MF_BYPOSITION);
        RemoveMenu(parent, originalMenuCount + 1, MF_BYPOSITION);
        RemoveMenu(parent, originalMenuCount, MF_BYPOSITION);
        DestroyMenu(popup);
        DrawMenuBar(window);
        return;
    }
    gMainWindow = window;
    gParentMenu = parent;
    gPatchMenu = popup;
    DrawMenuBar(window);
    if (!gInitialized) {
        gInitialized = true;
        LoadSettings();
        // The loaded configuration is the safe baseline.  A first failed
        // transaction must not roll the UI back to the zero-initialized
        // Settings object.
        gAppliedSettings = gSettings;
        gHasAppliedSettings = true;
        CommitSettings();
    }
}

void RestoreFailedFeatureSettings() {
    if (!gHasAppliedSettings) return;
    const wchar_t* error = EngineError();
    const bool superChanged = gSettings.superEnabled != gAppliedSettings.superEnabled ||
        memcmp(gSettings.superFlags, gAppliedSettings.superFlags, sizeof(gSettings.superFlags)) != 0;
    const bool phaseChanged = gSettings.phaseEnabled != gAppliedSettings.phaseEnabled ||
        memcmp(gSettings.phaseFlags, gAppliedSettings.phaseFlags, sizeof(gSettings.phaseFlags)) != 0 ||
        memcmp(gSettings.phase, gAppliedSettings.phase, sizeof(gSettings.phase)) != 0;
    const bool gearChanged = gSettings.gearEnabled != gAppliedSettings.gearEnabled ||
        gSettings.gearRaw != gAppliedSettings.gearRaw;
    bool restoreSuper = error && wcsstr(error, L"超级") != nullptr;
    bool restorePhase = error && wcsstr(error, L"阶段") != nullptr;
    bool restoreGear = error && wcsstr(error, L"齿轮") != nullptr;

    // A mutual-exclusion error belongs to whichever mode was newly selected.
    if (error && wcsstr(error, L"不能同时开启")) {
        if (superChanged && !phaseChanged) restoreSuper = true;
        else if (phaseChanged && !superChanged) restorePhase = true;
        else restoreSuper = restorePhase = true;
    }
    // Keep unrelated controls usable when a feature-specific transaction fails.
    // An unclassified error still rolls back only groups changed by this commit.
    if (!restoreSuper && !restorePhase && !restoreGear) {
        restoreSuper = superChanged;
        restorePhase = phaseChanged;
        restoreGear = gearChanged;
    }
    if (restoreSuper) {
        gSettings.superEnabled = gAppliedSettings.superEnabled;
        memcpy(gSettings.superFlags, gAppliedSettings.superFlags, sizeof(gSettings.superFlags));
    }
    if (restorePhase) {
        gSettings.phaseEnabled = gAppliedSettings.phaseEnabled;
        memcpy(gSettings.phaseFlags, gAppliedSettings.phaseFlags, sizeof(gSettings.phaseFlags));
        memcpy(gSettings.phase, gAppliedSettings.phase, sizeof(gSettings.phase));
    }
    if (restoreGear) {
        gSettings.gearEnabled = gAppliedSettings.gearEnabled;
        gSettings.gearRaw = gAppliedSettings.gearRaw;
    }
}

// The game imports the ANSI dispatcher, while every window created by the
// proxy uses the Unicode window classes.  Calling DispatchMessageA for both
// kinds of windows lets User32 thunk our WndProc and can corrupt text/control
// messages on systems whose ACP is not 936.  Keep the game's original ANSI
// path, but dispatch proxy-owned Unicode windows through the W entry point.
LRESULT DispatchMessageForWindow(const MSG* message) {
    if (!message || !message->hwnd || !IsWindowUnicode(message->hwnd))
        return DispatchMessageA(message);
    return DispatchMessageW(message);
}

LRESULT WINAPI PatchedDispatch(const MSG* message) {
    if (message) {
        AttachWindow(message->hwnd);
        if (gMainWindow) {
            const ULONGLONG now = GetTickCount64();
            PollBattleStats(now);
            if (!InterlockedCompareExchange(&gUninstalling, FALSE, FALSE) &&
                gCompatible && gAddProcess && now - gLastModuleCheck > 1000) {
                gLastModuleCheck = now;
                gAddProcess(GetCurrentProcess());
            }
        }
    }
    return DispatchMessageForWindow(message);
}

HANDLE WINAPI PatchedCreateFileA(LPCSTR fileName, DWORD desiredAccess, DWORD shareMode,
    LPSECURITY_ATTRIBUTES security, DWORD creationDisposition, DWORD flags,
    HANDLE templateFile) {
    // Only the game's IAT slot is replaced.  This call uses the proxy DLL's
    // own kernel32 import and therefore cannot recurse through this hook.
    HANDLE result = CreateFileA(fileName, desiredAccess, shareMode, security, creationDisposition,
        flags, templateFile);
    if (result != INVALID_HANDLE_VALUE) ObserveGuideScenarioPathA(fileName);
    return result;
}

BOOL WINAPI PatchedTranslate(const MSG* message) {
    if (message) {
        AttachWindow(message->hwnd);
        if (!InterlockedCompareExchange(&gUninstalling, FALSE, FALSE) && gMainWindow &&
            (HandleConfigMessage(const_cast<MSG*>(message)) ||
                HandleGuideMessage(const_cast<MSG*>(message)) ||
                HandleBattleInfoMessage(const_cast<MSG*>(message)) ||
                HandleFeatureSettingsMessage(const_cast<MSG*>(message)) ||
                HandleSaveBrowserMessage(const_cast<MSG*>(message)))) {
            // The native loop dispatches after translation; dialog navigation is already handled.
            MSG* consumed = const_cast<MSG*>(message);
            consumed->message = WM_NULL;
            consumed->wParam = consumed->lParam = 0;
            return TRUE;
        }
    }
    return TranslateMessage(message);
}

bool InstallGameImportHook(void* original, void* replacement) {
    BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return false;
    const auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
    const DWORD size = nt->OptionalHeader.SizeOfImage;
    const DWORD first = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    auto valid = [&](DWORD offset, DWORD bytes) { return offset < size && bytes <= size - offset; };
    if (!first) return false;
    // This EXE's directory size omits descriptors; the mapped image is the bound.
    for (DWORD offset = first, count = 0; count < 256 && valid(offset, sizeof(IMAGE_IMPORT_DESCRIPTOR));
         offset += sizeof(IMAGE_IMPORT_DESCRIPTOR), ++count) {
        const auto entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + offset);
        if (!entry->Name && !entry->FirstThunk && !entry->OriginalFirstThunk) break;
        if (!entry->FirstThunk) continue;
        for (DWORD slotOffset = entry->FirstThunk, n = 0; n < 4096 && valid(slotOffset, sizeof(void*));
             slotOffset += sizeof(void*), ++n) {
            auto slot = reinterpret_cast<void**>(base + slotOffset);
            if (!*slot) break;
            if (*slot != original) continue;
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
            InterlockedExchangePointer(slot, replacement);
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), old, &ignored);
            return true;
        }
    }
    return false;
}

DWORD WINAPI Initialize(void*) {
    const INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    gGameBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    GetModuleFileNameW(gInstance, gDirectory, static_cast<DWORD>(_countof(gDirectory)));
    wchar_t* slash = wcsrchr(gDirectory, L'\\');
    if (!slash) return 0;
    *slash = 0;
    swprintf_s(gConfigPath, L"%s\\patch\\xiaoan66\\pl_06.ini", gDirectory);
    LoadFeatureSettings(gDirectory);
    InitializeBattleRuntime(gDirectory);
    InitializeSaveEnhancer(gDirectory);
    InitializeBattleStats(gDirectory);
    SetNativeToggleHandler(&NativeSuperCommand);
    gCompatible = InitializeEngine();
    if (gCompatible) {
        wchar_t path[32768];
        swprintf_s(path, L"%s\\GearNtKe.dll", gDirectory);
        gGearModule = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (gGearModule) {
            gAddProcess = reinterpret_cast<AddProcessFn>(GetProcAddress(gGearModule, "AddProcess"));
            gSetSpeed = reinterpret_cast<SetSpeedFn>(GetProcAddress(gGearModule, "SetSpeed"));
            gUnIntercept = reinterpret_cast<UnInterceptFn>(GetProcAddress(gGearModule, "UnIntercept"));
        }
    }
    InstallGameImportHook(reinterpret_cast<void*>(&DispatchMessageA), reinterpret_cast<void*>(&PatchedDispatch));
    // Some 6.6 variants import DispatchMessageW instead of the older ANSI
    // entry point.  Installing the same wrapper is harmless when the slot is
    // absent and keeps the proxy UI Unicode in those variants as well.
    InstallGameImportHook(reinterpret_cast<void*>(&DispatchMessageW), reinterpret_cast<void*>(&PatchedDispatch));
    InstallGameImportHook(reinterpret_cast<void*>(&TranslateMessage), reinterpret_cast<void*>(&PatchedTranslate));
    gKeyboardThread = GetCurrentThreadId();
    MSG bootstrap = {};
    PeekMessageW(&bootstrap, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    gKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, gInstance, 0);
    if (!gKeyboardHook) {
        gKeyboardThread = 0;
        return 0;
    }
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UnhookWindowsHookEx(gKeyboardHook);
    gKeyboardHook = nullptr;
    gKeyboardThread = 0;
    return 0;
}
}

void CommitSettings() {
    if (InterlockedCompareExchange(&gUninstalling, FALSE, FALSE)) return;
    if (!gCompatible || !ApplyEngineState(gSettings)) {
        if (!gCompatible && gHasAppliedSettings) gSettings = gAppliedSettings;
        else RestoreFailedFeatureSettings();
        if (!gReportedError) {
            gReportedError = true;
            MessageBoxW(gMainWindow, EngineError(), L"加速补丁", MB_ICONERROR);
        }
        SyncConfig();
        return;
    }
    const bool featureWarning = EngineError()[0] != 0;
    if (featureWarning) {
        const wchar_t* error = EngineError();
        if (wcsstr(error, L"阶段加速")) gSettings.phaseEnabled = false;
        if (wcsstr(error, L"超级加速对白")) gSettings.superFlags[0] = false;
        if (wcsstr(error, L"超级加速地图文字")) gSettings.superFlags[1] = false;
        if (wcsstr(error, L"超级加速延时")) gSettings.superFlags[2] = false;
        if (wcsstr(error, L"超级加速 R")) gSettings.superFlags[3] = false;
        if (wcsstr(error, L"超级加速 S")) gSettings.superFlags[4] = false;
        if (!gReportedError) {
            gReportedError = true;
            MessageBoxW(gMainWindow, error, L"加速补丁", MB_ICONERROR);
        }
    }
    const bool gearReady = gAddProcess && gSetSpeed && gAddProcess(GetCurrentProcess());
    if (gearReady) gSetSpeed(gSettings.gearEnabled ? gSettings.gearRaw : 0);
    else {
        if (gSetSpeed) gSetSpeed(0);
        if (gSettings.gearEnabled) {
            gSettings.gearEnabled = false;
            MessageBoxW(gMainWindow, L"无法加载或启用齿轮加速模块。", L"加速补丁", MB_ICONERROR);
        }
    }
    gAppliedSettings = gSettings;
    gHasAppliedSettings = true;
    if (!featureWarning) gReportedError = false;
    SyncConfig();
}

bool SaveSettings() {
    if (InterlockedCompareExchange(&gUninstalling, FALSE, FALSE)) return false;
    gConfigLines[0] = gSettings.superEnabled;
    for (int i = 0; i < 5; ++i) gConfigLines[1 + i] = gSettings.superFlags[i];
    gConfigLines[6] = gSettings.phaseEnabled;
    for (int i = 0; i < 3; ++i) {
        gConfigLines[7 + i] = gSettings.phaseFlags[i];
        gConfigLines[11 + i] = gSettings.phase[i];
    }
    gConfigLines[10] = gSettings.wheelEnabled;
    gConfigLines[14] = gSettings.wheelStep;
    gConfigLines[15] = gSettings.gearEnabled;
    gConfigLines[16] = gSettings.gearRaw;
    gConfigLines[35] = gSettings.shiftKey;
    for (int i = 0; i < 5; ++i) gConfigLines[65 + i] = gSettings.hotkeys[i];
    wchar_t folder[32768], privateFolder[32768], temporary[32768];
    swprintf_s(folder, L"%s\\patch", gDirectory);
    if (!CreateDirectoryW(folder, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    swprintf_s(privateFolder, L"%s\\xiaoan66", folder);
    if (!CreateDirectoryW(privateFolder, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    swprintf_s(temporary, L"%s.tmp", gConfigPath);
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary, L"wb") != 0) return false;
    bool success = true;
    for (int value : gConfigLines) if (fprintf(file, "%d\r\n", value) < 0) success = false;
    if (fflush(file) != 0) success = false;
    if (fclose(file) != 0) success = false;
    if (success) success = MoveFileExW(temporary, gConfigPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!success) DeleteFileW(temporary);
    return success;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        gInstance = instance;
        DisableThreadLibraryCalls(instance);
        // Install before the EXE entry point can load its first R/S file.  The
        // hook only observes successful opens and delegates unchanged.
        InstallGameImportHook(reinterpret_cast<void*>(&CreateFileA),
            reinterpret_cast<void*>(&PatchedCreateFileA));
        HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
