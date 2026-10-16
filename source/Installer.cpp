#include "InstallCommon.h"
#include "ResourceIds.h"

#include <objbase.h>
#include <shellapi.h>
#include <string>

namespace {

struct Arguments {
    std::wstring root;
    bool install = false;
    bool uninstall = false;
    bool quiet = false;
};

Arguments ParseArguments() {
    Arguments result;
    int count = 0;
    wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!values) return result;
    for (int i = 1; i < count; ++i) {
        if (!_wcsicmp(values[i], L"/install")) result.install = true;
        else if (!_wcsicmp(values[i], L"/uninstall")) result.uninstall = true;
        else if (!_wcsicmp(values[i], L"/quiet")) result.quiet = true;
        else if (result.root.empty()) result.root = values[i];
    }
    LocalFree(values);
    return result;
}

std::wstring ExecutableDirectory() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path[length] = 0;
    wchar_t* slash = wcsrchr(path.data(), L'\\');
    if (!slash) return {};
    *slash = 0;
    return XiaoAnInstall::CanonicalRoot(path.data());
}

void Show(bool quiet, const std::wstring& text, UINT flags) {
    if (!quiet) MessageBoxW(nullptr, text.c_str(), L"加速补丁6.6安装器", flags | MB_SETFOREGROUND);
}

bool LoadPayload(XiaoAnInstall::PackagePayload& payload, std::wstring& error) {
    HMODULE module = GetModuleHandleW(nullptr);
    return XiaoAnInstall::ReadResourceBytes(module, IDR_INSTALL_PROXY, payload.proxy, error) &&
           XiaoAnInstall::ReadResourceBytes(module, IDR_INSTALL_GEAR, payload.gear, error) &&
           XiaoAnInstall::ReadResourceBytes(module, IDR_INSTALL_CONFIG, payload.defaultConfig, error) &&
           XiaoAnInstall::ReadResourceBytes(module, IDR_INSTALL_FEATURES, payload.defaultFeatures, error);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Arguments arguments = ParseArguments();
    if (arguments.install && arguments.uninstall) {
        Show(arguments.quiet, L"/install 与 /uninstall 不能同时使用。", MB_ICONERROR);
        CoUninitialize();
        return 2;
    }
    const bool useCurrentFolder = arguments.root.empty();
    if (useCurrentFolder) {
        arguments.root = ExecutableDirectory();
        if (arguments.root.empty() ||
            !XiaoAnInstall::FileExists(XiaoAnInstall::JoinPath(arguments.root, L"Ekd5.exe"))) {
            Show(arguments.quiet, L"当前所在文件夹不是游戏根目录", MB_ICONERROR);
            CoUninitialize();
            return 3;
        }
    }
    arguments.root = XiaoAnInstall::CanonicalRoot(arguments.root);
    std::wstring detail;
    XiaoAnInstall::TargetKind kind = XiaoAnInstall::DetectTarget(arguments.root, detail);
    if (kind == XiaoAnInstall::TargetKind::Invalid) {
        Show(arguments.quiet, detail, MB_ICONERROR);
        CoUninitialize();
        return 3;
    }

    bool uninstall = arguments.uninstall;
    if (!arguments.install && !arguments.uninstall) {
        if (kind == XiaoAnInstall::TargetKind::Installed) {
            const int choice = MessageBoxW(nullptr,
                L"已安装补丁，选择\"是\"更新补丁，选择\"否\"卸载补丁，选择“取消”退出",
                L"加速补丁6.6安装器", MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
            if (choice == IDCANCEL) {
                CoUninitialize();
                return 1;
            }
            uninstall = choice == IDNO;
        } else {
            const int choice = MessageBoxW(nullptr,
                L"是否安装安装补丁",
                L"加速补丁6.6安装器", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
            if (choice != IDYES) {
                CoUninitialize();
                return 1;
            }
        }
    }

    std::wstring report, error;
    bool success = false;
    if (uninstall) {
        if (kind != XiaoAnInstall::TargetKind::Installed) {
            error = L"此目录没有可用的安装清单，不能执行完整卸载。";
        } else if (!XiaoAnInstall::IsGameRunning(arguments.root, error)) {
            success = XiaoAnInstall::RestorePatch(arguments.root, report, error, true);
        }
    } else {
        XiaoAnInstall::PackagePayload payload;
        if (LoadPayload(payload, error)) success = XiaoAnInstall::InstallPatch(arguments.root, payload, kind, report, error);
    }
    if (!success) Show(arguments.quiet, error, MB_ICONERROR);
    else if (!uninstall)
        Show(arguments.quiet, L"补丁已完成安装，小暗祝您游玩开心哦！", MB_ICONINFORMATION);
    CoUninitialize();
    return success ? 0 : 4;
}
