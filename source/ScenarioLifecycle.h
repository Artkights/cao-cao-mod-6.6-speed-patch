#pragma once

#include "Patch.h"

struct ScenarioLifecycleImageLayout {
    uintptr_t nativeInstall = 0;
    uintptr_t ordinalAccessor = 0;
    uintptr_t ordinalAddress = 0;
    uintptr_t installedSite = 0;
    uintptr_t installedContinuation = 0;
    uintptr_t progressCall = 0;
    uintptr_t initialCall = 0;
    uintptr_t reloadCall = 0;
    uintptr_t menuFunction = 0;
};

bool InitializeScenarioLifecycle();
void ShutdownScenarioLifecycle();
// The native ordinal is R = even, S = odd; -1 means unavailable.
int CurrentScenarioOrdinal();
void BeginScenarioLoad();
void EndScenarioLoad(bool succeeded);
bool ScenarioLoadInProgress();
bool ResolveScenarioLifecycleForTest(BYTE* image, uintptr_t virtualBase,
    ScenarioLifecycleImageLayout& layout);

// Native ABI and state-machine harnesses; they never open game resources.
void ConfigureScenarioLifecycleForTest(WORD* ordinal, uintptr_t initialReturn,
    uintptr_t progressReturn, uintptr_t reloadReturn);
void CompleteScenarioInstallForTest(bool succeeded, uintptr_t returnAddress,
    uintptr_t callerFrame, ULONGLONG now);
uintptr_t ScenarioInstalledBridgeForTest(uintptr_t continuation);
