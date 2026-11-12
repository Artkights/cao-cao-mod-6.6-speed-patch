#include "InstallCommon.h"
#include "ResourceIds.h"
#include "FeatureSelection.h"

#include <objbase.h>
#include <shellapi.h>
#include <cwctype>
#include <string>

namespace {

struct Arguments {
    std::wstring root;
    bool install = false;
    bool uninstall = false;
    bool diagnose = false;
    bool quiet = false;
    bool featuresSpecified = false;
    bool invalidFeatures = false;
    bool forceVersion = false;
    bool invalidForceVersion = false;
    EngineProfileId forceProfile = EngineProfileId::Unknown;
    unsigned features = XiaoAnInstall::FeatureAll;
};

struct VersionSelection {
    std::wstring root;
    EngineProfileId profile = EngineProfileId::Unknown;
};

INT_PTR CALLBACK VersionSelectionProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* selection = reinterpret_cast<VersionSelection*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        selection = reinterpret_cast<VersionSelection*>(lParam);
        SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(selection));
        const std::wstring note = L"将先备份主程序、音频 DLL、齿轮 DLL 和补丁配置，再按所选版本选择补丁载荷。"
            L"选择版本不等于引擎布局已确认；实际启动与各功能仍须单独验证，未通过时不会显示为已就绪。"
            L"版本选择错误可能导致游戏异常，可通过安装清单卸载恢复。\r\n\r\n备份保存在游戏目录的 patch\\xiaoan64、65 或 66\\backup 下。\r\n游戏目录：" + selection->root;
        SetDlgItemTextW(window, IDC_VERSION_NOTE, note.c_str());
        EnableWindow(GetDlgItem(window, IDOK), FALSE);
        return TRUE;
    }
    if (message != WM_COMMAND || !selection) return FALSE;
    const WORD id = LOWORD(wParam);
    if (id == IDC_VERSION_64 || id == IDC_VERSION_65 || id == IDC_VERSION_66) {
        selection->profile = id == IDC_VERSION_64 ? EngineProfileId::Ccz64 :
            id == IDC_VERSION_65 ? EngineProfileId::Ccz65 : EngineProfileId::Ccz66;
        EnableWindow(GetDlgItem(window, IDOK), TRUE);
        return TRUE;
    }
    if (id == IDCANCEL || (id == IDOK && selection->profile != EngineProfileId::Unknown)) {
        EndDialog(window, id);
        return TRUE;
    }
    return FALSE;
}

