#include "Patch.h"
#include "EngineProfile.h"
#include "EngineLayout.h"
#include "InstallCommon.h"
#include "ResourceIds.h"
#include "FeatureSelection.h"
#include "Guide.h"
#include "Features.h"
#include "BattleRuntime.h"
#include "BattleInfo.h"
#include "FeatureUi.h"
#include "SaveEnhancer.h"
#include "BattleStats.h"
#include "BattleRosterRuntime.h"
#include "StatsDiagnostics.h"
#include "ScenarioLifecycle.h"
#include "SaveTransactions.h"
#include "ProtectedRuntime.h"
#include "PeImage.h"
#include "RuntimeStartup.h"
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <functional>

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
using ConfigureNativeClockSlotsFn = BOOL (__cdecl*)(HMODULE, DWORD, DWORD, void*, void*);
AddProcessFn gAddProcess = nullptr;
SetSpeedFn gSetSpeed = nullptr;
UnInterceptFn gUnIntercept = nullptr;
ConfigureNativeClockSlotsFn gConfigureNativeClockSlots = nullptr;
HMODULE gGearModule = nullptr;
HMENU gParentMenu = nullptr;
HMENU gPatchMenu = nullptr;
bool gCompatible = false;
bool gInitialized = false;
bool gReportedError = false;
RuntimeNativeImportPlan gRuntimeNativePlan;
bool gRuntimeNativeGate = false;
size_t gRuntimeImageSize = 0;
using NativeCreateFileFn = decltype(&CreateFileA);
NativeCreateFileFn gNativeCreateFile = nullptr;
using NativeCreateFileWFn = decltype(&CreateFileW);
using NativeDispatchAFn = decltype(&DispatchMessageA);
using NativeDispatchWFn = decltype(&DispatchMessageW);
using NativeTranslateFn = decltype(&TranslateMessage);
NativeCreateFileWFn gNativeCreateFileW = nullptr;
NativeDispatchAFn gNativeDispatchA = nullptr;
NativeDispatchWFn gNativeDispatchW = nullptr;
NativeTranslateFn gNativeTranslate = nullptr;
unsigned gInstalledFeatures = XiaoAnInstall::FeatureAll;
unsigned gRequestedFeatures = XiaoAnInstall::FeatureAll;
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
SRWLOCK gScenarioCaptureLock = SRWLOCK_INIT;
// Pending observations contain only a bounded ASCII basename. No guide or
// game state is touched until the executable has passed profile verification.
enum class ScenarioCaptureState { Pending, Active, Rejected };
ScenarioCaptureState gScenarioCaptureState = ScenarioCaptureState::Pending;
char gPendingScenario[32] = {};
HANDLE gStartupEvent = nullptr;
volatile LONG gStartupFinished = FALSE;
volatile LONG gStartupAccepted = FALSE;
DWORD gStartupThread = 0;
using LoaderCalloutFn = BOOLEAN (NTAPI*)();
LoaderCalloutFn gWithinLoaderCallout = nullptr;

bool HasFeature(unsigned feature) {
    return (gInstalledFeatures & feature) != 0;
}

// A missing state file identifies an older installation and retains its
// historic all-functions behavior.  A malformed present file disables all
// optional hooks rather than silently re-enabling a removed function.
bool LoadInstalledFeatures(const wchar_t* root, EngineProfileId profile) {
    const std::wstring path = PatchPrivatePath(root, profile) + L"\\feature-state.ini";
    if (!XiaoAnInstall::FileExists(path)) {
        gInstalledFeatures = XiaoAnInstall::FeatureAll;
        return true;
    }
    const int version = GetPrivateProfileIntW(L"Install", L"SchemaVersion", -1, path.c_str());
    const int mask = GetPrivateProfileIntW(L"Install", L"FeatureMask", -1, path.c_str());
    if (version != 1 || mask <= 0 || !XiaoAnInstall::IsValidFeatureMask(static_cast<unsigned>(mask))) {
        gInstalledFeatures = XiaoAnInstall::FeatureNone;
        return false;
    }
    gInstalledFeatures = static_cast<unsigned>(mask);
    return true;
}

void CompleteStartup() {
    InterlockedExchange(&gStartupFinished, TRUE);
    if (gStartupEvent) SetEvent(gStartupEvent);
}

