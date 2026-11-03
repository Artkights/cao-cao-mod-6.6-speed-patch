#pragma once
#include "Patch.h"
#include "EngineProfile.h"

// Addresses use the legacy 0x400000 origin; they are independent of the loaded image base.
enum EngineSiteIndex : size_t {
    SiteDialog,
    SiteMapText,
    SiteDelay,
    SiteRWait,
    SiteWaitCall,
    SiteInputModifier,
    SiteDialog3,
    SiteExtraDelay,
    SiteWaitBranch,
    SiteDialogDisplay,
    SiteExtendedDisplay,
    EngineSiteCount
};

// The resolver is deliberately explicit about the trust level of a plan.  A
// caller may present a ForceRequired plan to the user, but must not apply it
// as if it were an automatically verified layout.
enum class EngineResolveMode : unsigned char {
    Automatic,
    Forced
};

enum class EnginePlanStatus : unsigned char {
    Unresolved,
    AutoVerified,
    ForceRequired,
    ForceAccepted,
    Rejected
};

struct EngineSitePlan {
    EngineSiteIndex index = EngineSiteCount;
    uintptr_t legacyAddress = 0;
    uintptr_t rva = 0;
    size_t rawFileOffset = SIZE_MAX;
    size_t length = 0;
    unsigned capability = 0;
    bool required = false;
    bool present = false;
    BYTE original[8] = {};
    BYTE enabled[8] = {};
};

// An auditable description of every address that may be changed by the
// runtime patch.  Addresses remain normalized to the legacy 0x400000 origin;
// RVA and rawFileOffset make the same plan useful for an on-disk report.
struct EnginePatchPlan {
    EngineProfileId profile = EngineProfileId::Unknown;
    EngineResolveMode mode = EngineResolveMode::Automatic;
    EnginePlanStatus status = EnginePlanStatus::Unresolved;
    unsigned confidence = 0;
    uintptr_t preferredBase = 0x400000;
    uintptr_t loadedBase = 0;
    size_t imageSize = 0;
    unsigned capabilities = 0;
    EngineSitePlan sites[EngineSiteCount] = {};
    EngineSitePlan nativeRoute;
    EngineSitePlan nativeState;
    EngineSitePlan nativeBusy;
    EngineSitePlan phaseTable;
    // Read-only runtime dependencies are still audited and file-mapped.
    EngineSitePlan phaseValue;
    EngineSitePlan speedValue;
    EngineSitePlan timerContext;
    std::wstring evidence;
};

