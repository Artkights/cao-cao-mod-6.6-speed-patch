#include "BattleStats.h"

#include <array>
#include <cstring>
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

const BattleUnitTotals* FindUnit(const BattleStatsSnapshot& snapshot, int slot) {
    for (const BattleUnitTotals& unit : snapshot.units)
        if (unit.slot == slot) return &unit;
    return nullptr;
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
        layout.settlementCalls[0] == 0x405ADC &&
        layout.settlementCalls[1] == 0x4229FC &&
        layout.settlementCalls[2] == 0x4247DC &&
        layout.settlementCalls[3] == 0x4249DA,
        "resolve damage and both real recovery-result calls");
    Check(layout.physicalResultCall == 0x452802 && layout.criticalCommit == 0x405F1A,
        "resolve actual-result entry and committed critical decision");
    Check(layout.criticalOffset == 0x604 && layout.comboOffset == 0x608,
        "derive adjacent critical and extra-strike fields semantically");

    std::vector<BYTE> changed = image;
    changed[0x4247DC - base] = 0x90;
    BattleStatsImageLayout partial;
    Check(ResolveBattleStatsImageLayoutForTest(changed.data(), base, partial) &&
        partial.settlementCalls.size() == 4 && !partial.settlementCalls[2] &&
        !partial.settlementCalls[3] &&
        partial.settlementCalls[0] == 0x405ADC && partial.settlementCalls[1] == 0x4229FC,
        "disable complete recovery capability when either result path is missing");
    changed = image;
    changed[0x405F1A - base] = 0x90;
    Check(ResolveBattleStatsImageLayoutForTest(changed.data(), base, partial) &&
        !partial.criticalCommit && !partial.physicalResultCall &&
        partial.settlementCalls[2] == 0x4247DC &&
        partial.settlementCalls[3] == 0x4249DA,
        "isolate an invalid critical graph from recovery settlement support");

    std::vector<BYTE> units(kUnitStride * kUnitCount, 0);
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
    SetBattleStatsModelActiveForTest(true);
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

    BattleStatsSnapshot snapshot;
    Check(ReadBattleStatsSnapshot(snapshot) && snapshot.physicalSettlementAvailable &&
        snapshot.strategyDamageAvailable && snapshot.recoveryAvailable &&
        snapshot.criticalAvailable && snapshot.comboAvailable,
        "publish five independent battle-stat capabilities");
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
    Check(!FindUnit(snapshot, enemyActorSlot),
        "exclude enemy recovery and physical events from friendly totals");

    ShutdownBattleStats();
    printf("BattleStatsTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