void RecordRuntimeStartup(const wchar_t* stage, const std::wstring& detail) {
    if (!gDirectory[0]) return;
    const std::wstring privateRoot = PatchPrivatePath(gDirectory, CompiledEngineProfile());
    CreateDirectoryW((std::wstring(gDirectory) + L"\\patch").c_str(), nullptr);
    CreateDirectoryW(privateRoot.c_str(), nullptr);
    const std::wstring diagnostics = privateRoot + L"\\diagnostics";
    CreateDirectoryW(diagnostics.c_str(), nullptr);
    HANDLE file = CreateFileW((diagnostics + L"\\runtime.log").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    DWORD written = 0;
    if (GetFileSizeEx(file, &size) && !size.QuadPart) {
        const WORD bom = 0xFEFF;
        WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    }
    const std::wstring line = std::wstring(stage) + L": " + detail + L"\r\n";
    WriteFile(file, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

bool AwaitStartup() {
    if (!gStartupEvent) return true; // Standalone test harnesses do not run DllMain.
    const DWORD previousError = GetLastError();
    // Game IAT wrappers must stay transparent while initialization is pending.
    // In particular, blocking the first file call can deadlock a protector.
    const bool ready = InterlockedCompareExchange(&gStartupFinished, FALSE, FALSE) != FALSE;
    SetLastError(previousError);
    return ready;
}

bool StartupUiAvailable() {
    return !gStartupEvent || InterlockedCompareExchange(&gStartupAccepted, FALSE, FALSE) != FALSE;
}

void StopKeyboardHook();

void RestoreProtectedGate() {
    if (gRuntimeNativeGate && gGameBase) {
        std::wstring error;
        if (RestoreRuntimeNativeImports(reinterpret_cast<BYTE*>(gGameBase), gRuntimeImageSize,
            gRuntimeNativePlan, error)) {
            gRuntimeNativeGate = false;
            // A game thread can already be inside a wrapper while slots are
            // restored. Keep its captured target until process termination.
        } else {
            RecordRuntimeStartup(L"native-import-restore-conflict", error);
        }
    }
}

void DisableRuntimeResourceProviders() {
    // Disable the public provider before clearing its resolved layout. Guide
    // tasks can still be draining while startup or shutdown is in progress.
    ConfigureGuideConditionalExtension(false);
    ConfigureBattleRuntimeResourceNames(false);
    ResetBattleRuntime();
}

bool InitializeRuntimeResourceProviders(bool protected220, EngineProfileId profile) {
    const bool needsRuntimeResources = HasFeature(XiaoAnInstall::FeatureGuide) ||
        HasFeature(XiaoAnInstall::FeatureBattleStats);
    bool runtimeResourcesReady = false;
    if (needsRuntimeResources) {
        runtimeResourcesReady = InitializeBattleRuntime(gDirectory);
        // Identity and import validation authorize read-only discovery even
        // when the unpacker has not published the resource accessors yet.
        ConfigureBattleRuntimeResourceNames(true);
        RecordRuntimeStartup(runtimeResourcesReady ? L"runtime-layout-ready" : L"runtime-layout-pending",
            runtimeResourcesReady ? L"运行时布局已识别；人物表与物品表分别读取并确认就绪。" :
                L"运行时布局尚未就绪，将按需重试；攻略保留磁盘名称和 ID 回退。");
    } else {
        DisableRuntimeResourceProviders();
    }
    // The audited condition decoder and resource-table discovery are
    // independent capabilities. Neither table readiness nor statistics-only
    // installation grants the guide's condition extension.
    ConfigureGuideConditionalExtension(HasFeature(XiaoAnInstall::FeatureGuide) &&
        protected220 && profile == EngineProfileId::Ccz65);
    return runtimeResourcesReady;
}

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
    FeatureSelection selection;
    selection.mask = selection.available = manifest.featureMask;
    selection.uninstall = true;
    selection.closeGame = true;
    if (DialogBoxParamW(gInstance, MAKEINTRESOURCEW(IDD_FEATURE_SELECT), owner,
        FeatureSelectionProcedure, reinterpret_cast<LPARAM>(&selection)) != IDOK) return;
    std::vector<unsigned char> workerBytes;
    std::wstring workerHash;
    if (!XiaoAnInstall::ReadResourceBytes(gInstance, IDR_UNINSTALL_WORKER, workerBytes, error) ||
        !XiaoAnInstall::Sha256Bytes(workerBytes, workerHash, error)) {
        MessageBoxW(owner, (L"卸载助手预检失败，没有修改游戏文件。\r\n\r\n" + error).c_str(),
                    L"加速补丁", MB_ICONERROR);
        return;
    }
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
        std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(inheritedProcess))) +
        L" /features " + std::to_wstring(selection.mask);
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
    ShutdownScenarioLifecycle();
    ShutdownBattleRosterRuntime();
    ShutdownBattleStats();
    RestoreEngine();
    RestoreSaveEnhancer();
    DisableRuntimeResourceProviders();
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
    if (!HasFeature(XiaoAnInstall::FeatureAccelerator)) return -1;
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
    if (!HasFeature(XiaoAnInstall::FeatureAccelerator) ||
        InterlockedCompareExchange(&gUninstalling, FALSE, FALSE) || !GameForeground() || EditingShortcut())
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
    if (!HasFeature(XiaoAnInstall::FeatureAccelerator) || InterlockedCompareExchange(&gUninstalling, FALSE, FALSE)) return;
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
        if (LOWORD(wParam) == kConfigure && HasFeature(XiaoAnInstall::FeatureAccelerator)) { OpenConfig(window); return 0; }
        if (LOWORD(wParam) == kSave && HasFeature(XiaoAnInstall::FeatureAccelerator)) {
            if (!SaveSettings()) MessageBoxW(window, L"无法保存加速配置。", L"加速补丁", MB_ICONERROR);
            return 0;
        }
        if (LOWORD(wParam) == kFeatures && (HasFeature(XiaoAnInstall::FeatureAccelerator) || HasFeature(XiaoAnInstall::FeatureBattleStats))) { OpenFeatureSettings(window, gDirectory); return 0; }
        if (LOWORD(wParam) == kSaveBrowser && HasFeature(XiaoAnInstall::FeatureBattleStats)) { OpenSaveBrowser(window, gDirectory); return 0; }
        if (LOWORD(wParam) == kUninstall) { BeginUninstall(window); return 0; }
        if (LOWORD(wParam) == kGuide && HasFeature(XiaoAnInstall::FeatureGuide)) { OpenGuide(window, gDirectory); return 0; }
        if (LOWORD(wParam) == kBattleInfo && HasFeature(XiaoAnInstall::FeatureBattleStats)) { OpenBattleInfo(window); return 0; }
    }
    if (message == WM_MOUSEWHEEL) WheelCommand(wParam);
    if (message == WM_NCDESTROY) {
        if (gDialog) DestroyWindow(gDialog);
        CloseGuide();
        CloseBattleInfo();
        CloseFeatureSettings();
        CloseSaveBrowser();
        RestoreSaveEnhancer();
        ShutdownScenarioLifecycle();
        ShutdownBattleRosterRuntime();
        ShutdownBattleStats();
        RestoreEngine();
        DisableRuntimeResourceProviders();
        RestoreProtectedGate();
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
    const bool accelerator = HasFeature(XiaoAnInstall::FeatureAccelerator);
    const bool stats = HasFeature(XiaoAnInstall::FeatureBattleStats);
    bool menuOk = true;
    if (accelerator) menuOk = AppendMenuW(popup, MF_STRING, kConfigure, L"配置补丁") &&
        AppendMenuW(popup, MF_STRING, kSave, L"保存配置");
    if (menuOk && (accelerator || stats)) menuOk = AppendMenuW(popup, MF_STRING, kFeatures, L"功能设置") != FALSE;
    if (menuOk && stats) menuOk = AppendMenuW(popup, MF_STRING, kSaveBrowser, L"存档浏览") != FALSE;
    if (menuOk && (accelerator || stats)) menuOk = AppendMenuW(popup, MF_SEPARATOR, 0, nullptr) != FALSE;
    if (menuOk) menuOk = AppendMenuW(popup, MF_STRING, kUninstall, L"卸载补丁") != FALSE;
    if (!menuOk) {
        DestroyMenu(popup);
        return;
    }
    const int originalMenuCount = GetMenuItemCount(parent);
    if (!AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(popup), L"加速补丁")) {
        DestroyMenu(popup);
        return;
    }
    int added = 1;
    if (HasFeature(XiaoAnInstall::FeatureGuide)) {
        menuOk = AppendMenuW(parent, MF_STRING, kGuide, L"简易攻略") != FALSE;
        if (menuOk) ++added;
    }
    if (menuOk && stats) {
        menuOk = AppendMenuW(parent, MF_STRING, kBattleInfo, L"战斗统计") != FALSE;
        if (menuOk) ++added;
    }
    if (!menuOk) {
        for (int position = added - 1; position >= 0; --position)
            RemoveMenu(parent, originalMenuCount + position, MF_BYPOSITION);
        DestroyMenu(popup);
        return;
    }
    if (!SetWindowSubclass(window, MainProc, kSubclass, 0)) {
        for (int position = added - 1; position >= 0; --position)
            RemoveMenu(parent, originalMenuCount + position, MF_BYPOSITION);
        DestroyMenu(popup);
        DrawMenuBar(window);
        return;
    }
    gMainWindow = window;
    gParentMenu = parent;
    gPatchMenu = popup;
    DrawMenuBar(window);
    if (!gInitialized && accelerator) {
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
        return gNativeDispatchA ? gNativeDispatchA(message) : DispatchMessageA(message);
    return gNativeDispatchW ? gNativeDispatchW(message) : DispatchMessageW(message);
}

