#pragma once

#include "Patch.h"

#include <string>
#include <vector>

struct BattleUnitTotals {
    int slot = -1;
    std::wstring label;
    long long damageDealt = 0;
    long long damageReceived = 0;
    long long recoveryDone = 0;
    long long criticalCount = 0;
    long long comboExtraHitCount = 0;
    int defeatedEnemies = 0;
};

struct BattleStatsSnapshot {
    bool hookAvailable = false;
    bool physicalSettlementAvailable = false;
    bool strategyDamageAvailable = false;
    bool recoveryAvailable = false;
    bool criticalAvailable = false;
    bool comboAvailable = false;
    bool active = false;
    std::vector<BattleUnitTotals> units;
};

struct BattleStatsImageLayout {
    uintptr_t hpSetter = 0;
    uintptr_t unitBase = 0;
    uintptr_t strategyBase = 0;
    unsigned strategyCount = 0;
    uintptr_t physicalResultCall = 0;
    uintptr_t criticalCommit = 0;
    uint32_t criticalOffset = 0;
    uint32_t comboOffset = 0;
    std::vector<uintptr_t> settlementCalls;
};

void InitializeBattleStats(const wchar_t* gameRoot);
void PollBattleStats(ULONGLONG now);
void ResetBattleStats();
void ShutdownBattleStats();
bool ReadBattleStatsSnapshot(BattleStatsSnapshot& snapshot);

bool ResolveBattleStatsImageForTest(const BYTE* image, uintptr_t virtualBase,
    uintptr_t& hpSetter, std::vector<uintptr_t>& callSites);
bool ResolveBattleStatsImageLayoutForTest(const BYTE* image, uintptr_t virtualBase,
    BattleStatsImageLayout& layout);

// Deterministic model entry points used by the native regression harness.
void InitializeBattleStatsModelForTest(uintptr_t unitBase,
    uintptr_t strategyBase = 0, unsigned strategyCount = 0);
void SetBattleStatsModelActiveForTest(bool active);
void RecordBattleStatsSettlementForTest(int kind, uintptr_t target,
    uintptr_t callerFrame, int oldHp, int finalHp);
void RecordBattleStatsPhysicalResultForTest(uintptr_t context, uint32_t comboOffset);
void RecordBattleStatsCriticalForTest(uintptr_t context, uint32_t comboOffset);
