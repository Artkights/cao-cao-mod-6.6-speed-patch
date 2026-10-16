#pragma once
#include "Patch.h"

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

struct EngineLayout {
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
    uintptr_t waitCallers[32] = {};
    unsigned char waitKinds[32] = {};
    size_t waitCallerCount = 0;
    unsigned capabilities = 0;
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
