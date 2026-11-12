#pragma once

#include "Patch.h"

#include <string>
#include <vector>
#include <cstdint>

enum class BattleRosterSource : uint8_t {
    RuntimeJoin3B = 0,
    LegacyStaticR = 1,
    ExistingCampaignLedger = 2
};

struct BattleRosterEntry {
    int personId = -1;
    std::wstring label;
    bool joined = false;
    bool tracking = false;
    bool changedFaction = false;
    int lastCamp = -1;
    int joinScenarioOrdinal = -1;
    uint32_t joinCommandOrdinal = 0xFFFFFFFFu;
    BattleRosterSource source = BattleRosterSource::RuntimeJoin3B;
};

enum BattleStatsMetric : uint32_t {
    BattleStatsDamageDealt = 1u << 0,
    BattleStatsDamageReceived = 1u << 1,
    BattleStatsRecovery = 1u << 2,
    BattleStatsCritical = 1u << 3,
    BattleStatsCombo = 1u << 4,
    BattleStatsAttackBlock = 1u << 5,
    BattleStatsSpellBlock = 1u << 6,
    // Binary schema 2 keeps the old field/bit positions. These two metrics
    // now mean genuine native final hit-test failures for the defender.
    BattleStatsAttackDodge = BattleStatsAttackBlock,
    BattleStatsStrategyDodge = BattleStatsSpellBlock,
    BattleStatsDefeatedEnemies = 1u << 7,
    BattleStatsPhysicalReceived = 1u << 8,
    BattleStatsStrategyReceived = 1u << 9
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
    // Schema 2 compatibility names: counts of real physical / hostile
    // strategy final hit-test failures, excluding immunity and invalid targets.
    // Each independent judgment counts once; no inference from HP or animation.
    long long attackBlockCount = 0;
    long long spellBlockCount = 0;
    int defeatedEnemies = 0;
    // Actual hostile attack segments accepted by this player unit. Misses,
    // immunity and zero damage count; damage plus status in one segment is one.
    long long physicalReceivedCount = 0;
    long long strategyReceivedCount = 0;
};

struct BattleStatsSnapshot {
    bool hookAvailable = false;
    bool physicalSettlementAvailable = false;
    bool strategyDamageAvailable = false;
    bool recoveryAvailable = false;
    bool criticalAvailable = false;
    bool comboAvailable = false;
    // True only after the complete native final-hit / eligibility call graph
    // is verified and all observer CALL patches install together.
    bool attackBlockAvailable = false;
    bool spellBlockAvailable = false;
    bool physicalReceivedAvailable = false;
    bool strategyReceivedAvailable = false;
    bool active = false;
    bool loading = false;
    uint32_t schemaVersion = 4;
    int scenarioOrdinal = -1;
    uint64_t campaignId = 0;
    uint64_t battleInstance = 0;
    uint32_t knownMetrics = 0;
    bool historyIncomplete = false;
    bool rosterHistoryIncomplete = false;
    bool rosterAvailable = false;
    bool scenarioIsR = false;
    unsigned long long elapsedMilliseconds = 0;
    std::wstring scenario;
    std::wstring status;
    std::vector<BattleUnitTotals> units;
    std::vector<BattleUnitTotals> campaignUnits;
    std::vector<int> currentBattleParticipants;
    std::vector<BattleRosterEntry> campaignRoster;
};

struct BattleStatsPersistedState {
    uint32_t schemaVersion = 4;
    int scenarioOrdinal = -1;
    uint64_t campaignId = 0;
    uint64_t battleInstance = 0;
    uint32_t knownMetrics = 0;
    bool historyIncomplete = false;
    bool rosterHistoryIncomplete = false;
    std::wstring status;
    bool active = false;
    unsigned long long elapsedMilliseconds = 0;
    std::wstring scenario;
    std::vector<BattleUnitTotals> units;
    std::vector<BattleUnitTotals> campaignUnits;
    std::vector<int> currentBattleParticipants;
    std::vector<BattleRosterEntry> campaignRoster;
};

// Schema 4 stores membership separately from totals. Reject partial or
// contradictory graphs before replacing either an in-memory or on-disk save.
inline bool ValidateBattleStatsRosterState(const BattleStatsPersistedState& state) {
    if (state.schemaVersion < 4) return state.campaignRoster.empty() &&
        state.currentBattleParticipants.empty();
    if (state.campaignRoster.size() > 65535 || state.campaignUnits.size() > 65535 ||
        state.currentBattleParticipants.size() > 250 || state.units.size() > 250 ||
        (!state.active && (!state.units.empty() || !state.currentBattleParticipants.empty())) ||
        (state.active && state.scenarioOrdinal >= 0 && !(state.scenarioOrdinal & 1))) return false;
    std::vector<const BattleRosterEntry*> roster(65535, nullptr);
    for (const BattleRosterEntry& item : state.campaignRoster) {
        if (item.personId < 0 || item.personId >= 65535 || roster[item.personId] ||
            !item.joined || item.label.size() > 256 || item.lastCamp < -1 || item.lastCamp > 255 ||
            item.joinScenarioOrdinal < -1 || item.joinScenarioOrdinal >= 65535 ||
            static_cast<unsigned>(item.source) > static_cast<unsigned>(BattleRosterSource::ExistingCampaignLedger) ||
            (item.tracking && (item.changedFaction || item.lastCamp != 0)) ||
            (item.changedFaction && item.lastCamp < 0) ||
            (item.source != BattleRosterSource::ExistingCampaignLedger &&
                (item.joinScenarioOrdinal < 0 || (item.joinScenarioOrdinal & 1))) ||
            (item.source == BattleRosterSource::RuntimeJoin3B &&
                item.joinCommandOrdinal == 0xFFFFFFFFu)) return false;
        roster[item.personId] = &item;
    }
    std::vector<bool> campaign(65535, false), participants(65535, false), current(65535, false);
    for (const BattleUnitTotals& unit : state.campaignUnits) {
        if (unit.personId < 0 || unit.personId >= 65535 || campaign[unit.personId] ||
            !roster[unit.personId] || roster[unit.personId]->label != unit.label) return false;
        campaign[unit.personId] = true;
    }
    if (state.campaignUnits.size() != state.campaignRoster.size()) return false;
    for (int id : state.currentBattleParticipants) {
        if (id < 0 || id >= 65535 || participants[id]) return false;
        participants[id] = true;
    }
    for (const BattleUnitTotals& unit : state.units) {
        if (unit.personId < 0 || unit.personId >= 65535 || current[unit.personId] ||
            !participants[unit.personId]) return false;
        current[unit.personId] = true;
    }
    return state.units.size() == state.currentBattleParticipants.size();
}

