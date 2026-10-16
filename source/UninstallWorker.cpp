#include "InstallCommon.h"

#include <shellapi.h>
#include <string>
#include <vector>

namespace {

bool ParseArguments(std::wstring& root, HANDLE& process, bool& quiet) {
    int count = 0;
    wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!values) return false;
    process = nullptr;
    quiet = false;
    for (int i = 1; i < count; ++i) {
        if (!_wcsicmp(values[i], L"/root") && i + 1 < count) root = values[++i];
        else if (!_wcsicmp(values[i], L"/handle") && i + 1 < count) {
            wchar_t* end = nullptr;
            const unsigned long long raw = _wcstoui64(values[++i], &end, 10);
            if (!end || *end) {
                LocalFree(values);
                return false;
            }
            process = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw));
        } else if (!_wcsicmp(values[i], L"/quiet")) quiet = true;
    }
    LocalFree(values);
    root = XiaoAnInstall::CanonicalRoot(root);
    return !root.empty() && process;
}

void Show(bool quiet, const std::wstring& text, UINT flags) {
    if (!quiet) MessageBoxW(nullptr, text.c_str(), L"加速补丁卸载", flags | MB_SETFOREGROUND);
}

void ScheduleSelfDelete() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length && length < path.size()) MoveFileExW(path.data(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::wstring root;
    HANDLE process = nullptr;
    bool quiet = false;
    if (!ParseArguments(root, process, quiet)) {
        Show(quiet, L"卸载助手参数无效。", MB_ICONERROR);
        return 2;
    }
    const DWORD wait = WaitForSingleObject(process, 120000);
    CloseHandle(process);
    if (wait != WAIT_OBJECT_0) {
        const std::wstring reason = wait == WAIT_TIMEOUT ?
            L"等待游戏退出超过 120 秒。未强制结束游戏，也没有修改磁盘文件；可从游戏菜单重试。" :
            L"等待游戏进程失败，Windows 错误：" + std::to_wstring(GetLastError()) +
                L"。没有修改磁盘文件。";
        Show(quiet, reason, MB_ICONERROR);
        ScheduleSelfDelete();
        return 3;
    }
    std::wstring report, error;
    if (!XiaoAnInstall::RestorePatch(root, report, error, true)) {
        Show(quiet, L"卸载未完成。原始备份和安装清单已保留，可使用安装器的应急卸载入口重试。\r\n\r\n" + error,
             MB_ICONERROR);
        ScheduleSelfDelete();
        return 4;
    }
    Show(quiet, report, MB_ICONINFORMATION);
    ScheduleSelfDelete();
    return 0;
}
