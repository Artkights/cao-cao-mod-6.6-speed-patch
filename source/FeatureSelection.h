#pragma once
#include "InstallCommon.h"
#include "ResourceIds.h"

struct FeatureSelection {
    unsigned mask = XiaoAnInstall::FeatureAll;
    bool uninstall = false;
    unsigned available = XiaoAnInstall::FeatureAll;
};

inline INT_PTR CALLBACK FeatureSelectionProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* selection = reinterpret_cast<FeatureSelection*>(GetWindowLongPtrW(window, DWLP_USER));
    const int controls[] = {IDC_FEATURE_ACCELERATOR, IDC_FEATURE_GUIDE, IDC_FEATURE_BATTLE_STATS};
    if (message == WM_INITDIALOG) {
        selection = reinterpret_cast<FeatureSelection*>(lParam);
        SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(selection));
        SetWindowTextW(window, selection->uninstall ? L"选择要卸载的功能" : L"选择要安装的功能");
        for (unsigned i = 0; i < 3; ++i) {
            CheckDlgButton(window, controls[i], selection->mask & (1u << i) ? BST_CHECKED : BST_UNCHECKED);
            EnableWindow(GetDlgItem(window, controls[i]), !selection->uninstall || (selection->available & (1u << i)));
        }
        return TRUE;
    }
    if (message == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) {
        if (LOWORD(wParam) == IDOK && selection) {
            selection->mask = 0;
            for (unsigned i = 0; i < 3; ++i)
                if (IsDlgButtonChecked(window, controls[i]) == BST_CHECKED) selection->mask |= 1u << i;
            if (!selection->mask) {
                MessageBoxW(window, L"请至少选择一个功能。", L"选择补丁功能", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
        }
        EndDialog(window, LOWORD(wParam));
        return TRUE;
    }
    return FALSE;
}