struct AnimationEntryLayout {
    uintptr_t address = 0;
    BYTE original[16] = {};
    unsigned char length = 0;
    unsigned char kind = 0;
    unsigned char actorSource = 0;
    bool rScene = false;
    bool dialogueRouter = false;
    unsigned char relativeCallOffset = 0xFF;
    bool physicalSprite = false;
    bool mapPhysicalSprite = false;
    bool numberPresentation = false;
    unsigned char argumentBytes = 0;
    unsigned char sPresentationRole = 0;
    unsigned char rPresentationRole = 0;
};
struct EngineLayout {
    EngineProfileId profile = EngineProfileId::Unknown;
    // Each profile owns its instruction templates and native toggle membership.
    BYTE siteOriginal[EngineSiteCount][8] = {};
    BYTE siteEnabled[EngineSiteCount][8] = {};
    unsigned nativeSiteMask = 0;
    uintptr_t waitWrapper = 0;
    uintptr_t timerContext = 0;
    bool conditionScriptExtension = false;
    uintptr_t scriptDelayHandler = 0;
    uintptr_t scriptDelayCall = 0;
    bool scriptDelayVerified = false;
    bool rPresentationVerified = false;
    bool sPresentationVerified = false;
    unsigned sPresentationPathMask = 0;
    uintptr_t rObjectBase = 0;
    uintptr_t rAsyncWaitReturn = 0;
    uintptr_t rFinalRefresh = 0;
    uintptr_t rComposeAdvance = 0;
    uintptr_t rScriptContext = 0;
    uintptr_t rCommandReturn = 0;
    uintptr_t sceneState = 0;
    uintptr_t frameDepth = 0;
    uintptr_t frameWrapperReturn = 0;
    uintptr_t frameCallers[96] = {};
    unsigned char frameKinds[96] = {};
    size_t frameCallerCount = 0;
    uintptr_t attackFrameCallers[2] = {};
    uintptr_t attackWaitCallers[2] = {};
    uintptr_t animationUnitBase = 0;
    uintptr_t animationDialogueSlotFlag = 0;
    uintptr_t animationPersonTable = 0;
    uintptr_t animationNeutralSprite = 0;
    uintptr_t animationNeutralMapSprite = 0;
    uintptr_t animationMapDraw = 0;
    uintptr_t animationMapSpriteCallers[16] = {};
    size_t animationMapSpriteCallerCount = 0;
    uintptr_t animationScriptPhysicalSpriteCallers[20] = {};
    size_t animationScriptPhysicalSpriteCallerCount = 0;
    uintptr_t animationNumberWaitCallers[2] = {};
    uintptr_t animationNumberPresentationCallers[3] = {};
    size_t animationNumberPresentationCallerCount = 0;
    uintptr_t animationNumberTickWait = 0;
    uintptr_t animationAutoInformationCallers[8] = {};
    size_t animationAutoInformationCallerCount = 0;
    uintptr_t animationMapInformationCallers[8] = {};
    size_t animationMapInformationCallerCount = 0;
    uintptr_t animationDirectPanelCallers[4] = {};
    size_t animationDirectPanelCallerCount = 0;
    uintptr_t animationAiPreviewMoveCallers[4] = {};
    size_t animationAiPreviewMoveCallerCount = 0;
    uintptr_t animationAiPreviewRangeCallers[4] = {};
    size_t animationAiPreviewRangeCallerCount = 0;
    uintptr_t animationAiPreviewPanelCallers[4] = {};
    size_t animationAiPreviewPanelCallerCount = 0;
    uintptr_t animationPreviewArrowCallers[8] = {};
    size_t animationPreviewArrowCallerCount = 0;
    uintptr_t animationHighlightFillCallers[4] = {};
    size_t animationHighlightFillCallerCount = 0;
    uintptr_t animationWaitRouterReturn = 0;
    uintptr_t animationWaitLoopReturn = 0;
    uintptr_t animationOrdinaryPhysical = 0;
    uintptr_t animationPhysicalSpriteCallers[8] = {};
    size_t animationPhysicalSpriteCallerCount = 0;
    bool animationDialogueEncodedSlots = false;
    uintptr_t animationPresent = 0;
    BYTE animationPresentOriginal[16] = {};
    unsigned char animationPresentLength = 0;
    AnimationEntryLayout animationEntries[128] = {};
    size_t animationEntryCount = 0;
    bool stageUsesThis = false;
    // 6.4 keeps the phase/timer state in the proxy.  The resolver still
    // records the verified phase variable and timing chain, but does not
    // require the 6.5/6.6 native toggle or phase table.
    bool phaseOwnedByPatch = false;
    bool dialog3Applicable = true;
    uintptr_t sites[EngineSiteCount] = {};
    uintptr_t nativeStage = 0;
    uintptr_t nativeSuper = 0;
    uintptr_t nativeSuperCall = 0;
    uintptr_t nativeSuperState = 0;
    uintptr_t nativeSuperBusy = 0;
    uintptr_t deadline = 0;
    uintptr_t sleep = 0;
    uintptr_t pump = 0;
    uintptr_t phase = 0;
    uintptr_t speed = 0;
    uintptr_t phaseTable = 0;
    uintptr_t phaseCounter = 0;
    uintptr_t waitCallers[128] = {};
    unsigned char waitKinds[128] = {};
    size_t waitCallerCount = 0;
    unsigned uniqueSignatureAttempts = 0;
    unsigned missingSignatureAttempts = 0;
    unsigned ambiguousSignatureAttempts = 0;
    unsigned ambiguousWaitCallers = 0;
    unsigned capabilities = 0;
    EnginePatchPlan plan;
};

enum EngineCapability : unsigned {
    CapabilitySuperDialog = 1u << 0,
    CapabilitySuperMapText = 1u << 1,
    CapabilitySuperDelay = 1u << 2,
    CapabilitySuperR = 1u << 3,
    CapabilitySuperS = 1u << 4,
    CapabilityPhase = 1u << 5,
    CapabilityInput = 1u << 6,
    CapabilityDialog3 = 1u << 7,
    CapabilityExtraDelay = 1u << 8,
    CapabilityDialogDisplay = 1u << 9,
    CapabilityExtendedDisplay = 1u << 10,
    CapabilityNativeSuper = 1u << 11
};

bool ResolveEngineLayout(const BYTE* image, EngineLayout& layout, uintptr_t virtualBase = 0);

// Explicit fallback for a user-selected family.  It uses that family's built
// in address template, checks PE/section boundaries, and captures the target's
// actual bytes.  It never infers or automatically accepts the family.
bool ResolveForcedEngineLayout(const BYTE* image, size_t imageSize,
    EngineProfileId profile, EngineLayout& layout, uintptr_t virtualBase = 0);

// Build the file-addressable/auditable plan for a resolved mapped image.  The
// existing resolver remains the source of truth; this helper adds raw offsets
// and explicit mode/status without changing legacy callers.
bool BuildEnginePatchPlan(const BYTE* image, size_t imageSize, const EngineLayout& layout,
    EnginePatchPlan& plan, uintptr_t virtualBase = 0,
    EngineResolveMode mode = EngineResolveMode::Automatic);

const wchar_t* EnginePlanStatusName(EnginePlanStatus status);
std::wstring EnginePatchPlanReport(const EnginePatchPlan& plan);

// Returns the last plan used by the in-process engine resolver.  The object is
// owned by the runtime and remains valid until the next initialization.
const EnginePatchPlan& RuntimeEnginePatchPlan();
