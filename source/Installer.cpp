#include "InstallCommon.h"
#include "ResourceIds.h"
#include "FeatureSelection.h"

#include <objbase.h>
#include <shellapi.h>
#include <cwctype>
#include <string>

namespace {

struct ForceSelection {
    std::wstring diagnostic;
    EngineProfileId profile = EngineProfileId::Unknown;
};

INT_PTR CALLBACK ForceSelectionProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* selection = reinterpret_cast<ForceSelection*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        selection = reinterpret_cast<ForceSelection*>(lParam);
        SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(selection));
        SetDlgItemTextW(window, IDC_FORCE_DIAGNOSTIC, selection->diagnostic.c_str());
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL) {
        EndDialog(window, IDCANCEL);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == IDOK && selection) {
        if (IsDlgButtonChecked(window, IDC_FORCE_64) == BST_CHECKED) selection->profile = EngineProfileId::Ccz64;
        else if (IsDlgButtonChecked(window, IDC_FORCE_65) == BST_CHECKED) selection->profile = EngineProfileId::Ccz65;
        else if (IsDlgButtonChecked(window, IDC_FORCE_66) == BST_CHECKED) selection->profile = EngineProfileId::Ccz66;
        else {
            MessageBoxW(window, L"请明确选择 6.4、6.5 或 6.6 模板。", L"强行注入", MB_OK | MB_ICONINFORMATION);
            return TRUE;
        }
        EndDialog(window, IDOK);
        return TRUE;
    }
    return FALSE;
}

struct Arguments {
    std::wstring root;
    bool install = false;
    bool uninstall = false;
    bool quiet = false;
    bool featuresSpecified = false;
    bool invalidFeatures = false;
    unsigned features = XiaoAnInstall::FeatureAll;
};

bool ParseFeatureMask(const std::wstring& text, unsigned& mask) {
    if (text.empty()) return false;
    bool numeric = true;
    for (wchar_t ch : text) if (ch < L'0' || ch > L'9') numeric = false;
    if (numeric) {
        wchar_t* end = nullptr;
        const unsigned long value = wcstoul(text.c_str(), &end, 10);
        if (!end || *end || value > XiaoAnInstall::FeatureAll) return false;
        mask = static_cast<unsigned>(value);
        return mask != XiaoAnInstall::FeatureNone;
    }
    unsigned result = XiaoAnInstall::FeatureNone;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find_first_of(L",;+|", start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring token = text.substr(start, end - start);
        while (!token.empty() && iswspace(token.front())) token.erase(token.begin());
        while (!token.empty() && iswspace(token.back())) token.pop_back();
        if (!_wcsicmp(token.c_str(), L"all") || token == L"全部") result |= XiaoAnInstall::FeatureAll;
        else if (!_wcsicmp(token.c_str(), L"accelerator") || !_wcsicmp(token.c_str(), L"accel") || token == L"加速")
            result |= XiaoAnInstall::FeatureAccelerator;
        else if (!_wcsicmp(token.c_str(), L"guide") || token == L"攻略" || token == L"简易攻略")
            result |= XiaoAnInstall::FeatureGuide;
        else if (!_wcsicmp(token.c_str(), L"stats") || !_wcsicmp(token.c_str(), L"battle") ||
                 !_wcsicmp(token.c_str(), L"battlestats") || token == L"统计" || token == L"战场统计")
            result |= XiaoAnInstall::FeatureBattleStats;
        else return false;
        start = end + 1;
    }
    mask = result;
    return mask != XiaoAnInstall::FeatureNone;
}

Arguments ParseArguments() {
    Arguments result;
    int count = 0;
    wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!values) return result;
    for (int i = 1; i < count; ++i) {
        if (!_wcsicmp(values[i], L"/install")) result.install = true;
        else if (!_wcsicmp(values[i], L"/uninstall")) result.uninstall = true;
        else if (!_wcsicmp(values[i], L"/quiet")) result.quiet = true;
        else if (!_wcsicmp(values[i], L"/features") && i + 1 < count) {
            result.featuresSpecified = true;
            if (!ParseFeatureMask(values[++i], result.features)) result.invalidFeatures = true;
        } else if (!_wcsnicmp(values[i], L"/features=", 10)) {
            result.featuresSpecified = true;
            if (!ParseFeatureMask(values[i] + 10, result.features)) result.invalidFeatures = true;
        }
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
    if (!quiet) MessageBoxW(nullptr, text.c_str(), L"加速补丁安装器", flags | MB_SETFOREGROUND);
}