struct BattleStatsImageLayout {
    uintptr_t hpSetter = 0;
    uintptr_t unitBase = 0;
    uintptr_t strategyBase = 0;
    unsigned strategyCount = 0;
    uintptr_t physicalResultCall = 0;
    uintptr_t criticalCommit = 0;
    uintptr_t attackBlockCommit = 0;
    uintptr_t spellBlockCommit = 0;
    bool dodgeAvailable = false;
    uintptr_t finalHit = 0;
    uint32_t criticalOffset = 0;
    uint32_t comboOffset = 0;
    bool physicalReceivedAvailable = false;
    bool strategyReceivedAvailable = false;
    std::vector<uintptr_t> receivedCalls;
    std::vector<uintptr_t> settlementCalls;
};

void InitializeBattleStats(const wchar_t* gameRoot);
void PollBattleStats(ULONGLONG now);
void ResetBattleStats(const wchar_t* reason = L"explicit-reset");
// A successful native load has already replaced the game. Even when its
// statistics cannot be decoded or allocated, continue from that scenario
// without retaining another save's ledgers. Core replacement allocates no memory.
void ResetBattleStatsForLoadedScenario(const wchar_t* reason, int ordinal) noexcept;
void ShutdownBattleStats();
bool ReadBattleStatsSnapshot(BattleStatsSnapshot& snapshot);
bool CaptureBattleStatsState(BattleStatsPersistedState& state, ULONGLONG now);
bool RestoreBattleStatsState(const BattleStatsPersistedState& state, ULONGLONG now);
void SetBattleStatsLoading(bool loading);
void NotifyBattleStatsScenarioInstalled(int ordinal, bool newCampaign,
    bool forceNewInstance, ULONGLONG now);
void NotifyBattleRosterJoined(int personId, int scenarioOrdinal,
    size_t commandOrdinal, ULONGLONG now);
void NotifyBattleRosterCamp(int personId, int camp, ULONGLONG now);
void NotifyBattleRosterLabel(int personId, const std::wstring& label);
void SetBattleRosterCapability(bool available, const wchar_t* reason = nullptr);
bool ReadBattleRosterTrackingIds(std::vector<int>& personIds);
void SetBattleStatsStatus(const std::wstring& status, bool historyIncomplete);

bool ResolveBattleStatsImageForTest(const BYTE* image, uintptr_t virtualBase,
    uintptr_t& hpSetter, std::vector<uintptr_t>& callSites);
bool ResolveBattleStatsImageLayoutForTest(const BYTE* image, uintptr_t virtualBase,
    BattleStatsImageLayout& layout);

// Deterministic model entry points used by the native regression harness.
void InitializeBattleStatsModelForTest(uintptr_t unitBase,
    uintptr_t strategyBase = 0, unsigned strategyCount = 0, const wchar_t* gameRoot = nullptr);
struct BattleSnapshot;
void ObserveBattleStatsSnapshotForTest(const BattleSnapshot& snapshot);
void SetBattleStatsModelActiveForTest(bool active);
void RecordBattleStatsSettlementForTest(int kind, uintptr_t target,
    uintptr_t callerFrame, int oldHp, int finalHp);
void RecordBattleStatsPhysicalResultForTest(uintptr_t context, uint32_t comboOffset);
void RecordBattleStatsCriticalForTest(uintptr_t context, uint32_t comboOffset);
void RecordBattleStatsAttackBlockForTest(int attackerSlot, int targetSlot);
void RecordBattleStatsSpellBlockForTest(int attackerSlot, int targetSlot);

#ifdef XA66_TEST
bool ConfigureBattleStatsReceivedForTest(const BYTE* image, uintptr_t virtualBase);
void DispatchBattleStatsReceivedForTest(void* snapshot, unsigned role);
void RecordBattleStatsReceivedForTest(bool strategy, int attackerSlot, int targetSlot);
bool ConfigureBattleStatsDodgeForTest(const BYTE* image, uintptr_t virtualBase);
void SetBattleStatsDodgeTargetForTest(unsigned role, uintptr_t target);
void DispatchBattleStatsDodgeForTest(void* snapshot, unsigned role);
bool BattleStatsDodgeObservationActiveForTest();
bool InstallBattleStatsDodgeHooksForTest(BYTE* mapped, unsigned sabotage,
    std::vector<uintptr_t>& sites);
bool InstallAllBattleStatsHooksForTest(BYTE* mapped, uintptr_t virtualBase,
    std::vector<uintptr_t>& sites);
// Failure 1 rejects initial protection; 2 leaves post-copy protection pending;
// failure 3 leaves instruction-cache flush pending. Negative index disables it.
void SetBattleStatsWriteFaultForTest(int forwardIndex,unsigned forwardFailure,
    int restoreIndex,unsigned restoreFailure);
bool BattleStatsRecoveryPendingForTest();
#endif