LRESULT WINAPI PatchedDispatch(const MSG* message) {
    if (!AwaitStartup() || !StartupUiAvailable()) return DispatchMessageForWindow(message);
    if (message) {
        AttachWindow(message->hwnd);
        if (gMainWindow) {
            const ULONGLONG now = GetTickCount64();
            if (HasFeature(XiaoAnInstall::FeatureBattleStats)) {
                PollSaveEnhancer();
                PollBattleRosterRuntime(now);
                PollBattleStats(now);
            }
            if (!InterlockedCompareExchange(&gUninstalling, FALSE, FALSE) &&
                HasFeature(XiaoAnInstall::FeatureAccelerator) &&
                gCompatible && gAddProcess && now - gLastModuleCheck > 1000) {
                gLastModuleCheck = now;
                gAddProcess(GetCurrentProcess());
            }
        }
    }
    return DispatchMessageForWindow(message);
}

bool ScenarioBasenameA(const char* path, char (&name)[32]) {
    if (!path) return false;
    const char* basename = path;
    size_t length = 0;
    for (; length < 32768 && path[length]; ++length)
        if (path[length] == '\\' || path[length] == '/') basename = path + length + 1;
    if (length == 32768 || path + length - basename < 7 || path + length - basename >= 32) return false;
    if ((basename[0] != 'R' && basename[0] != 'r' && basename[0] != 'S' && basename[0] != 's') ||
        basename[1] != '_') return false;
    const char* digit = basename + 2;
    unsigned number = 0;
    if (*digit < '0' || *digit > '9') return false;
    do {
        number = number * 10 + static_cast<unsigned>(*digit - '0');
        if (number > 999) return false;
        ++digit;
    } while (*digit >= '0' && *digit <= '9');
    if (path + length - digit != 4 || digit[0] != '.' ||
        (digit[1] != 'e' && digit[1] != 'E') || (digit[2] != 'e' && digit[2] != 'E') ||
        (digit[3] != 'x' && digit[3] != 'X')) return false;
    // Keep the original digits; the guide canonicalizes them after activation.
    const size_t bytes = static_cast<size_t>(path + length - basename) + 1;
    memcpy(name, basename, bytes);
    return true;
}

void CaptureScenarioOpen(const char* path, HANDLE result) {
    if (result == INVALID_HANDLE_VALUE) return;
    char name[32] = {};
    if (!ScenarioBasenameA(path, name)) return;
    AcquireSRWLockExclusive(&gScenarioCaptureLock);
    if (gScenarioCaptureState == ScenarioCaptureState::Pending)
        memcpy(gPendingScenario, name, sizeof(gPendingScenario));
    else if (gScenarioCaptureState == ScenarioCaptureState::Active)
        ObserveGuideScenarioPathA(name);
    ReleaseSRWLockExclusive(&gScenarioCaptureLock);
}

void CompleteScenarioCapture(bool accepted) {
    AcquireSRWLockExclusive(&gScenarioCaptureLock);
    if (accepted) {
        // Replay under the same lock as subsequent observations, so an older
        // pending name can never replace a newer active observation.
        if (gScenarioCaptureState == ScenarioCaptureState::Pending && gPendingScenario[0])
            ObserveGuideScenarioPathA(gPendingScenario);
        gScenarioCaptureState = ScenarioCaptureState::Active;
    } else {
        gScenarioCaptureState = ScenarioCaptureState::Rejected;
    }
    memset(gPendingScenario, 0, sizeof(gPendingScenario));
    ReleaseSRWLockExclusive(&gScenarioCaptureLock);
}