bool SelectMissingVersion(const Arguments& arguments, EngineDetection& detection,
    std::wstring& error, bool& acknowledged, bool& cancelled) {
    if (arguments.quiet || arguments.forceProfile != EngineProfileId::Unknown ||
        !XiaoAnInstall::ValidatePe32Executable(XiaoAnInstall::JoinPath(arguments.root, L"Ekd5.exe"), error)) return false;
    VersionSelection choice;
    choice.root = arguments.root;
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_VERSION_SELECT), nullptr,
        VersionSelectionProcedure, reinterpret_cast<LPARAM>(&choice)) != IDOK) {
        cancelled = true;
        error.clear();
        return false;
    }
    if (!DetectVersionEngineProfile(arguments.root, detection, error, choice.profile)) return false;
    acknowledged = true;
    return true;
}

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
        else if (!_wcsicmp(values[i], L"/diagnose")) result.diagnose = true;
        else if (!_wcsnicmp(values[i], L"/diagnose=", 10)) {
            result.diagnose = true;
            if (result.root.empty()) result.root = values[i] + 10;
        }
        else if (!_wcsicmp(values[i], L"/quiet")) result.quiet = true;
        else if (!_wcsicmp(values[i], L"/force-version")) result.forceVersion = true;
        else if (!_wcsnicmp(values[i], L"/force-version=", 15)) {
            result.forceVersion = true;
            wchar_t* end = nullptr;
            const long profile = wcstol(values[i] + 15, &end, 10);
            result.forceProfile = profile == 64 ? EngineProfileId::Ccz64 :
                profile == 65 ? EngineProfileId::Ccz65 : profile == 66 ? EngineProfileId::Ccz66 : EngineProfileId::Unknown;
            result.invalidForceVersion = !end || *end || result.forceProfile == EngineProfileId::Unknown;
        }
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
    payload.expectedExeSha256 = engine.exeSha256;
    payload.engineVariant = engine.variant.empty() ? L"Standard" : engine.variant;
    payload.runtimeValidation = engine.requiresRuntimeValidation ? L"Required" : L"NotRequired";
    payload.status.container = engine.container;
    payload.status.layout = engine.layoutStatus;
    payload.status.startup = L"Pending";
    payload.status.accelerator = L"Pending";
    payload.status.guide = L"Pending";
    payload.status.stats = L"Pending";
    payload.status.detail = engine.evidence;
    payload.versionFallback = engine.versionFallback;
    HMODULE module = GetModuleHandleW(nullptr);
    const WORD proxyId = engine.profile == EngineProfileId::Ccz64 ? IDR_INSTALL_PROXY64 :
        engine.profile == EngineProfileId::Ccz65 ? IDR_INSTALL_PROXY65 : IDR_INSTALL_PROXY;
    return XiaoAnInstall::ReadResourceBytes(module, proxyId, payload.proxy, error) &&
           XiaoAnInstall::ReadResourceBytes(module, IDR_INSTALL_GEAR, payload.gear, error) &&
           XiaoAnInstall::ReadResourceBytes(module, IDR_INSTALL_CONFIG, payload.defaultConfig, error) &&
           XiaoAnInstall::ReadResourceBytes(module, IDR_INSTALL_FEATURES, payload.defaultFeatures, error);
}

