#include "BattleStats.h"
#include "BattleRuntime.h"
#include "EngineProfile.h"
#include "ScenarioScript.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <stdio.h>
#include <string>
#include <vector>

extern "C" uintptr_t gBattleStatsOriginalSetHp;
extern "C" void BattleStatsRecoveryBridge();

bool GetObservedGuideScenario(std::wstring& name) {
    name.clear();
    return false;
}

namespace {
constexpr size_t kUnitStride = 0x30;
constexpr size_t kUnitCount = 250;
constexpr size_t kStrategyStride = 0x61;
constexpr size_t kStrategyTypeOffset = 0x0B;
constexpr unsigned kStrategyCount = 0x90;
constexpr BYTE kSupplyStrategy = 55;
constexpr BYTE kPersistentRecoveryStrategy = 104;
constexpr uint32_t kComboOffset = 0x608;
int failures = 0;

extern "C" __declspec(naked) void TestHpSetter() {
    __asm {
        mov eax, dword ptr [esp + 4]
        mov dword ptr [ecx + 10h], eax
        ret 4
    }
}

extern "C" __declspec(naked) void InvokeRecoveryBridgeForTest(uintptr_t, uintptr_t) {
    __asm {
        push ebp
        mov ebp, esp
        sub esp, 20h
        mov eax, dword ptr [ebp + 8]
        mov dword ptr [ebp - 10h], eax
        mov eax, dword ptr [ebp + 0Ch]
        mov dword ptr [ebp - 0Ch], eax
        mov byte ptr [ebp - 4], 0
        mov ecx, dword ptr [ebp + 8]
        push 82
        call BattleStatsRecoveryBridge
        mov esp, ebp
        pop ebp
        ret 8
    }
}

void Check(bool condition, const char* name) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

bool LoadImage(const wchar_t* path, std::vector<BYTE>& image, DWORD& base) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb")) return false;
    _fseeki64(file, 0, SEEK_END);
    const __int64 length = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    if (length < 256 || length > 64 * 1024 * 1024) {
        fclose(file);
        return false;
    }
    std::vector<BYTE> raw(static_cast<size_t>(length));
    const bool read = fread(raw.data(), 1, raw.size(), file) == raw.size();
    fclose(file);
    if (!read) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage > 64 * 1024 * 1024 ||
        nt->OptionalHeader.SizeOfHeaders > raw.size()) return false;
    image.assign(nt->OptionalHeader.SizeOfImage, 0);
    memcpy(image.data(), raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        if (section.PointerToRawData + static_cast<size_t>(section.SizeOfRawData) > raw.size() ||
            section.VirtualAddress + static_cast<size_t>(section.SizeOfRawData) > image.size()) return false;
        memcpy(image.data() + section.VirtualAddress, raw.data() + section.PointerToRawData,
            section.SizeOfRawData);
    }
    base = nt->OptionalHeader.ImageBase;
    return true;
}

void PutPointer(BYTE* address, uintptr_t value) {
    memcpy(address, &value, sizeof(value));
}

void PutByte(BYTE* address, BYTE value) {
    memcpy(address, &value, sizeof(value));
}

void PutInt(BYTE* address, int value) {
    memcpy(address, &value, sizeof(value));
}

void PutPerson(BYTE* address, uint16_t value) {
    memcpy(address, &value, sizeof(value));
}

const BattleUnitTotals* FindPerson(const std::vector<BattleUnitTotals>& units, int personId) {
    for (const BattleUnitTotals& unit : units)
        if (unit.personId == personId) return &unit;
    return nullptr;
}

const BattleUnitTotals* FindUnit(const BattleStatsSnapshot& snapshot, int slot) {
    for (const BattleUnitTotals& unit : snapshot.units)
        if (unit.slot == slot) return &unit;
    return nullptr;
}

bool SameTotals(const BattleUnitTotals& left, const BattleUnitTotals& right) {
    return left.slot == right.slot && left.personId == right.personId && left.label == right.label &&
        left.damageDealt == right.damageDealt &&
        left.damageReceived == right.damageReceived &&
        left.recoveryDone == right.recoveryDone &&
        left.criticalCount == right.criticalCount &&
        left.comboExtraHitCount == right.comboExtraHitCount &&
        left.attackBlockCount == right.attackBlockCount &&
        left.spellBlockCount == right.spellBlockCount &&
        left.defeatedEnemies == right.defeatedEnemies;
}

void DeployForTest(const std::vector<BYTE>& units, std::initializer_list<int> slots) {
    BattleSnapshot snapshot;
    snapshot.capabilities = BattleCapabilityUnits | BattleCapabilityDeployment;
    for (int slot : slots) {
        uint16_t person = 0xFFFF;
        memcpy(&person, units.data() + slot * kUnitStride, sizeof(person));
        BattleUnitSnapshot unit;
        unit.slot = slot;
        unit.dataId = person;
        unit.camp = units[slot * kUnitStride + 5];
        unit.visibility = 2;
        unit.x = slot % 20;
        unit.y = slot / 20;
        snapshot.units.push_back(unit);
    }
    ObserveBattleStatsSnapshotForTest(snapshot);
}