#ifdef XA66_TEST
using CreateFileATestFn = decltype(&CreateFileA);
CreateFileATestFn gTestCreateFileA = nullptr;
#endif
HANDLE WINAPI PatchedNativeCreateFileA(LPCSTR fileName, DWORD desiredAccess, DWORD shareMode,
    LPSECURITY_ATTRIBUTES security, DWORD creationDisposition, DWORD flags, HANDLE templateFile) {
    // Preserve the protector's virtual-file wrapper, including embedded game
    // resources; forwarding directly to Win32 would bypass those resources.
    const auto forward = gNativeCreateFile ? gNativeCreateFile : &CreateFileA;
    HANDLE result = forward(fileName, desiredAccess, shareMode, security,
        creationDisposition, flags, templateFile);
    const DWORD error = GetLastError();
    CaptureScenarioOpen(fileName, result);
    SetLastError(error);
    return result;
}

HANDLE WINAPI PatchedCreateFileA(LPCSTR fileName, DWORD desiredAccess, DWORD shareMode,
    LPSECURITY_ATTRIBUTES security, DWORD creationDisposition, DWORD flags,
    HANDLE templateFile) {
    // Only the game's IAT slot is replaced.  This call uses the proxy DLL's
    // own kernel32 import and therefore cannot recurse through this hook.
    auto forward = &CreateFileA;
#ifdef XA66_TEST
    if (gTestCreateFileA) forward = gTestCreateFileA;
#endif
    HANDLE result = forward(fileName, desiredAccess, shareMode, security, creationDisposition,
        flags, templateFile);
    const DWORD error = GetLastError();
    CaptureScenarioOpen(fileName, result);
    SetLastError(error);
    return result;
}

BOOL WINAPI PatchedTranslate(const MSG* message) {
    if (!AwaitStartup() || !StartupUiAvailable())
        return gNativeTranslate ? gNativeTranslate(message) : TranslateMessage(message);
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
    return gNativeTranslate ? gNativeTranslate(message) : TranslateMessage(message);
}

HANDLE WINAPI PatchedNativeCreateFileW(LPCWSTR fileName, DWORD desiredAccess, DWORD shareMode,
    LPSECURITY_ATTRIBUTES security, DWORD creationDisposition, DWORD flags, HANDLE templateFile) {
    const auto forward = gNativeCreateFileW ? gNativeCreateFileW : &CreateFileW;
    HANDLE result = forward(fileName, desiredAccess, shareMode, security,
        creationDisposition, flags, templateFile);
    const DWORD error = GetLastError();
    if (result != INVALID_HANDLE_VALUE && fileName) {
        char path[32768] = {};
        const int length = WideCharToMultiByte(CP_ACP, 0, fileName, -1, path,
            static_cast<int>(_countof(path)), nullptr, nullptr);
        if (length > 0 && length < static_cast<int>(_countof(path))) CaptureScenarioOpen(path, result);
    }
    SetLastError(error);
    return result;
}

LRESULT WINAPI PatchedNativeDispatchA(const MSG* message) {
    if (!AwaitStartup() || !StartupUiAvailable())
        return gNativeDispatchA ? gNativeDispatchA(message) : DispatchMessageA(message);
    return PatchedDispatch(message);
}

LRESULT WINAPI PatchedNativeDispatchW(const MSG* message) {
    if (!AwaitStartup() || !StartupUiAvailable())
        return gNativeDispatchW ? gNativeDispatchW(message) : DispatchMessageW(message);
    return PatchedDispatch(message);
}

bool LayoutRuntimeReady(const RuntimeImageView& view, EngineProfileId profile,
    bool allowVersionFallback, EngineDetection& detection, std::wstring& error) {
    if (DetectEngineProfileRuntime(reinterpret_cast<const BYTE*>(view.RuntimeBase()),
        view.Size(), detection, error))
        return detection.profile == profile;
    if (!allowVersionFallback) return false;
    EngineLayout layout;
    if (!ResolveVersionEngineLayout(view.Data(), view.Size(), profile, layout,
        view.RuntimeBase())) return false;
    for (const EngineSitePlan& site : layout.plan.sites) {
        if (!site.required || !site.present) continue;
        const uintptr_t address = view.RuntimeBase() + site.rva;
        std::wstring pageError;
        if (!ValidateRuntimeMemory(reinterpret_cast<const void*>(address), site.length,
            true, pageError)) {
            error = L"版本模板站点不在已提交可执行页面：" + pageError;
            return false;
        }
    }
    detection = EngineDetection();
    detection.profile = profile;
    detection.versionFallback = true;
    detection.container = L"Packed";
    detection.layoutStatus = L"VersionSelected";
    detection.packedContainer = true;
    detection.planReport = EnginePatchPlanReport(layout.plan);
    detection.evidence = L"按已确认的版本模板捕获了可执行运行时页面中的原始字节。";
    error.clear();
    return true;
}

