#pragma once
#include "EngineLayout.h"

// Notice leaves are independent of R/S. Unknown/interactive callers stay native.
// The profile-specific 6.4/6.5 and 6.6 strategy-name callers are separately
// verified; their formatter follows its selected native
// Strategy owner; ordinary dialog notices retain their independent checkbox.
bool PrepareNoticePresentation(const EngineLayout& layout, uintptr_t gameBase, uintptr_t virtualBase = 0) noexcept;
bool ConfigureNoticePresentation(bool cancelDialog, bool protectPresentation) noexcept;
bool RestoreNoticePresentation() noexcept;
bool NoticePresentationIdle() noexcept;
bool NoticePresentationAvailable() noexcept;
bool NoticePresentationRecoveryPending() noexcept;
void SuspendNoticePresentationForRecovery() noexcept;
#ifdef XA66_AUDIT
// Static auditor reads the actual prepared production descriptors. It never
// installs these jumps or loads the engine DLL into the game process.
struct NoticeHookAudit {
    uintptr_t address = 0;
    BYTE original[16] = {};
    unsigned length = 0, relativeCallOffset = 0xFF, argumentBytes = 0;
};
bool ReadNoticeHookAudit(size_t index, NoticeHookAudit& output) noexcept;
bool NoticeStrategyNameAuditReady() noexcept;
#endif
#ifdef XA66_TEST
void NoticePresentationTestFailWrites(int forwardIndex, int rollbackIndex,
    bool afterCopy = false, bool failCache = false) noexcept;
#endif
