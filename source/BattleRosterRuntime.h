#pragma once
#include "Patch.h"
#include <cstdint>
#include <cstddef>

struct BattleRosterRuntimeLayout {
    uintptr_t handler = 0;
    uintptr_t completedSite = 0;
    uintptr_t continuation = 0;
    uintptr_t peoplePointer = 0;
    uintptr_t sceneMode = 0;
};

// Observes the native successful 3B path after its side effects, never a
// resource open or a statically present command.
bool InitializeBattleRosterRuntime();
void PollBattleRosterRuntime(ULONGLONG now);
bool ShutdownBattleRosterRuntime();
bool ResolveBattleRosterRuntimeForTest(BYTE* image, size_t size, uintptr_t base,
    BattleRosterRuntimeLayout& layout);
#ifdef XA66_TEST
bool InstallBattleRosterRuntimeForTest(BYTE* image, size_t size);
void CompleteBattleRosterJoinForTest(int personId, int requestedCamp,
    size_t commandPosition, ULONGLONG now);
uintptr_t BattleRosterBridgeForTest(uintptr_t continuation);
void FailBattleRosterWriteForTest(int stage);
#endif