bool ResolveStableRuntime(EngineProfileId profile, bool versionFallback,
    unsigned requestedFeatures, EngineDetection& detection,
    RuntimeNativeImportPlan& selectedImports, unsigned& readyFeatures,
    size_t& imageSize, std::wstring& error) {
    RuntimeStartupTracker tracker(GetTickCount64(), requestedFeatures);
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const HMODULE user = GetModuleHandleW(L"user32.dll");
    RuntimeNativeImportTargets targets;
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::CreateFileA)] =
        reinterpret_cast<void*>(GetProcAddress(kernel, "CreateFileA"));
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::CreateFileW)] =
        reinterpret_cast<void*>(GetProcAddress(kernel, "CreateFileW"));
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::TranslateMessage)] =
        reinterpret_cast<void*>(GetProcAddress(user, "TranslateMessage"));
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::DispatchMessageA)] =
        reinterpret_cast<void*>(GetProcAddress(user, "DispatchMessageA"));
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::DispatchMessageW)] =
        reinterpret_cast<void*>(GetProcAddress(user, "DispatchMessageW"));
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::GetTickCount)] =
        reinterpret_cast<void*>(GetProcAddress(kernel, "GetTickCount"));
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::SetTimer)] =
        reinterpret_cast<void*>(GetProcAddress(user, "SetTimer"));
    const unsigned uiFeatures = XiaoAnInstall::FeatureGuide | XiaoAnInstall::FeatureBattleStats;
    const unsigned fileMask = RuntimeNativeImportBit(RuntimeNativeImportKind::CreateFileA) |
        RuntimeNativeImportBit(RuntimeNativeImportKind::CreateFileW);
    const unsigned dispatchMask = RuntimeNativeImportBit(RuntimeNativeImportKind::DispatchMessageA) |
        RuntimeNativeImportBit(RuntimeNativeImportKind::DispatchMessageW);
    bool haveSnapshot = false;
    RuntimeStartupStage previousStage = RuntimeStartupStage::Failed;
    while (!tracker.Finished()) {
        if (!tracker.ShouldPoll(GetTickCount64())) { Sleep(RuntimeStartupTracker::PollIntervalMs); continue; }
        RuntimeImageView view;
        std::wstring captureError;
        const bool imageReady = view.Capture(reinterpret_cast<const BYTE*>(gGameBase), 0, captureError);
        EngineDetection candidateDetection;
        std::wstring layoutError;
        const bool layoutReady = imageReady && LayoutRuntimeReady(view, profile,
            versionFallback, candidateDetection, layoutError);
        RuntimeNativeImportPlan candidateImports;
        std::wstring importError;
        const bool importScan = imageReady && ResolveRuntimeNativeImports(reinterpret_cast<const BYTE*>(gGameBase),
            view.Size(), view.RuntimeBase(), targets, candidateImports, importError);
        unsigned available = 0;
        const bool dispatchReady = importScan && (candidateImports.capabilities & dispatchMask);
        if (layoutReady && dispatchReady && (requestedFeatures & XiaoAnInstall::FeatureAccelerator))
            available |= XiaoAnInstall::FeatureAccelerator;
        const bool fileReady = importScan && (candidateImports.capabilities & fileMask);
        const bool uiReady = dispatchReady && fileReady &&
            (candidateImports.capabilities & RuntimeNativeImportBit(RuntimeNativeImportKind::TranslateMessage)) &&
            (candidateImports.capabilities & dispatchMask);
        if (layoutReady && uiReady) available |= requestedFeatures & uiFeatures;
        RuntimeStartupObservation observation;
        observation.imageReady = imageReady;
        // Import discovery has a definitive result for each captured image;
        // missing capabilities are reported as partial support after two
        // stable snapshots instead of keeping the game blocked.
        observation.importsReady = imageReady;
        observation.layoutReady = layoutReady;
        observation.importFingerprint = importScan ? candidateImports.fingerprint : 0;
        observation.layoutFingerprint = layoutReady ? std::hash<std::wstring>{}(candidateDetection.planReport) : 0;
        observation.availableFeatures = available;
        const RuntimeStartupStage stage = tracker.Observe(GetTickCount64(), observation);
        if (stage != previousStage) RecordRuntimeStartup(stage == RuntimeStartupStage::WaitingForImage ? L"runtime-wait-image" :
            stage == RuntimeStartupStage::ImportsReady ? L"runtime-wait-imports" :
            stage == RuntimeStartupStage::LayoutReady ? L"runtime-wait-stable-layout" :
            stage == RuntimeStartupStage::FeaturesReady ? L"runtime-ready" :
            stage == RuntimeStartupStage::Partial ? L"runtime-partial" :
            stage == RuntimeStartupStage::TimedOut ? L"runtime-timeout" : L"runtime-failed",
            layoutReady ? candidateDetection.evidence :
                (layoutError.empty() ? captureError : layoutError));
        previousStage = stage;
        if (imageReady) {
            imageSize = view.Size();
            haveSnapshot = true;
        }
        if (layoutReady && (stage == RuntimeStartupStage::FeaturesReady ||
            stage == RuntimeStartupStage::Partial)) {
            detection = candidateDetection;
            selectedImports = candidateImports;
            if (!importError.empty()) error = importError;
        }
        if (tracker.Finished()) break;
        Sleep(RuntimeStartupTracker::PollIntervalMs);
    }
    readyFeatures = tracker.ReadyFeatures() & requestedFeatures;
    if ((tracker.Stage() != RuntimeStartupStage::FeaturesReady &&
         tracker.Stage() != RuntimeStartupStage::Partial) || !readyFeatures || !haveSnapshot) {
        error = L"运行时等待结束，但没有可安全启用的已验证功能。";
        return false;
    }
    if ((readyFeatures & XiaoAnInstall::FeatureAccelerator) &&
        detection.profile != profile) {
        error = L"运行时布局与安装载荷版本不一致。";
        return false;
    }
    return true;
}