std::wstring BuildReadOnlyDiagnostic(const std::wstring& root, XiaoAnInstall::TargetKind target,
    const std::wstring& targetDetail, const EngineDetection* engine, const std::wstring& engineError,
    const XiaoAnInstall::Manifest* manifest) {
    std::wstring text = L"只读诊断：未写入游戏文件或安装清单。\r\n游戏目录：" + root + L"\r\n";
    text += L"目标状态：";
    text += target == XiaoAnInstall::TargetKind::Clean ? L"可识别的未安装游戏" :
        target == XiaoAnInstall::TargetKind::Legacy ? L"旧版安装" :
        target == XiaoAnInstall::TargetKind::Installed ? L"已安装" : L"未确认";
    text += L"\r\n" + targetDetail + L"\r\n";
    if (engine) {
        text += std::wstring(L"引擎版本：") + EngineProfileName(engine->profile) +
            L"\r\n容器：" + engine->container + L"\r\n布局状态：" + engine->layoutStatus +
            L"\r\n变体：" + (engine->variant.empty() ? L"Standard" : engine->variant) +
            L"\r\nSHA-256：" + engine->exeSha256 + L"\r\n置信度：" + std::to_wstring(engine->confidence) +
            L"\r\n证据：" + engine->evidence + L"\r\n";
        if (!engine->planReport.empty()) text += L"\r\n计划摘要：\r\n" + engine->planReport + L"\r\n";
    } else {
        text += L"引擎识别失败：" + engineError + L"\r\n";
    }
    if (manifest) {
        text += L"\r\n已记录运行状态：\r\n代理/启动：" + manifest->status.startup +
            L"\r\n加速：" + manifest->status.accelerator + L"\r\n简易攻略：" +
            manifest->status.guide + L"\r\n战场统计：" + manifest->status.stats;
        if (!manifest->status.detail.empty()) text += L"\r\n状态详情：" + manifest->status.detail;
        text += L"\r\n以上为清单中最后一次记录；本次诊断未启动游戏或代理。";
    } else {
        text += L"\r\n代理入口、游戏启动和各功能尚未验证；状态保持 Pending，不能据此视为已验证成功。";
    }
    return text;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Arguments arguments = ParseArguments();
    if (arguments.invalidForceVersion) {
        Show(arguments.quiet, L"/force-version 版本必须是 64、65 或 66。", MB_ICONERROR);
        CoUninitialize();
        return 2;
    }
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
    EngineDetection cachedDetection;
    bool hasCachedDetection = false;
    XiaoAnInstall::TargetKind kind = XiaoAnInstall::DetectTarget(arguments.root, detail, false,
        EngineProfileId::Unknown, nullptr, &cachedDetection);
    hasCachedDetection = !cachedDetection.exeSha256.empty();
    if (arguments.diagnose) {
        std::wstring diagnosticError;
        XiaoAnInstall::Manifest installedManifest;
        bool hasManifest = kind == XiaoAnInstall::TargetKind::Installed &&
            XiaoAnInstall::LoadManifest(arguments.root, installedManifest, diagnosticError);
        if (!hasCachedDetection && kind == XiaoAnInstall::TargetKind::Installed) {
            hasCachedDetection = DetectEngineProfile(arguments.root, cachedDetection, diagnosticError);
        } else if (!hasCachedDetection) diagnosticError = detail;
        Show(false, BuildReadOnlyDiagnostic(arguments.root, kind, detail,
            hasCachedDetection ? &cachedDetection : nullptr, diagnosticError,
            hasManifest ? &installedManifest : nullptr), MB_ICONINFORMATION);
        CoUninitialize();
        return hasCachedDetection ? 0 : 3;
    }
    EngineDetection fallbackDetection;
    bool usingVersionFallback = false;
    bool versionAcknowledged = false;
    bool versionSelectionCancelled = false;
    if (kind == XiaoAnInstall::TargetKind::Invalid && !arguments.uninstall) {
        std::wstring fallbackError;
        if (DetectVersionEngineProfile(arguments.root, fallbackDetection, fallbackError, arguments.forceProfile) ||
            SelectMissingVersion(arguments, fallbackDetection, fallbackError, versionAcknowledged, versionSelectionCancelled)) {
            usingVersionFallback = true;
            cachedDetection = fallbackDetection;
            hasCachedDetection = true;
            detail = std::wstring(L"无法通过自动引擎定位，但已读取版本资源为 ") +
                EngineProfileName(fallbackDetection.profile) + L"。\r\n" +
                L"此结果只用于选择对应载荷，不代表运行时布局或功能已经确认。";
            kind = XiaoAnInstall::DetectTarget(arguments.root, detail, true, fallbackDetection.profile,
                &fallbackDetection);
        } else if (!fallbackError.empty()) {
            detail += L"\r\n" + fallbackError;
        }
    }
    if (versionSelectionCancelled) {
        CoUninitialize();
        return 1;
    }
    bool existingVersionFallback = false;
    if (!arguments.uninstall && kind != XiaoAnInstall::TargetKind::Invalid && !usingVersionFallback) {
        EngineDetection automatic;
        std::wstring ignored;
        XiaoAnInstall::Manifest existing;
        existingVersionFallback = kind == XiaoAnInstall::TargetKind::Installed &&
            XiaoAnInstall::LoadManifest(arguments.root, existing, ignored) && existing.versionFallback;
        const bool automaticDetected = hasCachedDetection || DetectEngineProfile(arguments.root, automatic, ignored);
        if (hasCachedDetection) automatic = cachedDetection;
        if (automaticDetected && !hasCachedDetection) {
            cachedDetection = automatic;
            hasCachedDetection = true;
        }
        const bool protectedDisk = automaticDetected && automatic.requiresRuntimeValidation;
        if (existingVersionFallback || protectedDisk || arguments.forceVersion ||
            (!automaticDetected && kind == XiaoAnInstall::TargetKind::Installed)) {
            const EngineProfileId requested = arguments.forceProfile != EngineProfileId::Unknown ? arguments.forceProfile :
                existingVersionFallback ? existing.profile : automatic.profile;
            usingVersionFallback = DetectVersionEngineProfile(arguments.root, fallbackDetection, ignored, requested);
            if (!usingVersionFallback && kind != XiaoAnInstall::TargetKind::Installed)
                usingVersionFallback = SelectMissingVersion(arguments, fallbackDetection, ignored, versionAcknowledged, versionSelectionCancelled);
            if (usingVersionFallback) {
                cachedDetection = fallbackDetection;
                hasCachedDetection = true;
            }
            if (!usingVersionFallback) {
                if (kind != XiaoAnInstall::TargetKind::Installed) kind = XiaoAnInstall::TargetKind::Invalid;
                detail = ignored;
            }
        }
    }
    if (versionSelectionCancelled) {
        CoUninitialize();
        return 1;
    }
    if (kind == XiaoAnInstall::TargetKind::Invalid) {
        Show(arguments.quiet, detail, MB_ICONERROR);
        CoUninitialize();
        return 3;
    }

    bool uninstall = arguments.uninstall;
    std::wstring report, error;
    if (!arguments.quiet && (!arguments.featuresSpecified ||
        (!arguments.install && !arguments.uninstall && kind == XiaoAnInstall::TargetKind::Installed))) {
        FeatureSelection selection;
        selection.mask = arguments.features;
        selection.uninstall = uninstall;
        selection.update = kind == XiaoAnInstall::TargetKind::Installed;
        selection.chooseAction = !arguments.install && !arguments.uninstall && selection.update;
        if (kind == XiaoAnInstall::TargetKind::Installed) {
            XiaoAnInstall::Manifest manifest;
            if (XiaoAnInstall::LoadManifest(arguments.root, manifest, error)) {
                if (!arguments.featuresSpecified) selection.mask = manifest.featureMask;
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
        uninstall = selection.uninstall;
    }
    if (!uninstall && usingVersionFallback && !arguments.forceVersion && !existingVersionFallback && !versionAcknowledged) {
        if (arguments.quiet) {
            CoUninitialize();
            return 5; // An unattended fallback requires an explicit version-install request.
        }
        const std::wstring backupPath = PatchPrivatePath(arguments.root, fallbackDetection.profile) + L"\\backup";
        const std::wstring warning = std::wstring(L"自动识别未完成或主程序已加壳，将按版本 ") +
            EngineProfileName(fallbackDetection.profile) +
            L"选择载荷。\r\n\r\n先备份主程序、音频 DLL、齿轮 DLL 和补丁配置。版本选择不等于运行时兼容确认，"
            L"启动与各功能状态会保持待验证，失败时会记录诊断；布局不兼容仍可能导致游戏异常，可通过安装清单卸载恢复。\r\n备份目录：\r\n" +
            backupPath + L"\r\n\r\n是否继续？";
        if (MessageBoxW(nullptr, warning.c_str(), L"确认按版本安装", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
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
        EngineDetection engine = cachedDetection;
        XiaoAnInstall::PackagePayload payload;
        const bool detected = hasCachedDetection || (usingVersionFallback ?
            DetectVersionEngineProfile(arguments.root, engine, error, fallbackDetection.profile) :
            DetectEngineProfile(arguments.root, engine, error));
        if (detected && !hasCachedDetection) {
            cachedDetection = engine;
            hasCachedDetection = true;
        }
        if (usingVersionFallback) {
            engine.versionFallback = true;
            cachedDetection.versionFallback = true;
        }
        if (detected && LoadPayload(engine, payload, error)) {
            payload.versionFallback = usingVersionFallback;
            success = XiaoAnInstall::InstallPatch(arguments.root, payload, kind, report, error,
                arguments.featuresSpecified ? arguments.features : XiaoAnInstall::FeatureAll,
                &cachedDetection);
        }
    }
    if (!success) Show(arguments.quiet, error, MB_ICONERROR);
    else Show(arguments.quiet, report, MB_ICONINFORMATION);
    CoUninitialize();
    return success ? 0 : 4;
}
