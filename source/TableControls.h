#pragma once

#include "ResourceIds.h"

// Scope v6 common controls to our own table windows. The game keeps its
// original activation context and process DPI awareness.
class TableControlsContext {
public:
    explicit TableControlsContext(HMODULE module) {
        ACTCTXW description = {sizeof(description)};
        description.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        description.hModule = module;
        description.lpResourceName = MAKEINTRESOURCEW(IDR_TABLE_CONTROLS);
        context_ = CreateActCtxW(&description);
        if (context_ != INVALID_HANDLE_VALUE)
            active_ = ActivateActCtx(context_, &cookie_) != FALSE;
        if (active_) {
            const INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
            InitCommonControlsEx(&controls);
        }
    }
    ~TableControlsContext() {
        if (active_) DeactivateActCtx(0, cookie_);
        if (context_ != INVALID_HANDLE_VALUE) ReleaseActCtx(context_);
    }
    TableControlsContext(const TableControlsContext&) = delete;
    TableControlsContext& operator=(const TableControlsContext&) = delete;
private:
    HANDLE context_ = INVALID_HANDLE_VALUE;
    ULONG_PTR cookie_ = 0;
    bool active_ = false;
};