void UpdateCompatibilityStatus(bool packed, const EngineDetection& detection,
    unsigned readyFeatures, unsigned partialFeatures, const std::wstring& detail) {
    XiaoAnInstall::RuntimeStatus status;
    status.container = packed && detection.container == L"Standard" ? L"Packed" : detection.container;
    status.layout = detection.layoutStatus == L"Verified" || detection.layoutStatus == L"AutoVerified" ||
        detection.layoutStatus == L"RuntimeValidated" ?
        L"Verified" : detection.layoutStatus == L"VersionSelected" ? L"VersionSelected" : L"Unresolved";
    status.startup = (readyFeatures & gRequestedFeatures) == gRequestedFeatures && !partialFeatures ? L"Ready" :
        readyFeatures ? L"Partial" : L"Failed";
    const auto moduleStatus = [&](unsigned feature) {
        return !(gRequestedFeatures & feature) ? L"Disabled" :
            !(readyFeatures & feature) ? L"Unsupported" :
            (partialFeatures & feature) ? L"Partial" : L"Ready";
    };
    status.accelerator = moduleStatus(XiaoAnInstall::FeatureAccelerator);
    status.guide = moduleStatus(XiaoAnInstall::FeatureGuide);
    status.stats = moduleStatus(XiaoAnInstall::FeatureBattleStats);
    status.detail = detail;
    std::wstring error;
    if (!XiaoAnInstall::UpdateRuntimeStatus(gDirectory, CompiledEngineProfile(), status, error) &&
        !error.empty()) RecordRuntimeStartup(L"status-write-failed", error);
}

void RejectScenarioCapture() {
    CompleteScenarioCapture(false);
    DisableRuntimeResourceProviders();
    RestoreProtectedGate();
}

bool CommitNativeRuntimePlan(RuntimeNativeImportPlan& plan, size_t imageSize,
    std::wstring& error) {
    const auto index = [](RuntimeNativeImportKind kind) { return static_cast<size_t>(kind); };
    gNativeCreateFile = reinterpret_cast<NativeCreateFileFn>(
        plan.sites[index(RuntimeNativeImportKind::CreateFileA)].original);
    gNativeCreateFileW = reinterpret_cast<NativeCreateFileWFn>(
        plan.sites[index(RuntimeNativeImportKind::CreateFileW)].original);
    gNativeDispatchA = reinterpret_cast<NativeDispatchAFn>(
        plan.sites[index(RuntimeNativeImportKind::DispatchMessageA)].original);
    gNativeDispatchW = reinterpret_cast<NativeDispatchWFn>(
        plan.sites[index(RuntimeNativeImportKind::DispatchMessageW)].original);
    gNativeTranslate = reinterpret_cast<NativeTranslateFn>(
        plan.sites[index(RuntimeNativeImportKind::TranslateMessage)].original);
    plan.sites[index(RuntimeNativeImportKind::CreateFileA)].replacement =
        reinterpret_cast<void*>(&PatchedNativeCreateFileA);
    plan.sites[index(RuntimeNativeImportKind::CreateFileW)].replacement =
        reinterpret_cast<void*>(&PatchedNativeCreateFileW);
    plan.sites[index(RuntimeNativeImportKind::DispatchMessageA)].replacement =
        reinterpret_cast<void*>(&PatchedNativeDispatchA);
    plan.sites[index(RuntimeNativeImportKind::DispatchMessageW)].replacement =
        reinterpret_cast<void*>(&PatchedNativeDispatchW);
    plan.sites[index(RuntimeNativeImportKind::TranslateMessage)].replacement =
        reinterpret_cast<void*>(&PatchedTranslate);
    gRuntimeImageSize = imageSize;
    const bool committed = CommitRuntimeNativeImports(reinterpret_cast<BYTE*>(gGameBase),
        imageSize, plan, error);
    // Keep the journal even when rollback is pending after a failed commit.
    gRuntimeNativePlan = plan;
    gRuntimeNativeGate = false;
    for (const auto& site : plan.sites)
        gRuntimeNativeGate = gRuntimeNativeGate || site.committed ||
            site.protectionPending || site.flushPending;
    if (!committed || !RuntimeNativeImportsStillOwned(reinterpret_cast<const BYTE*>(gGameBase),
        imageSize, plan)) {
        RestoreProtectedGate();
        return false;
    }
    for (size_t i = 0; i < plan.sites.size(); ++i) {
        const auto& site = plan.sites[i];
        if (!site.present) continue;
        wchar_t line[256] = {};
        swprintf_s(line, L"kind=%u slot=0x%08X original=0x%08X callers=%u committed=%u",
            static_cast<unsigned>(i), static_cast<unsigned>(site.slotOffset),
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(site.original)),
            site.callReferences, site.committed ? 1u : 0u);
        RecordRuntimeStartup(L"native-import", line);
    }
    return true;
}

