#pragma once

#include "EngineLayout.h"

// Preparing never changes the game. Hook changes require idle action scopes
// and roll back earlier writes when a later site cannot be changed.
bool PrepareAnimationRuntime(const EngineLayout& layout, uintptr_t gameBase) noexcept;
// allPresentation is true only when every super skip option is actually
// applied. It still executes native business/parameter parsing; it removes the
// remaining kind/camp ambiguity for presentation owners whose actor cannot be
// proven (hidden appearance, withdrawal, revive, shared status/effect owners).
bool ConfigureAnimationRuntime(bool superS, bool superR, bool cancelDialog = false,
    bool cancelMapText = false, bool allPresentation = false) noexcept;
bool RestoreAnimationRuntime() noexcept;
// A failed rollback retains its hooks and allocations until disable succeeds.
// Enabling is rejected while this is true; disabling retries the recorded sites.
bool AnimationRuntimeRecoveryPending() noexcept;
void SuspendAnimationRuntimeForRecovery() noexcept;
bool AnimationRuntimeAvailable() noexcept;
bool AnimationRuntimeSupportsS() noexcept;
bool AnimationRuntimeSupportsR() noexcept;
bool AnimationRuntimeDialogAvailable() noexcept;
bool AnimationRuntimeMapTextAvailable() noexcept;
bool AnimationRuntimeIdle() noexcept;
// Independently verified presentation owners may establish a child policy.
// A native child prevents an enclosing R/S action from hiding an unselected
// notification. Always end the exact returned token in a finally block.
uintptr_t BeginAnimationPresentationScope(bool suppress) noexcept;
uintptr_t BeginAnimationPresentationScopeForFrame(bool suppress, uintptr_t nativeReturnSlot) noexcept;
bool EndAnimationPresentationScope(uintptr_t token) noexcept;
bool AnimationPresentationSelected(unsigned kind, int camp, bool rScene = false) noexcept;
// True only when all five super skip options were applied, every S kind and
// every camp remains selected, and a concrete R/S scene is proven.
bool AnimationPresentationAllSelected() noexcept;
// Battlefield previews follow the phase UI policy, independently of action
// category/camp filters. Unknown/loading/recovery states stay native.
bool AnimationBattlePresentationEnabled() noexcept;
enum class AnimationPresentationCategory : unsigned char { RAction, SAction, Dialog, MapText };
// Independent categories do not inherit a surrounding R/S action's selection.
bool AnimationPresentationSelected(AnimationPresentationCategory category,
    unsigned kind = 0, int camp = -1) noexcept;
// Retain native display for this entire live chain after an ownership/depth
// failure. The latch clears only after every tracked scope has returned.
void PreserveNativePresentationUntilIdle() noexcept;
// A verified post-parser consumer may refine the exact currently active owner
// to its resolved target. Unknown owners/camps retain native presentation.
bool RetargetAnimationPresentationOwner(uintptr_t ownerAddress, int camp) noexcept;
// Read-only unwrapping for an independently verified native frame. Both the
// observed return and the exact live slot must still belong to this thread's
// tracked exit bridge. Callers must separately validate the native caller.
bool ResolveNativeAnimationReturn(uintptr_t returnSlot, uintptr_t observedReturn,
    uintptr_t& nativeReturn) noexcept;
// -1 means no proven action. 0/1 preserves/omits its presentation wait.
int CurrentAnimationWaitPolicy(uintptr_t caller = 0, uintptr_t callerFrame = 0) noexcept;
// -1 unknown/title/loading, 0 confirmed R, 1 confirmed battlefield.
void SetAnimationScenePredicate(int (*predicate)()) noexcept;
// Invalidate every live owner and detached R token across same-scene loads.
// A null provider preserves standalone callers without scenario lifecycle.
void SetAnimationScenarioGenerationProvider(ULONGLONG (*provider)()) noexcept;
// Returns false once after a provider generation changes, allowing independent
// Notice hooks to retain native business for that first post-load query.
bool ConsumeAnimationScenarioGenerationInvalidation() noexcept;
// Accept state-0 maps only inside an exact native script-dispatch scope.
void SetAnimationMapIdentityPredicate(bool (*predicate)(uintptr_t, unsigned, ULONGLONG&)) noexcept;
bool AnimationSelectedStrategyOwner() noexcept;
// A verified shared child may identify its live native owner independently of
// that owner's selection. Excluded owners still define the child's category.
bool AnimationPresentationOwnerActive(uintptr_t ownerEntry) noexcept;
bool AnimationPresentationOwnerSelected(uintptr_t ownerEntry) noexcept;
void RecordUnclassifiedAnimationWait(uintptr_t caller) noexcept;

#if defined(XA66_TEST) || defined(ANIMATION_TEST_OBSERVER)
bool AnimationTestBegin(unsigned kind, int camp, bool rScene = false) noexcept;
void AnimationTestEnd() noexcept;
int AnimationTestPresent(int surface, int left, int top, int width, int height) noexcept;
void AnimationTestConfigure(bool superS, bool superR,
    int (__stdcall* present)(int, int, int, int, int)) noexcept;
void AnimationTestAllPresentation(bool enabled) noexcept;
void AnimationTestFailWrites(int forwardIndex, int rollbackIndex,
    bool afterForwardCopy = false, bool afterRollbackCopy = false, bool failCache = false) noexcept;
#endif