bool LoadPayload(const EngineDetection& engine, XiaoAnInstall::PackagePayload& payload, std::wstring& error) {
    if (engine.profile != EngineProfileId::Ccz64 && engine.profile != EngineProfileId::Ccz65 && engine.profile != EngineProfileId::Ccz66) {
        error = L"没有与当前游戏版本匹配的补丁载荷。";
        return false;
    }
    payload.profile = engine.profile;
    payload.forced = engine.forced;
    payload.expectedExeSha256 = engine.exeSha256;
    HMODULE module = GetModuleHandleW(nullptr);
    const WORD proxyId = engine.profile == EngineProfileId::Ccz64 ? IDR_INSTALL_PROXY64 :
        engine.profile == EngineProfileId::Ccz65 ? IDR_INSTALL_PROXY65 : IDR_INSTALL_PROXY;
    return XiaoAnInstall::ReadResourceBytes(module, proxyId, payload.proxy, error) &&
           XiaoAnInstall::ReadResourceBytes(module, IDR_INSTALL_GEAR, payload.gear, error) &&
           XiaoAnInstall::ReadResourceBytes(module, IDR_INSTALL_CONFIG, payload.defaultConfig, error) &&
           XiaoAnInstall::ReadResourceBytes(module, IDR_INSTALL_FEATURES, payload.defaultFeatures, error);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Arguments arguments = ParseArguments();
    if (arguments.invalidFeatures || (arguments.featuresSpecified && !XiaoAnInstall::IsValidFeatureMask(arguments.features))) {
        Show(arguments.quiet, L"/features 参数无效。可使用 accelerator,guide,stats 的逗号分隔组合，或 all。", MB_ICONERROR);
        CoUninitialize();
        return 2;
    }
    if (arguments.install && arguments.uninstall) {
        Show(arguments.quiet, L"/install 与 /uninstall 不能同时使用。", MB_ICONERROR);
        CoUninitialize();
        return 2;
    }
    const bool useCurrentFolder = arguments.root.empty();
    if (useCurrentFolder) {
        arguments.root = ExecutableDirectory();
        if (arguments.root.empty() ||
            (!XiaoAnInstall::FileExists(XiaoAnInstall::JoinPath(arguments.root, L"Ekd5.exe")) &&
             !XiaoAnInstall::FileExists(XiaoAnInstall::ManifestPath(arguments.root, EngineProfileId::Ccz64)) &&
             !XiaoAnInstall::FileExists(XiaoAnInstall::ManifestPath(arguments.root, EngineProfileId::Ccz65)) &&
             !XiaoAnInstall::FileExists(XiaoAnInstall::ManifestPath(arguments.root, EngineProfileId::Ccz66)))) {
            Show(arguments.quiet, L"当前所在文件夹不是游戏根目录", MB_ICONERROR);
            CoUninitialize();
            return 3;
        }
    }
    arguments.root = XiaoAnInstall::CanonicalRoot(arguments.root);
    std::wstring detail;
    XiaoAnInstall::TargetKind kind = XiaoAnInstall::DetectTarget(arguments.root, detail);
    EngineProfileId forcedProfile = EngineProfileId::Unknown;
    if (kind == XiaoAnInstall::TargetKind::Invalid && !arguments.uninstall && !arguments.quiet) {
        std::wstring validation, hash;
        EngineDetection automatic;
        const bool engineUnknown = !DetectEngineProfile(arguments.root, automatic, validation);
        if (engineUnknown && XiaoAnInstall::ValidatePe32Executable(
            XiaoAnInstall::JoinPath(arguments.root, L"Ekd5.exe"), validation) &&
            XiaoAnInstall::Sha256File(XiaoAnInstall::JoinPath(arguments.root, L"Ekd5.exe"), hash, validation)) {
            ForceSelection selection;
            selection.diagnostic = detail + L"\r\n\r\n主程序 SHA-256：\r\n" + hash +
                L"\r\n\r\n继续后将使用所选版本的固定偏移。未知布局可能导致游戏崩溃。"
                L"安装前会校验 PE 和偏移边界，保留原始文件备份；卸载将恢复安装前状态。";
            if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_FORCE_SELECT), nullptr,
                ForceSelectionProcedure, reinterpret_cast<LPARAM>(&selection)) == IDOK) {
                // Resolve the selected template before the final confirmation so
                // the user can inspect the exact plan that will be written.  A
                // template choice alone is never treated as permission to blind
                // write: PE bounds, original bytes and all required sites still
                // have to pass the forced-mode checks.
                EngineDetection forcedDetection;
                std::wstring forcedError;
                if (!DetectForcedEngineProfile(arguments.root, selection.profile,
                    forcedDetection, forcedError)) {
                    Show(false, L"所选模板无法通过基础校验，强行注入已取消。\r\n" + forcedError,
                         MB_ICONERROR);
                    CoUninitialize();
                    return 3;
                }
                std::wstring plan = forcedDetection.planReport;
                if (plan.size() > 12000) plan.resize(12000);
                const std::wstring warning = std::wstring(L"确认强行使用 ") + EngineProfileName(selection.profile) +
                    L" 内置偏移模板？\r\n自动识别未通过，此操作不能保证游戏兼容。\r\n\r\n" +
                    L"所选模板的可审计计划（将写入清单并用于卸载恢复）：\r\n" + plan +
                    L"\r\n\r\n当前功能选择将在下一步确认；只有选择加速时才会启用引擎加速路径。";
                if (MessageBoxW(nullptr, warning.c_str(), L"再次确认强行注入",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                    forcedProfile = selection.profile;
                    kind = XiaoAnInstall::DetectTarget(arguments.root, detail, forcedProfile);
                }
            }
        }
    }
    if (kind == XiaoAnInstall::TargetKind::Invalid) {
        Show(arguments.quiet, detail, MB_ICONERROR);
        CoUninitialize();
        return 3;
    }

    bool uninstall = arguments.uninstall;
    if (!arguments.install && !arguments.uninstall) {
        if (kind == XiaoAnInstall::TargetKind::Installed) {
            const int choice = MessageBoxW(nullptr,
                (detail + L"\r\n选择\"是\"更新补丁，选择\"否\"卸载补丁，选择“取消”退出").c_str(),
                L"加速补丁安装器", MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
            if (choice == IDCANCEL) {
                CoUninitialize();
                return 1;
            }
            uninstall = choice == IDNO;
        } else {
            const int choice = MessageBoxW(nullptr,
                (L"已识别受支持的游戏版本，是否安装加速补丁？\r\n" + detail).c_str(),
                L"加速补丁安装器", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
            if (choice != IDYES) {
                CoUninitialize();
                return 1;
            }
        }
    }

    std::wstring report, error;
    if (!arguments.featuresSpecified && !arguments.quiet) {
        FeatureSelection selection;
        selection.uninstall = uninstall;
        if (kind == XiaoAnInstall::TargetKind::Installed) {
            XiaoAnInstall::Manifest manifest;
            if (XiaoAnInstall::LoadManifest(arguments.root, manifest, error)) {
                selection.mask = manifest.featureMask;
                selection.available = manifest.featureMask;
            }
        }
        if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_FEATURE_SELECT), nullptr,
            FeatureSelectionProcedure, reinterpret_cast<LPARAM>(&selection)) != IDOK) {
            CoUninitialize();
            return 1;
        }
        arguments.features = selection.mask;
        arguments.featuresSpecified = true;
    }
    if (!arguments.quiet) {
        std::wstring summary = uninstall ? L"即将卸载：" : L"即将安装/更新：";
        const unsigned mask = arguments.features;
        if (mask & XiaoAnInstall::FeatureAccelerator) summary += L"\r\n• 加速（含齿轮模块）";
        if (mask & XiaoAnInstall::FeatureGuide) summary += L"\r\n• 简易攻略";
        if (mask & XiaoAnInstall::FeatureBattleStats) summary += L"\r\n• 战场统计";
        summary += L"\r\n\r\n共享代理仅在最后一个功能卸载后恢复。是否继续？";
        if (MessageBoxW(nullptr, summary.c_str(), L"确认功能变更", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            CoUninitialize();
            return 1;
        }
    }
    bool success = false;
    if (uninstall) {
        if (kind != XiaoAnInstall::TargetKind::Installed) {
            error = L"此目录没有可用的安装清单，不能执行完整卸载。";
        } else if (!XiaoAnInstall::IsGameRunning(arguments.root, error)) {
            success = XiaoAnInstall::RestorePatch(arguments.root, report, error, true,
                arguments.featuresSpecified ? arguments.features : XiaoAnInstall::FeatureAll);
        }
    } else {
        EngineDetection engine;
        XiaoAnInstall::PackagePayload payload;
        if (forcedProfile == EngineProfileId::Unknown && kind == XiaoAnInstall::TargetKind::Installed) {
            XiaoAnInstall::Manifest manifest;
            if (XiaoAnInstall::LoadManifest(arguments.root, manifest, error) && manifest.forced)
                forcedProfile = manifest.profile;
        }
        const bool detected = forcedProfile == EngineProfileId::Unknown ?
            DetectEngineProfile(arguments.root, engine, error) :
            DetectForcedEngineProfile(arguments.root, forcedProfile, engine, error);
        if (detected && LoadPayload(engine, payload, error)) {
            success = XiaoAnInstall::InstallPatch(arguments.root, payload, kind, report, error,
                arguments.featuresSpecified ? arguments.features : XiaoAnInstall::FeatureAll);
        }
    }
    if (!success) Show(arguments.quiet, error, MB_ICONERROR);
    else Show(arguments.quiet, report, MB_ICONINFORMATION);
    CoUninitialize();
    return success ? 0 : 4;
}