DWORD InitializeWorker() {
    const INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    gGameBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    GetModuleFileNameW(gInstance, gDirectory, static_cast<DWORD>(_countof(gDirectory)));
    wchar_t* slash = wcsrchr(gDirectory, L'\\');
    if (!slash) { RejectScenarioCapture(); return 0; }
    *slash = 0;
    EngineDetection detection, diskDetection;
    std::wstring error;
    XiaoAnInstall::Manifest installedManifest;
    const bool hasManifest = XiaoAnInstall::FileExists(
        XiaoAnInstall::ManifestPath(gDirectory, CompiledEngineProfile()));
    bool versionFallback = false;
    if (hasManifest) {
        if (!XiaoAnInstall::LoadManifest(gDirectory, installedManifest, error) ||
            installedManifest.profile != CompiledEngineProfile()) {
            RejectScenarioCapture();
            MessageBoxW(nullptr, L"补丁清单无法确认，已停用可选功能。请运行安装器修复。",
                L"加速补丁", MB_ICONERROR);
            return 0;
        }
        versionFallback = installedManifest.versionFallback;
    }
    if (!LoadInstalledFeatures(gDirectory, CompiledEngineProfile()) ||
        (hasManifest && gInstalledFeatures != installedManifest.featureMask)) {
        RejectScenarioCapture();
        MessageBoxW(nullptr, L"补丁功能状态与清单不一致，已停用可选功能。请运行安装器修复。",
            L"加速补丁", MB_ICONERROR);
        return 0;
    }
    gRequestedFeatures = gInstalledFeatures;
    const bool diskKnown = versionFallback ?
        DetectVersionEngineProfile(gDirectory, diskDetection, error, CompiledEngineProfile()) :
        DetectEngineProfile(gDirectory, diskDetection, error);
    std::wstring planHash;
    if (!diskKnown || diskDetection.profile != CompiledEngineProfile() ||
        (hasManifest && !installedManifest.executableSha256.empty() &&
            _wcsicmp(installedManifest.executableSha256.c_str(), diskDetection.exeSha256.c_str())) ||
        (hasManifest && !versionFallback && !installedManifest.planSha256.empty() &&
            (!XiaoAnInstall::Sha256File(PatchPrivatePath(gDirectory, CompiledEngineProfile()) +
                L"\\engine-plan.txt", planHash, error) ||
             _wcsicmp(installedManifest.planSha256.c_str(), planHash.c_str())))) {
        RecordRuntimeStartup(L"disk-identity-failed", error);
        RejectScenarioCapture();
        MessageBoxW(nullptr, L"主程序、载荷或安装计划与清单不一致，已停用可选功能。请运行安装器重新检测。",
            L"加速补丁", MB_ICONERROR);
        return 0;
    }
    const bool packed = diskDetection.packedContainer || diskDetection.requiresRuntimeValidation;
    const bool runtimeRequired = packed || versionFallback;
    const bool protected220 = diskDetection.variant == L"Protected220";
    RecordRuntimeStartup(L"proxy-loaded", L"外部音频代理已加载；后台检查映像，游戏调用保持透明转发。");
    RuntimeNativeImportPlan imports;
    unsigned readyFeatures = 0;
    size_t imageSize = 0;
    if (!ResolveStableRuntime(CompiledEngineProfile(), versionFallback, gRequestedFeatures,
        detection, imports, readyFeatures, imageSize, error) ||
        !CommitNativeRuntimePlan(imports, imageSize, error)) {
        detection.container = diskDetection.container;
        RecordRuntimeStartup(L"runtime-profile-check-failed", error);
        UpdateCompatibilityStatus(packed, detection, 0, 0, error);
        if (runtimeRequired) {
            std::wstring stateError;
            XiaoAnInstall::UpdateRuntimeValidation(gDirectory, CompiledEngineProfile(), L"Failed", stateError);
        }
        RejectScenarioCapture();
        CompleteStartup();
        MessageBoxW(nullptr, (L"补丁未能确认所需运行时入口，已保留音频并停止可选功能。\r\n" + error +
            L"\r\n详细结果见补丁 diagnostics\\runtime.log。").c_str(), L"加速补丁", MB_ICONWARNING);
        return 0;
    }
    detection.container = diskDetection.container;
    detection.packedContainer = packed;
    gInstalledFeatures = readyFeatures;
    unsigned partialFeatures = 0;
    std::wstring summary;
    const auto note = [&](const std::wstring& text) {
        if (!summary.empty()) summary += L"\r\n";
        summary += text;
        RecordRuntimeStartup(L"module-detail", text);
    };
    swprintf_s(gConfigPath, L"%s\\pl_06.ini", PatchPrivatePath(gDirectory, detection.profile).c_str());
    LoadFeatureSettings(gDirectory);
    const bool resourcesReady = InitializeRuntimeResourceProviders(protected220, detection.profile);
    if (HasFeature(XiaoAnInstall::FeatureGuide) && !resourcesReady) {
        partialFeatures |= XiaoAnInstall::FeatureGuide;
        note(L"攻略名称表尚未就绪，保留磁盘名称和人物编号回退，并按需重新检测。");
    }
    if (HasFeature(XiaoAnInstall::FeatureBattleStats)) {
        InitializeStatsDiagnostics(gDirectory);
        InitializeBattleStats(gDirectory);
        std::wstring recoveryError;
        const bool recoveryReady = RecoverSaveTransactions(gDirectory, recoveryError);
        SetSaveRecoveryReady(recoveryReady, recoveryError.c_str());
        const bool lifecycleReady = InitializeScenarioLifecycle();
        const bool rosterReady = InitializeBattleRosterRuntime();
        if (!rosterReady) SetBattleRosterCapability(false, L"武将加入调用链未确认，保留历史并暂停全局采集。");
        const bool saveReady = recoveryReady && InitializeSaveEnhancer(gDirectory);
        BattleStatsSnapshot stats;
        const bool modelReady = ReadBattleStatsSnapshot(stats) && stats.hookAvailable &&
            stats.physicalSettlementAvailable && stats.strategyDamageAvailable &&
            stats.recoveryAvailable && stats.criticalAvailable && stats.comboAvailable &&
            stats.attackBlockAvailable && stats.spellBlockAvailable &&
            stats.physicalReceivedAvailable && stats.strategyReceivedAvailable;
        if (!resourcesReady || !recoveryReady || !lifecycleReady || !rosterReady || !saveReady || !modelReady) {
            partialFeatures |= XiaoAnInstall::FeatureBattleStats;
            note(L"统计部分能力未确认；不可用指标显示 --，不替代游戏原生保存。" + recoveryError);
        }
        if (packed) {
            SetBattleStatsStatus(L"保护层展开后才启用监听，本次启动前期事件可能缺失。", true);
            partialFeatures |= XiaoAnInstall::FeatureBattleStats;
            note(L"保护层展开前的实际事件无法追溯，统计历史标为不完整，不补造早期事件。");
        }
        if (!lifecycleReady) SetBattleStatsStatus(L"场景生命周期未确认，无法确认本场开始和结束。", true);
    }
    if (HasFeature(XiaoAnInstall::FeatureAccelerator)) {
        SetNativeToggleHandler(&NativeSuperCommand);
        gCompatible = InitializeEngine(versionFallback);
        RecordRuntimeStartup(gCompatible ? L"engine-ready" : L"engine-failed", EngineError());
        if (!gCompatible) {
            readyFeatures &= ~XiaoAnInstall::FeatureAccelerator;
            gInstalledFeatures = readyFeatures;
            note(EngineError());
        }
    }
    if (HasFeature(XiaoAnInstall::FeatureAccelerator) && gCompatible) {
        wchar_t path[32768];
        swprintf_s(path, L"%s\\GearNtKe.dll", gDirectory);
        gGearModule = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (gGearModule) {
            gAddProcess = reinterpret_cast<AddProcessFn>(GetProcAddress(gGearModule, "AddProcess"));
            gSetSpeed = reinterpret_cast<SetSpeedFn>(GetProcAddress(gGearModule, "SetSpeed"));
            gUnIntercept = reinterpret_cast<UnInterceptFn>(GetProcAddress(gGearModule, "UnIntercept"));
            gConfigureNativeClockSlots = reinterpret_cast<ConfigureNativeClockSlotsFn>(
                GetProcAddress(gGearModule, "ConfigureNativeClockSlots"));
            const auto& tick = imports.sites[static_cast<size_t>(RuntimeNativeImportKind::GetTickCount)];
            const auto& timer = imports.sites[static_cast<size_t>(RuntimeNativeImportKind::SetTimer)];
            if (!gConfigureNativeClockSlots || !imports.clockTickSlotRva || !imports.clockTimerSlotRva ||
                !gConfigureNativeClockSlots(reinterpret_cast<HMODULE>(gGameBase),
                    imports.clockTickSlotRva, imports.clockTimerSlotRva, tick.original, timer.original)) {
                gAddProcess = nullptr;
                gSetSpeed = nullptr;
                gUnIntercept = nullptr;
            }
        }
        const bool gearCommitted = gAddProcess && gSetSpeed && gUnIntercept &&
            gAddProcess(GetCurrentProcess());
        if (!gearCommitted) {
            if (gUnIntercept) gUnIntercept();
            gAddProcess = nullptr;
            gSetSpeed = nullptr;
            partialFeatures |= XiaoAnInstall::FeatureAccelerator;
            note(L"齿轮原生时钟入口未确认，已停用齿轮；保留已验证的阶段与超级加速。");
        } else RecordRuntimeStartup(L"gear-ready", L"齿轮已验证并提交实际使用的原生时钟槽，初始保持原速。");
    }
    if ((readyFeatures & gRequestedFeatures) != gRequestedFeatures)
        note(L"部分所选功能缺少唯一且稳定的入口，未初始化相应模块。");
    if (runtimeRequired) {
        std::wstring stateError;
        // This field describes identity, layout and import verification.
        // Per-module readiness and history completeness are recorded separately.
        XiaoAnInstall::UpdateRuntimeValidation(gDirectory, CompiledEngineProfile(),
            L"Passed", stateError);
    }
    UpdateCompatibilityStatus(packed, detection, readyFeatures, partialFeatures, summary);
    CompleteScenarioCapture(HasFeature(XiaoAnInstall::FeatureGuide) || HasFeature(XiaoAnInstall::FeatureBattleStats));
    InterlockedExchange(&gStartupAccepted, readyFeatures ? TRUE : FALSE);
    CompleteStartup();
    if (partialFeatures || readyFeatures != gRequestedFeatures)
        MessageBoxW(nullptr, (L"补丁已按实际能力启用，以下项目尚未完全就绪：\r\n" + summary +
            L"\r\n详细结果见补丁 diagnostics\\runtime.log。").c_str(), L"加速补丁", MB_ICONWARNING);
    if (!HasFeature(XiaoAnInstall::FeatureAccelerator)) return 0;
    gKeyboardThread = GetCurrentThreadId();
    MSG bootstrap = {};
    PeekMessageW(&bootstrap, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    gKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, gInstance, 0);
    if (!gKeyboardHook) { gKeyboardThread = 0; return 0; }
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

DWORD WINAPI Initialize(void*) {
    gStartupThread = GetCurrentThreadId();
    struct StartupCompletion {
        ~StartupCompletion() { CompleteStartup(); }
    } completion;
    try {
        return InitializeWorker();
    } catch (...) {
        InterlockedExchange(&gStartupAccepted, FALSE);
        // Each subsystem owns its restoration journal. Attempt every rollback
        // even if one subsystem cannot finish restoring its own sites.
        try { RestoreSaveEnhancer(); } catch (...) {}
        try { ShutdownScenarioLifecycle(); } catch (...) {}
        try { ShutdownBattleRosterRuntime(); } catch (...) {}
        try { ShutdownBattleStats(); } catch (...) {}
        try { RestoreEngine(); } catch (...) {}
        try { if (gSetSpeed) gSetSpeed(0); } catch (...) {}
        try { if (gUnIntercept) gUnIntercept(); } catch (...) {}
        RejectScenarioCapture();
        CompleteStartup();
        MessageBoxW(nullptr, L"加速补丁初始化失败。游戏已解除启动等待，请重新启动后重试。",
            L"加速补丁", MB_ICONERROR);
        return 0;
    }
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
    wcscpy_s(privateFolder, PatchPrivatePath(gDirectory, CompiledEngineProfile()).c_str());
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
        // The event outlives all wrappers until process termination. DllMain
        // never waits for the worker or for a game thread.
        gStartupEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) gWithinLoaderCallout = reinterpret_cast<LoaderCalloutFn>(
            GetProcAddress(ntdll, "RtlIsThreadWithinLoaderCallout"));
        // No discovery, waiting or IAT writes under loader lock. The worker
        // observes the actual unpacked image before it installs any wrapper.
        HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        else CompleteStartup();
    }
    return TRUE;
}
