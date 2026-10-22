#pragma once

#include "Patch.h"

#include <string>
#include <vector>
#include <cstdint>

enum BattleStatsMetric : uint32_t {
    BattleStatsDamageDealt = 1u << 0,
    BattleStatsDamageReceived = 1u << 1,
    BattleStatsRecovery = 1u << 2,
    BattleStatsCritical = 1u << 3,
    BattleStatsCombo = 1u << 4,
    BattleStatsAttackBlock = 1u << 5,
    BattleStatsSpellBlock = 1u << 6,
    BattleStatsDefeatedEnemies = 1u << 7
};

struct BattleUnitTotals {
    int slot = -1;
    int personId = -1;
    std::wstring label;
    long long damageDealt = 0;
    long long damageReceived = 0;
    long long recoveryDone = 0;
    long long criticalCount = 0;
    long long comboExtraHitCount = 0;
    // Counts only explicit, successful result commits.  They must not be
    // inferred from unchanged HP, misses, immunity, or target redirection.
    long long attackBlockCount = 0;
    long long spellBlockCount = 0;
    int defeatedEnemies = 0;
};

struct BattleStatsSnapshot {
    bool hookAvailable = false;
    bool physicalSettlementAvailable = false;
    bool strategyDamageAvailable = false;
    bool recoveryAvailable = false;
    bool criticalAvailable = false;
    bool comboAvailable = false;
    // False means the current executable has no uniquely verified block
    // result commit; the UI renders that column as "--".
    bool attackBlockAvailable = false;
    bool spellBlockAvailable = false;
    bool active = false;
    bool loading = false;
    uint32_t schemaVersion = 2;
    int scenarioOrdinal = -1;
    uint64_t campaignId = 0;
    uint64_t battleInstance = 0;
    uint32_t knownMetrics = 0;
    bool historyIncomplete = false;
    unsigned long long elapsedMilliseconds = 0;
    std::wstring scenario;
    std::wstring status;
    std::vector<BattleUnitTotals> units;
    std::vector<BattleUnitTotals> campaignUnits;
};

struct BattleStatsPersistedState {
    uint32_t schemaVersion = 2;
    int scenarioOrdinal = -1;
    uint64_t campaignId = 0;
    uint64_t battleInstance = 0;
    uint32_t knownMetrics = 0;
    bool historyIncomplete = false;
    std::wstring status;
    bool active = false;
    unsigned long long elapsedMilliseconds = 0;
    std::wstring scenario;
    std::vector<BattleUnitTotals> units;
    std::vector<BattleUnitTotals> campaignUnits;
};

struct BattleStatsImageLayout {
    uintptr_t hpSetter = 0;
    uintptr_t unitBase = 0;
    uintptr_t strategyBase = 0;
    unsigned strategyCount = 0;
    uintptr_t physicalResultCall = 0;
    uintptr_t criticalCommit = 0;
    uintptr_t attackBlockCommit = 0;
    uintptr_t spellBlockCommit = 0;
    uint32_t criticalOffset = 0;
    uint32_t comboOffset = 0;
    std::vector<uintptr_t> settlementCalls;
};

void InitializeBattleStats(const wchar_t* gameRoot);
void PollBattleStats(ULONGLONG now);
void ResetBattleStats();
void ShutdownBattleStats();
bool ReadBattleStatsSnapshot(BattleStatsSnapshot& snapshot);
bool CaptureBattleStatsState(BattleStatsPersistedState& state, ULONGLONG now);
bool RestoreBattleStatsState(const BattleStatsPersistedState& state, ULONGLONG now);
void SetBattleStatsLoading(bool loading);
void NotifyBattleStatsScenarioInstalled(int ordinal, bool newCampaign,
    bool forceNewInstance, ULONGLONG now);
void SetBattleStatsStatus(const std::wstring& status, bool historyIncomplete);

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
void RecordBattleStatsAttackBlockForTest(int attackerSlot, int targetSlot);
void RecordBattleStatsSpellBlockForTest(int attackerSlot, int targetSlot);
