#pragma once
#include "InstallCommon.h"
#include "ResourceIds.h"

struct FeatureSelection {
    unsigned mask = XiaoAnInstall::FeatureAll;
    bool uninstall = false;
    unsigned available = XiaoAnInstall::FeatureAll;
    bool chooseAction = false;
    bool update = false;
    bool closeGame = false;
};

inline void UpdateFeatureSelectionDialog(HWND window, FeatureSelection* selection) {
    if (!selection) return;
    const int controls[] = {IDC_FEATURE_ACCELERATOR, IDC_FEATURE_GUIDE, IDC_FEATURE_BATTLE_STATS};
    unsigned mask = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const bool enabled = !selection->uninstall || (selection->available & (1u << i));
        EnableWindow(GetDlgItem(window, controls[i]), enabled);
        if (!enabled) CheckDlgButton(window, controls[i], BST_UNCHECKED);
        if (IsDlgButtonChecked(window, controls[i]) == BST_CHECKED) mask |= 1u << i;
    }
    EnableWindow(GetDlgItem(window, IDOK), mask != XiaoAnInstall::FeatureNone);
    SetWindowTextW(window, selection->uninstall ? L"选择要卸载的功能" :
        selection->update ? L"选择要安装 / 更新的功能" : L"选择要安装的功能");
    SetDlgItemTextW(window, IDOK, selection->uninstall ?
        (selection->closeGame ? L"退出游戏并卸载" : L"卸载所选功能") :
        selection->update ? L"安装 / 更新" : L"安装");
    const wchar_t* note = selection->uninstall ?
        (selection->closeGame ? L"游戏将先退出，未保存的进度会丢失。其余功能继续保留；卸载最后一项后恢复原始文件。" :
         L"只卸载勾选的功能，其余功能继续保留；卸载最后一项后恢复原始文件。")
        : L"所选功能共用核心代理；加速包含齿轮模块。安装完成后可在游戏内继续调整配置。";
    SetDlgItemTextW(window, IDC_FEATURE_NOTE, note);
}

inline INT_PTR CALLBACK FeatureSelectionProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* selection = reinterpret_cast<FeatureSelection*>(GetWindowLongPtrW(window, DWLP_USER));
    const int controls[] = {IDC_FEATURE_ACCELERATOR, IDC_FEATURE_GUIDE, IDC_FEATURE_BATTLE_STATS};
    if (message == WM_INITDIALOG) {
        selection = reinterpret_cast<FeatureSelection*>(lParam);
        SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(selection));
        CheckDlgButton(window, selection->uninstall ? IDC_FEATURE_UNINSTALL_ACTION : IDC_FEATURE_INSTALL_ACTION, BST_CHECKED);
        ShowWindow(GetDlgItem(window, IDC_FEATURE_INSTALL_ACTION), selection->chooseAction ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window, IDC_FEATURE_UNINSTALL_ACTION), selection->chooseAction ? SW_SHOW : SW_HIDE);
        for (unsigned i = 0; i < 3; ++i) {
            CheckDlgButton(window, controls[i], selection->mask & (1u << i) ? BST_CHECKED : BST_UNCHECKED);
            EnableWindow(GetDlgItem(window, controls[i]), !selection->uninstall || (selection->available & (1u << i)));
        }
        UpdateFeatureSelectionDialog(window, selection);
        return TRUE;
    }
    if (message == WM_COMMAND && (HIWORD(wParam) == BN_CLICKED) &&
        (LOWORD(wParam) == IDC_FEATURE_ACCELERATOR || LOWORD(wParam) == IDC_FEATURE_GUIDE ||
         LOWORD(wParam) == IDC_FEATURE_BATTLE_STATS)) {
        UpdateFeatureSelectionDialog(window, selection);
        return TRUE;
    }
    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && selection && selection->chooseAction &&
        (LOWORD(wParam) == IDC_FEATURE_INSTALL_ACTION || LOWORD(wParam) == IDC_FEATURE_UNINSTALL_ACTION)) {
        selection->uninstall = LOWORD(wParam) == IDC_FEATURE_UNINSTALL_ACTION;
        UpdateFeatureSelectionDialog(window, selection);
        return TRUE;
    }
    if (message == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) {
        if (LOWORD(wParam) == IDOK && selection) {
            selection->mask = 0;
            for (unsigned i = 0; i < 3; ++i)
                if (IsDlgButtonChecked(window, controls[i]) == BST_CHECKED) selection->mask |= 1u << i;
            if (!selection->mask) return TRUE;
        }
        EndDialog(window, LOWORD(wParam));
        return TRUE;
    }
    return FALSE;
}