bool WriteJoinScenarioForTest(const std::wstring& path, int personId) {
    std::vector<BYTE> body;
    auto word = [](std::vector<BYTE>& bytes, unsigned value) {
        bytes.push_back(static_cast<BYTE>(value));
        bytes.push_back(static_cast<BYTE>(value >> 8));
    };
    word(body, 0x3B);
    const unsigned tags[] = {0x02, 0x0E, 0x3E};
    const int values[] = {personId, 0, 0};
    for (size_t i = 0; i < 3; ++i) { word(body, tags[i]); word(body, values[i]); }
    std::vector<BYTE> bytes(10, 0);
    bytes[0] = 'E'; bytes[1] = 'E'; bytes[2] = 'X';
    bytes.push_back(14); bytes.insert(bytes.end(), 3, 0);
    word(bytes, 1); word(bytes, static_cast<unsigned>(body.size() + 4));
    word(bytes, 0); word(bytes, static_cast<unsigned>(body.size()));
    bytes.insert(bytes.end(), body.begin(), body.end());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool okay = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != FALSE;
    return okay && closed;
}

void ConfigureRecovery(std::array<BYTE, 0x40>& context, std::array<BYTE, 0x80>& frame,
    int kind, BYTE strategyId, BYTE actorSlot, BYTE initialTargetSlot,
    uintptr_t initialTarget, BYTE resultTargetSlot, uintptr_t resultTarget,
    int resultIndex = 0) {
    context.fill(0);
    // The resolver consumes the bounded low-byte result slot at [EBP-04].
    // Leave surrounding stack bytes noisy so a DWORD read cannot pass by
    // accident on the path whose increment is byte-sized.
    frame.fill(0xA5);
    BYTE* callerFrame = frame.data() + 0x40;
    PutByte(context.data(), strategyId);
    PutByte(context.data() + 1, initialTargetSlot);
    PutByte(context.data() + 2, actorSlot);
    PutPointer(context.data() + 4, static_cast<uintptr_t>(0x12345678));
    PutPointer(context.data() + 8, initialTarget);
    PutByte(context.data() + 0x0D + resultIndex, resultTargetSlot);
    PutPointer(callerFrame - 0x0C, reinterpret_cast<uintptr_t>(context.data()));
    PutByte(callerFrame - 0x04, static_cast<BYTE>(resultIndex));
    PutPointer(callerFrame - (kind == 2 ? 0x10 : 0x14), resultTarget);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool engine65 = CompiledEngineProfile() == EngineProfileId::Ccz64 || CompiledEngineProfile() == EngineProfileId::Ccz65;
    const uintptr_t recoverySimple = engine65 ? 0x4247E6 : 0x4247DC;
    const uintptr_t recoveryAnimated = engine65 ? 0x4249F1 : 0x4249DA;
    const uintptr_t damageCall = CompiledEngineProfile() == EngineProfileId::Ccz64 ? 0x405B0B : 0x405ADC;
    if (argc != 2) return 2;
    std::vector<BYTE> image;
    DWORD base = 0;
    Check(LoadImage(argv[1], image, base), "load x86 game image");

    BattleStatsImageLayout layout;
    Check(ResolveBattleStatsImageLayoutForTest(image.data(), base, layout),
        "resolve settlement and actual physical-event graph");
    Check(layout.hpSetter == 0x43F6DB, "resolve the shared clamping HP setter");
    Check(layout.strategyBase == 0x4A3E77 && layout.strategyCount == kStrategyCount,
        "derive the loaded strategy table and its row count from executable semantics");
    Check(layout.settlementCalls.size() == 4 &&
        layout.settlementCalls[0] == damageCall &&
        layout.settlementCalls[1] == 0x4229FC &&
        layout.settlementCalls[2] == recoverySimple &&
        layout.settlementCalls[3] == recoveryAnimated,
        "resolve damage and both real recovery-result calls");
    Check(layout.physicalResultCall == 0x452802 && layout.criticalCommit == 0x405F1A,
        "resolve actual-result entry and committed critical decision");
    Check(layout.criticalOffset == 0x604 && layout.comboOffset == 0x608,
        "derive adjacent critical and extra-strike fields semantically");
    Check(layout.attackBlockCommit == 0 && layout.spellBlockCommit == 0,
        "do not infer physical or spell block commits from unverified hit paths");

    std::vector<BYTE> changed = image;
    changed[recoverySimple - base] = 0x90;
    BattleStatsImageLayout partial;
    Check(ResolveBattleStatsImageLayoutForTest(changed.data(), base, partial) &&
        partial.settlementCalls.size() == 4 && !partial.settlementCalls[2] &&
        !partial.settlementCalls[3] &&
        partial.settlementCalls[0] == damageCall && partial.settlementCalls[1] == 0x4229FC,
        "disable complete recovery capability when either result path is missing");
    changed = image;
    changed[0x405F1A - base] = 0x90;
    Check(ResolveBattleStatsImageLayoutForTest(changed.data(), base, partial) &&
        !partial.criticalCommit && !partial.physicalResultCall &&
        partial.settlementCalls[2] == recoverySimple &&
        partial.settlementCalls[3] == recoveryAnimated,
        "isolate an invalid critical graph from recovery settlement support");

    std::vector<BYTE> units(kUnitStride * kUnitCount, 0);
    for (size_t slot = 0; slot < kUnitCount; ++slot)
        PutPerson(units.data() + slot * kUnitStride, static_cast<uint16_t>(1000 + slot));
    constexpr BYTE actorSlot = 2;
    constexpr BYTE friendlyTargetSlot = 7;
    constexpr BYTE secondTargetSlot = 8;
    constexpr BYTE enemyTargetSlot = 100;
    constexpr BYTE enemyActorSlot = 101;
    units[actorSlot * kUnitStride + 5] = 0;
    units[friendlyTargetSlot * kUnitStride + 5] = 0;
    units[secondTargetSlot * kUnitStride + 5] = 0;
    units[enemyTargetSlot * kUnitStride + 5] = 2;
    units[enemyActorSlot * kUnitStride + 5] = 2;
    const uintptr_t unitBase = reinterpret_cast<uintptr_t>(units.data());
    const uintptr_t actor = unitBase + actorSlot * kUnitStride;
    const uintptr_t friendlyTarget = unitBase + friendlyTargetSlot * kUnitStride;
    const uintptr_t secondTarget = unitBase + secondTargetSlot * kUnitStride;
    const uintptr_t enemyTarget = unitBase + enemyTargetSlot * kUnitStride;

    std::vector<BYTE> strategies(kStrategyStride * kStrategyCount, 0);
    strategies[kSupplyStrategy * kStrategyStride + kStrategyTypeOffset] = 19;
    strategies[kPersistentRecoveryStrategy * kStrategyStride + kStrategyTypeOffset] = 43;
    InitializeBattleStatsModelForTest(unitBase,
        reinterpret_cast<uintptr_t>(strategies.data()), kStrategyCount);
    SetBattleRosterCapability(true, L"test-verified-3B-bridge");
    NotifyBattleStatsScenarioInstalled(0, true, false, 1);
    for (int slot : {actorSlot, friendlyTargetSlot, secondTargetSlot})
        NotifyBattleRosterJoined(1000 + slot, 0, slot, 1);
    NotifyBattleStatsScenarioInstalled(1, false, false, 2);
    DeployForTest(units, {actorSlot, friendlyTargetSlot, secondTargetSlot});
    std::array<BYTE, 0x40> context = {};
    std::array<BYTE, 0x80> frame = {};

    // Exercise the production bridge with a non-zero stack frame.  This
    // catches both the BYTE result index contract and PUSHAD stack offsets;
    // the direct model calls below intentionally remain as fine-grained
    // resolver/aggregation coverage.
    ConfigureRecovery(context, frame, 2, kSupplyStrategy, actorSlot, actorSlot,
        actor, actorSlot, actor);
    units[actorSlot * kUnitStride + 0x10] = 37;
    gBattleStatsOriginalSetHp = reinterpret_cast<uintptr_t>(&TestHpSetter);
    InvokeRecoveryBridgeForTest(actor, reinterpret_cast<uintptr_t>(context.data()));
    gBattleStatsOriginalSetHp = 0;
    BattleStatsSnapshot bridgeSnapshot;
    Check(ReadBattleStatsSnapshot(bridgeSnapshot) &&
        FindUnit(bridgeSnapshot, actorSlot) &&
        FindUnit(bridgeSnapshot, actorSlot)->recoveryDone == 45 &&
        units[actorSlot * kUnitStride + 0x10] == 82,
        "run the recovery bridge with the native setter ABI and final HP delta");

    ConfigureRecovery(context, frame, 2, kSupplyStrategy, actorSlot, friendlyTargetSlot,
        friendlyTarget, friendlyTargetSlot, friendlyTarget);
    RecordBattleStatsSettlementForTest(2, friendlyTarget,
        reinterpret_cast<uintptr_t>(frame.data() + 0x40), 37, 82);
    RecordBattleStatsSettlementForTest(2, friendlyTarget,
        reinterpret_cast<uintptr_t>(frame.data() + 0x40), 90, 100);
    RecordBattleStatsSettlementForTest(2, friendlyTarget,
        reinterpret_cast<uintptr_t>(frame.data() + 0x40), 100, 100);
    ConfigureRecovery(context, frame, 3, kSupplyStrategy, actorSlot, actorSlot,
        actor, actorSlot, actor);
    RecordBattleStatsSettlementForTest(3, actor,
        reinterpret_cast<uintptr_t>(frame.data() + 0x40), 20, 50);
    ConfigureRecovery(context, frame, 3, kSupplyStrategy, actorSlot, friendlyTargetSlot,
        friendlyTarget, secondTargetSlot, secondTarget, 1);
    RecordBattleStatsSettlementForTest(3, secondTarget,
        reinterpret_cast<uintptr_t>(frame.data() + 0x40), 10, 25);
    ConfigureRecovery(context, frame, 2, kSupplyStrategy, enemyActorSlot, friendlyTargetSlot,
        friendlyTarget, friendlyTargetSlot, friendlyTarget);
    RecordBattleStatsSettlementForTest(2, friendlyTarget,
        reinterpret_cast<uintptr_t>(frame.data() + 0x40), 40, 70);
    ConfigureRecovery(context, frame, 2, kPersistentRecoveryStrategy, actorSlot,
        friendlyTargetSlot, friendlyTarget, friendlyTargetSlot, friendlyTarget);
    RecordBattleStatsSettlementForTest(2, friendlyTarget,
        reinterpret_cast<uintptr_t>(frame.data() + 0x40), 40, 70);
    ConfigureRecovery(context, frame, 2, kSupplyStrategy, actorSlot, friendlyTargetSlot,
        friendlyTarget, friendlyTargetSlot, friendlyTarget);
    PutPointer(frame.data() + 0x40 - 0x10, secondTarget);
    RecordBattleStatsSettlementForTest(2, friendlyTarget,
        reinterpret_cast<uintptr_t>(frame.data() + 0x40), 40, 70);
    ConfigureRecovery(context, frame, 2, kSupplyStrategy, actorSlot, friendlyTargetSlot,
        friendlyTarget, secondTargetSlot, friendlyTarget);
    RecordBattleStatsSettlementForTest(2, friendlyTarget,
        reinterpret_cast<uintptr_t>(frame.data() + 0x40), 40, 70);
    ConfigureRecovery(context, frame, 2, kSupplyStrategy, actorSlot, friendlyTargetSlot,
        secondTarget, friendlyTargetSlot, friendlyTarget);
    RecordBattleStatsSettlementForTest(2, friendlyTarget,
        reinterpret_cast<uintptr_t>(frame.data() + 0x40), 40, 70);

    context.fill(0);
    frame.fill(0);
    BYTE* callerFrame = frame.data() + 0x40;
    PutByte(context.data() + 1, actorSlot);
    PutPointer(context.data() + 8, actor);
    PutByte(context.data() + 0x0C, enemyTargetSlot);
    PutPointer(callerFrame - 4, reinterpret_cast<uintptr_t>(context.data()));
    RecordBattleStatsSettlementForTest(1, enemyTarget,
        reinterpret_cast<uintptr_t>(callerFrame), 80, 50);

    std::array<BYTE, 0x700> physical = {};
    PutByte(physical.data(), actorSlot);
    PutByte(physical.data() + 1, 0xFF);
    PutByte(physical.data() + kComboOffset, 0);
    RecordBattleStatsPhysicalResultForTest(reinterpret_cast<uintptr_t>(physical.data()), kComboOffset);
    PutByte(physical.data() + 1, enemyTargetSlot);
    RecordBattleStatsCriticalForTest(reinterpret_cast<uintptr_t>(physical.data()), kComboOffset);
    RecordBattleStatsCriticalForTest(reinterpret_cast<uintptr_t>(physical.data()), kComboOffset);
    PutByte(physical.data() + kComboOffset, 1);
    RecordBattleStatsPhysicalResultForTest(reinterpret_cast<uintptr_t>(physical.data()), kComboOffset);
    RecordBattleStatsPhysicalResultForTest(reinterpret_cast<uintptr_t>(physical.data()), kComboOffset);
    RecordBattleStatsCriticalForTest(reinterpret_cast<uintptr_t>(physical.data()), kComboOffset);
    PutByte(physical.data() + 1, static_cast<BYTE>(enemyTargetSlot + 1));
    units[(enemyTargetSlot + 1) * kUnitStride + 5] = 2;
    RecordBattleStatsCriticalForTest(reinterpret_cast<uintptr_t>(physical.data()), kComboOffset);
    PutByte(physical.data() + kComboOffset, 2);
    RecordBattleStatsPhysicalResultForTest(reinterpret_cast<uintptr_t>(physical.data()), kComboOffset);

    std::array<BYTE, 0x700> enemyPhysical = {};
    PutByte(enemyPhysical.data(), enemyActorSlot);
    PutByte(enemyPhysical.data() + 1, actorSlot);
    PutByte(enemyPhysical.data() + kComboOffset, 0);
    RecordBattleStatsPhysicalResultForTest(reinterpret_cast<uintptr_t>(enemyPhysical.data()), kComboOffset);
    RecordBattleStatsCriticalForTest(reinterpret_cast<uintptr_t>(enemyPhysical.data()), kComboOffset);

    PutByte(physical.data() + 1, enemyTargetSlot);
    PutByte(physical.data() + kComboOffset, 0);
    RecordBattleStatsPhysicalResultForTest(reinterpret_cast<uintptr_t>(physical.data()), kComboOffset);
    RecordBattleStatsCriticalForTest(reinterpret_cast<uintptr_t>(physical.data()), kComboOffset);

    // Only explicit confirmed-result calls count as blocks.  Unchanged HP is
    // intentionally not used because it also covers misses, immunity and
    // redirected attacks.
    RecordBattleStatsAttackBlockForTest(enemyActorSlot, actorSlot);
    RecordBattleStatsSpellBlockForTest(enemyActorSlot, friendlyTargetSlot);
    RecordBattleStatsAttackBlockForTest(actorSlot, enemyTargetSlot);
    RecordBattleStatsSpellBlockForTest(enemyActorSlot, enemyTargetSlot);
    SetBattleStatsModelActiveForTest(false);
    RecordBattleStatsAttackBlockForTest(enemyActorSlot, actorSlot);
    SetBattleStatsModelActiveForTest(true);

    BattleStatsSnapshot snapshot;
    Check(ReadBattleStatsSnapshot(snapshot) && snapshot.physicalSettlementAvailable &&
        snapshot.strategyDamageAvailable && snapshot.recoveryAvailable &&
        snapshot.criticalAvailable && snapshot.comboAvailable &&
        snapshot.attackBlockAvailable && snapshot.spellBlockAvailable,
        "publish seven independent battle-stat capabilities");
    const BattleUnitTotals* actorTotals = FindUnit(snapshot, actorSlot);
    const BattleUnitTotals* friendlyTotals = FindUnit(snapshot, friendlyTargetSlot);
    const BattleUnitTotals* secondTotals = FindUnit(snapshot, secondTargetSlot);
    Check(actorTotals && actorTotals->recoveryDone == 75,
        "attribute self recovery to the unit whose HP increased");
    Check(friendlyTotals && friendlyTotals->recoveryDone == 205,
        "accumulate every positive friendly HP delta on its target unit");
    Check(secondTotals && secondTotals->recoveryDone == 15,
        "accumulate positive HP deltas for each multi-target recipient");
    Check(actorTotals && actorTotals->damageDealt == 30,
        "keep strategy-damage actor resolution separate from recovery layout");
    Check(actorTotals && actorTotals->comboExtraHitCount == 2,
        "count each actual extra-strike ordinal once");
    Check(actorTotals && actorTotals->criticalCount == 4,
        "count committed critical decisions once per action, strike, and target");
    Check(actorTotals && actorTotals->attackBlockCount == 1,
        "attribute confirmed physical blocks to the friendly defender");
    Check(friendlyTotals && friendlyTotals->spellBlockCount == 1,
        "attribute confirmed spell blocks to the friendly defender");
    Check(!FindUnit(snapshot, enemyActorSlot),
        "exclude enemy recovery and physical events from friendly totals");

    BattleStatsPersistedState persisted;
    Check(CaptureBattleStatsState(persisted, 1000) && persisted.active &&
        persisted.units.size() == snapshot.units.size(),
        "capture active battle statistics before closing the game");
    ResetBattleStats();
    BattleStatsSnapshot cleared;
    Check(ReadBattleStatsSnapshot(cleared) && !cleared.active && cleared.units.empty(),
        "clear live statistics independently of the persisted state");
    Check(RestoreBattleStatsState(persisted, 2000),
        "restore persisted statistics after a later game launch");
    BattleStatsSnapshot restored;
    bool same = ReadBattleStatsSnapshot(restored) && restored.active &&
        restored.units.size() == snapshot.units.size();
    if (same) {
        for (const BattleUnitTotals& expected : snapshot.units) {
            const BattleUnitTotals* actual = FindUnit(restored, expected.slot);
            if (!actual || !SameTotals(*actual, expected)) { same = false; break; }
        }
    }
    Check(same, "preserve every unit total when statistics are restored");

    bool sameCampaign = snapshot.units.size() == restored.campaignUnits.size();
    for (const auto& expected : snapshot.units) {
        const auto* actual = FindPerson(restored.campaignUnits, expected.personId);
        if (!actual || !SameTotals(expected, *actual)) sameCampaign = false;
    }
    Check(sameCampaign, "update current and campaign ledgers from every confirmed event atomically");

    BattleStatsPersistedState longElapsed = persisted;
    longElapsed.elapsedMilliseconds = 9000000000000ULL;
    Check(RestoreBattleStatsState(longElapsed, 100), "restore elapsed time beyond current system uptime");
    BattleStatsPersistedState timed;
    Check(CaptureBattleStatsState(timed, 175) &&
        timed.elapsedMilliseconds == longElapsed.elapsedMilliseconds + 75,
        "keep accumulated duration when a saved game is opened after an operating system restart");

    NotifyBattleStatsScenarioInstalled(0, true, false, 999);
    NotifyBattleRosterJoined(1000 + actorSlot, 0, 14, 999);
    NotifyBattleStatsScenarioInstalled(1, false, true, 1000);
    BattleStatsSnapshot fresh;
    ReadBattleStatsSnapshot(fresh);
    Check(fresh.active && fresh.units.empty() && fresh.campaignUnits.size() == 1 &&
        fresh.campaignId && fresh.battleInstance == 1,
        "new campaign starts an empty battlefield and retains the successful R join as a global zero row");
    DeployForTest(units, {actorSlot});
    RecordBattleStatsSettlementForTest(1, enemyTarget,
        reinterpret_cast<uintptr_t>(callerFrame), 80, 50);
    NotifyBattleStatsScenarioInstalled(1, false, false, 1100);
    ReadBattleStatsSnapshot(fresh);
    Check(FindPerson(fresh.units, 1000 + actorSlot) &&
        FindPerson(fresh.units, 1000 + actorSlot)->damageDealt == 30,
        "reobserving the installed scenario preserves the current instance");
    const uint64_t campaignIdentity = fresh.campaignId;
    NotifyBattleStatsScenarioInstalled(1, false, true, 1200);
    ReadBattleStatsSnapshot(fresh);
    Check(fresh.units.empty() && fresh.battleInstance == 2 && fresh.campaignId == campaignIdentity &&
        FindPerson(fresh.campaignUnits, 1000 + actorSlot)->damageDealt == 30,
        "a new instance of the same S clears only current statistics");
    Check(fresh.currentBattleParticipants.empty(), "new S instance clears all previous participants");
    DeployForTest(units, {actorSlot});
    RecordBattleStatsSettlementForTest(1, enemyTarget,
        reinterpret_cast<uintptr_t>(callerFrame), 80, 50);
    NotifyBattleStatsScenarioInstalled(2, false, false, 1300);
    RecordBattleStatsSettlementForTest(1, enemyTarget,
        reinterpret_cast<uintptr_t>(callerFrame), 80, 50);
    ReadBattleStatsSnapshot(fresh);
    const BattleUnitTotals* rCurrent = FindPerson(fresh.units, 1000 + actorSlot);
    const BattleUnitTotals* rCampaign = FindPerson(fresh.campaignUnits, 1000 + actorSlot);
    Check(!fresh.active && !rCurrent && rCampaign && rCampaign->damageDealt == 60,
        "R scenes clear the current battlefield while retaining campaign totals and pausing collection");
    NotifyBattleStatsScenarioInstalled(3, false, false, 1400);
    ReadBattleStatsSnapshot(fresh);
    Check(fresh.active && fresh.units.empty() && fresh.campaignUnits.size() == 1,
        "the next S starts an empty current ledger while retaining all campaign people");

    constexpr BYTE movedSlot = 3;
    PutPerson(units.data() + movedSlot * kUnitStride, 1000 + actorSlot);
    PutPerson(units.data() + actorSlot * kUnitStride, 902);
    DeployForTest(units, {movedSlot, actorSlot});
    PutByte(context.data() + 1, movedSlot);
    PutPointer(context.data() + 8, unitBase + movedSlot * kUnitStride);
    RecordBattleStatsSettlementForTest(1, enemyTarget,
        reinterpret_cast<uintptr_t>(callerFrame), 80, 50);
    PutByte(context.data() + 1, actorSlot);
    PutPointer(context.data() + 8, actor);
    RecordBattleStatsSettlementForTest(1, enemyTarget,
        reinterpret_cast<uintptr_t>(callerFrame), 80, 50);
    ReadBattleStatsSnapshot(fresh);
    Check(FindPerson(fresh.campaignUnits, 1000 + actorSlot)->damageDealt == 90 &&
        !FindPerson(fresh.campaignUnits, 902) && FindPerson(fresh.units, 902) && fresh.units.size() == 2,
        "slot reuse keeps current identities separate and never creates an unjoined campaign row");

    SetBattleStatsLoading(true);
    RecordBattleStatsSettlementForTest(1, enemyTarget,
        reinterpret_cast<uintptr_t>(callerFrame), 80, 50);
    NotifyBattleStatsScenarioInstalled(5, true, true, 1500);
    BattleStatsPersistedState frozen;
    Check(CaptureBattleStatsState(frozen, GetTickCount64()) && frozen.scenarioOrdinal == 3 &&
        !FindPerson(frozen.campaignUnits, 902) && FindPerson(frozen.units, 902)->damageDealt == 30,
        "loading freezes both ledgers and ignores scenario reset notifications");
    Check(RestoreBattleStatsState(persisted, GetTickCount64()), "replace ledgers with the loaded save");
    SetBattleStatsLoading(false);
    ReadBattleStatsSnapshot(fresh);
    Check(fresh.campaignUnits.size() == persisted.campaignUnits.size() &&
        !FindPerson(fresh.campaignUnits, 902) &&
        FindPerson(fresh.campaignUnits, 1000 + actorSlot)->damageDealt == 30,
        "loading an older save rolls campaign totals back instead of merging timelines");

    BattleStatsPersistedState legacy = persisted;
    legacy.schemaVersion = 1;
    legacy.campaignUnits.clear();
    legacy.campaignRoster.clear();
    legacy.currentBattleParticipants.clear();
    for (auto& unit : legacy.units) unit.personId = -1;
    Check(RestoreBattleStatsState(legacy, 2000), "migrate slot-indexed legacy statistics through current people");
    ReadBattleStatsSnapshot(fresh);
    Check(fresh.historyIncomplete && FindPerson(fresh.units, 902) &&
        fresh.units.size() == fresh.campaignUnits.size(),
        "legacy migration seeds campaign totals and marks the missing earlier campaign history");

    InitializeBattleStatsModelForTest(unitBase,
        reinterpret_cast<uintptr_t>(strategies.data()), kStrategyCount);
    NotifyBattleStatsScenarioInstalled(0, true, false, 3000);
    NotifyBattleRosterJoined(902, 0, 10, 3000);
    ReadBattleStatsSnapshot(fresh);
    Check(!fresh.rosterAvailable && fresh.campaignUnits.empty(),
        "an unavailable runtime bridge cannot create a campaign membership from an unverified notification");
    SetBattleRosterCapability(true, L"verified test dispatcher");
    NotifyBattleRosterJoined(902, 0, 10, 3000);
    NotifyBattleRosterJoined(902, 0, 11, 3001);
    NotifyBattleRosterJoined(55, 0, 12, 3002);
    ReadBattleStatsSnapshot(fresh);
    Check(fresh.rosterAvailable && fresh.campaignUnits.size() == 2 &&
        FindPerson(fresh.campaignUnits, 55)->damageDealt == 0 &&
        fresh.campaignRoster[1].personId == 902 &&
        fresh.campaignRoster[1].joinCommandOrdinal == 10,
        "successful 3B joins create zero reserve rows once and retain the first command source");
    NotifyBattleStatsScenarioInstalled(1, false, false, 3010);
    BattleSnapshot verifiedDeployment;
    verifiedDeployment.capabilities = BattleCapabilityUnits | BattleCapabilityDeployment;
    ObserveBattleStatsSnapshotForTest(verifiedDeployment);
    PutByte(context.data() + 1, actorSlot);
    PutPointer(context.data() + 8, actor);
    RecordBattleStatsSettlementForTest(1, enemyTarget,
        reinterpret_cast<uintptr_t>(callerFrame), 80, 50);
    ReadBattleStatsSnapshot(fresh);
    Check(fresh.units.empty() && FindPerson(fresh.campaignUnits, 902)->damageDealt == 30,
        "an empty deployment set cannot be bypassed by a friendly damage event");
    units[actorSlot * kUnitStride + 0x0C] = 2;
    units[actorSlot * kUnitStride + 6] = 3;
    units[actorSlot * kUnitStride + 7] = 4;
    RecordBattleStatsSettlementForTest(1, enemyTarget,
        reinterpret_cast<uintptr_t>(callerFrame), 80, 50);
    ReadBattleStatsSnapshot(fresh);
    Check(FindPerson(fresh.units, 902) && FindPerson(fresh.units, 902)->damageDealt == 30,
        "the first verified S event confirms deployment before the first periodic poll");
    PutPerson(units.data() + secondTargetSlot * kUnitStride, 55);
    units[secondTargetSlot * kUnitStride + 0x0C] = 2;
    units[secondTargetSlot * kUnitStride + 6] = 5;
    units[secondTargetSlot * kUnitStride + 7] = 6;
    RecordBattleStatsSettlementForTest(2, secondTarget, 0, 40, 50);
    ReadBattleStatsSnapshot(fresh);
    Check(FindPerson(fresh.units, 55) && FindPerson(fresh.units, 55)->recoveryDone == 10 &&
        FindPerson(fresh.campaignUnits, 55)->recoveryDone == 10,
        "a real reinforcement is confirmed by its first event without waiting for a poll");
    NotifyBattleRosterCamp(902, 2, 3020);
    RecordBattleStatsSettlementForTest(1, enemyTarget,
        reinterpret_cast<uintptr_t>(callerFrame), 80, 50);
    ReadBattleStatsSnapshot(fresh);
    Check(FindPerson(fresh.units, 902)->damageDealt == 30 &&
        FindPerson(fresh.campaignUnits, 902)->damageDealt == 60 &&
        fresh.campaignRoster[1].changedFaction && !fresh.campaignRoster[1].tracking,
        "a confirmed faction change freezes both ledgers even when the tactical slot still says friendly");
    NotifyBattleRosterCamp(902, 0, 3021);
    RecordBattleStatsSettlementForTest(1, enemyTarget,
        reinterpret_cast<uintptr_t>(callerFrame), 80, 50);
    ReadBattleStatsSnapshot(fresh);
    Check(FindPerson(fresh.units, 902)->damageDealt == 30 &&
        FindPerson(fresh.campaignUnits, 902)->damageDealt == 60,
        "returning to camp zero cannot silently resume a changed-faction member");
    NotifyBattleStatsScenarioInstalled(2, false, false, 3030);
    NotifyBattleRosterJoined(902, 2, 20, 3030);
    units[actorSlot * kUnitStride + 5] = 2;
    DeployForTest(units, {actorSlot});
    ReadBattleStatsSnapshot(fresh);
    Check(fresh.campaignRoster[1].tracking && !fresh.campaignRoster[1].changedFaction,
        "stale S tactical camps in an R scene cannot cancel a successful R rejoin");
    units[actorSlot * kUnitStride + 5] = 0;
    NotifyBattleStatsScenarioInstalled(3, false, false, 3040);
    DeployForTest(units, {actorSlot});
    RecordBattleStatsSettlementForTest(1, enemyTarget,
        reinterpret_cast<uintptr_t>(callerFrame), 80, 50);
    ReadBattleStatsSnapshot(fresh);
    Check(FindPerson(fresh.units, 902)->damageDealt == 30 &&
        FindPerson(fresh.campaignUnits, 902)->damageDealt == 90 &&
        fresh.campaignRoster[1].joinScenarioOrdinal == 2 &&
        fresh.campaignRoster[1].joinCommandOrdinal == 20,
        "only a later successful R 3B rejoin resumes tracking and updates the join source");
    NotifyBattleRosterCamp(902, 255, 3041);
    ReadBattleStatsSnapshot(fresh);
    Check(fresh.campaignRoster[1].lastCamp == 255 && !fresh.campaignRoster[1].tracking,
        "the full native camp byte including the removal sentinel stops tracking");

    ShutdownBattleStats();
    Check(RestoreBattleStatsState(persisted, 3000), "restore historical totals when collection hooks are unavailable");
    ReadBattleStatsSnapshot(fresh);
    Check(!fresh.hookAvailable && fresh.units.size() == persisted.units.size() &&
        fresh.campaignUnits.size() == persisted.campaignUnits.size() &&
        CaptureBattleStatsState(frozen, 3100),
        "historical display and saving do not depend on current collection-hook support");

    wchar_t temporary[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temporary);
    const std::filesystem::path migrationRoot = std::filesystem::path(temporary) /
        (L"CczRosterMigration-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(migrationRoot / L"RS");
    const std::wstring migrationText = migrationRoot.wstring();
    const std::wstring rPath = (migrationRoot / L"RS" / L"R_00.eex").wstring();
    Check(WriteJoinScenarioForTest(rPath, 56), "write an isolated R 3B migration fixture");
    std::vector<int> migrationCandidates;
    std::wstring migrationDiagnostic;
    const bool extracted = ExtractScenarioJoinCandidates(rPath, migrationCandidates, migrationDiagnostic);
    if (!extracted || migrationCandidates != std::vector<int>{56})
        printf("migration parser: success=%d candidates=%zu diagnostic=%ls\n", extracted,
            migrationCandidates.size(), migrationDiagnostic.c_str());
    Check(extracted &&
        migrationCandidates == std::vector<int>{56}, "the shared EEX parser finds the static 3B migration candidate");
    InitializeBattleStatsModelForTest(unitBase,
        reinterpret_cast<uintptr_t>(strategies.data()), kStrategyCount, migrationText.c_str());
    for (uint32_t schema : {1u, 2u, 3u}) {
        BattleStatsPersistedState old = persisted;
        old.schemaVersion = schema;
        old.scenarioOrdinal = 1;
        old.campaignRoster.clear();
        old.currentBattleParticipants.clear();
        if (schema == 1) old.campaignUnits.clear();
        Check(RestoreBattleStatsState(old, 5000), "migrate a legacy schema through the corresponding R scenario");
        ReadBattleStatsSnapshot(fresh);
        bool inferred = false;
        for (const auto& entry : fresh.campaignRoster)
            if (entry.personId == 56) inferred = entry.source == BattleRosterSource::LegacyStaticR &&
                entry.joinScenarioOrdinal == 0;
        Check(inferred && FindPerson(fresh.campaignUnits, 56) &&
            FindPerson(fresh.campaignUnits, 56)->damageDealt == 0 &&
            fresh.historyIncomplete && fresh.rosterHistoryIncomplete &&
            fresh.status.find(L"可能包含未执行分支") != std::wstring::npos,
            "legacy-only R inference creates a zero reserve row and explicitly warns about unexecuted branches");
        old.active = false;
        old.scenarioOrdinal = 0;
        Check(RestoreBattleStatsState(old, 5001), "restore a legacy R-scene save");
        ReadBattleStatsSnapshot(fresh);
        Check(fresh.units.empty() && fresh.currentBattleParticipants.empty(),
            "legacy R migration always clears the current battlefield and participant set");
        SetBattleRosterCapability(true, L"verified migration test dispatcher");
        NotifyBattleRosterJoined(56, 0, 40, 5002);
        ReadBattleStatsSnapshot(fresh);
        bool upgraded = false;
        for (const auto& entry : fresh.campaignRoster)
            if (entry.personId == 56) upgraded = entry.source == BattleRosterSource::RuntimeJoin3B &&
                entry.joinCommandOrdinal == 40;
        Check(upgraded, "a real later 3B upgrades legacy inferred membership to verified runtime provenance");
        SetBattleStatsLoading(true);
        NotifyBattleRosterJoined(57, 0, 41, 5003);
        NotifyBattleRosterCamp(56, 255, 5003);
        NotifyBattleRosterLabel(56, L"加载期间禁止改名");
        SetBattleStatsLoading(false);
        ReadBattleStatsSnapshot(fresh);
        Check(!FindPerson(fresh.campaignUnits, 57) && FindPerson(fresh.campaignUnits, 56) &&
            FindPerson(fresh.campaignUnits, 56)->label != L"加载期间禁止改名",
            "loading suppresses membership, camp and label notifications from the previous timeline");
    }
    BattleStatsPersistedState newGame;
    newGame.scenarioOrdinal = 0;
    Check(RestoreBattleStatsState(newGame, 5010), "restore an empty schema-4 R save beside the same static script");
    ReadBattleStatsSnapshot(fresh);
    Check(fresh.campaignRoster.empty() && fresh.campaignUnits.empty(),
        "schema-4 saves never infer membership from a statically present unexecuted 3B");
    std::filesystem::remove_all(migrationRoot);

    ShutdownBattleStats();
    printf("BattleStatsTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
