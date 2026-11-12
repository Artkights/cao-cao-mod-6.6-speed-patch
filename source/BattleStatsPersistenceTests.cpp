#include "BattleStatsPersistence.h"
#include "InstallCommon.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

namespace {
int failures = 0;
const char kNativePayload[] = "native-E5S-prefix-preserved-byte-for-byte";

#pragma pack(push, 1)
struct TestFooter {
    char magic[8];
    uint32_t schema, footerBytes;
    uint64_t nativeBytes, payloadBytes;
    char nativeSha256[64], payloadSha256[64], end[8];
};
struct TestHeader {
    char magic[8];
    uint32_t schema, headerBytes, flags, unitCount, campaignUnitCount;
    uint32_t scenarioCharacters, statusCharacters;
    int32_t scenarioOrdinal;
    uint32_t knownMetrics;
    uint64_t elapsedMilliseconds, campaignId, battleInstance;
};
struct TestLegacyRow {
    int32_t personId, slot, defeatedEnemies;
    int64_t damageDealt, damageReceived, recoveryDone, criticalCount;
    int64_t comboExtraHitCount, attackBlockCount, spellBlockCount;
    uint32_t labelCharacters;
};
struct TestRosterHeader { uint32_t rosterCount, participantCount; };
struct TestRosterRow {
    int32_t personId, joinScenarioOrdinal, lastCamp;
    uint32_t joinCommandOrdinal;
    uint8_t joined, tracking, changedFaction, source;
    uint32_t labelCharacters;
};
#pragma pack(pop)

void Check(bool condition, const char* name) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    fflush(stdout);
    if (!condition) ++failures;
}

bool WriteBytes(const std::wstring& path, const std::vector<unsigned char>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool okay = bytes.size() <= MAXDWORD &&
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    if (!CloseHandle(file)) okay = false;
    return okay;
}

bool WriteSample(const std::wstring& path, const char* value) {
    return WriteBytes(path, std::vector<unsigned char>(value, value + strlen(value)));
}

std::vector<unsigned char> ReadBytes(const std::wstring& path) {
    std::vector<unsigned char> bytes;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return bytes;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 128 * 1024 * 1024) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
            read != bytes.size()) bytes.clear();
    }
    CloseHandle(file);
    return bytes;
}

BattleUnitTotals SampleUnit(int personId, int slot) {
    BattleUnitTotals unit;
    unit.personId = personId;
    unit.slot = slot;
    unit.label = L"刘备 / 人物 " + std::to_wstring(personId);
    unit.damageDealt = (std::numeric_limits<long long>::max)() - personId;
    unit.damageReceived = 345;
    unit.recoveryDone = 67;
    unit.criticalCount = 8;
    unit.comboExtraHitCount = 9;
    unit.attackBlockCount = 10;
    unit.spellBlockCount = 11;
    unit.defeatedEnemies = 12;
    return unit;
}

BattleStatsPersistedState SampleState() {
    BattleStatsPersistedState state;
    state.active = true;
    state.elapsedMilliseconds = (1ull << 60) + 456789;
    state.scenario = L"S_07.eex";
    state.scenarioOrdinal = 7;
    state.campaignId = 0xFF123456789ABCDEull;
    state.battleInstance = 0x1020304050607080ull;
    state.knownMetrics = 0xA5;
    state.historyIncomplete = true;
    state.status = L"已迁移旧统计；历史记录从此存档继续。";
    state.units.push_back(SampleUnit(7, 3));
    state.units.push_back(SampleUnit(65000, 6));
    state.campaignUnits = state.units;
    state.campaignUnits.push_back(SampleUnit(65534, -1));
    BattleRosterEntry joined;
    joined.personId = 7;
    joined.label = L"刘备 / 人物 7";
    joined.joined = true;
    joined.tracking = true;
    joined.lastCamp = 0;
    joined.joinScenarioOrdinal = 2;
    joined.joinCommandOrdinal = 14;
    joined.source = BattleRosterSource::RuntimeJoin3B;
    state.campaignRoster.push_back(joined);
    BattleRosterEntry changed;
    changed.personId = 65000;
    changed.label = state.campaignUnits[1].label;
    changed.joined = true;
    changed.changedFaction = true;
    changed.lastCamp = 2;
    changed.joinScenarioOrdinal = 4;
    changed.source = BattleRosterSource::LegacyStaticR;
    state.campaignRoster.push_back(changed);
    BattleRosterEntry reserve;
    reserve.personId = 65534;
    reserve.label = state.campaignUnits[2].label;
    reserve.joined = reserve.tracking = true;
    reserve.lastCamp = 0;
    reserve.source = BattleRosterSource::ExistingCampaignLedger;
    state.campaignRoster.push_back(reserve);
    state.currentBattleParticipants = {7, 65000};
    return state;
}

bool SameUnit(const BattleUnitTotals& left, const BattleUnitTotals& right) {
    return left.personId == right.personId && left.slot == right.slot && left.label == right.label &&
        left.damageDealt == right.damageDealt && left.damageReceived == right.damageReceived &&
        left.recoveryDone == right.recoveryDone && left.criticalCount == right.criticalCount &&
        left.comboExtraHitCount == right.comboExtraHitCount && left.attackBlockCount == right.attackBlockCount &&
        left.spellBlockCount == right.spellBlockCount && left.defeatedEnemies == right.defeatedEnemies &&
        left.physicalReceivedCount == right.physicalReceivedCount &&
        left.strategyReceivedCount == right.strategyReceivedCount;
}

bool SameState(const BattleStatsPersistedState& left, const BattleStatsPersistedState& right) {
    if (!(left.schemaVersion == right.schemaVersion && left.active == right.active &&
        left.elapsedMilliseconds == right.elapsedMilliseconds && left.scenario == right.scenario &&
        left.scenarioOrdinal == right.scenarioOrdinal && left.campaignId == right.campaignId &&
        left.battleInstance == right.battleInstance && left.knownMetrics == right.knownMetrics &&
        left.historyIncomplete == right.historyIncomplete && left.status == right.status &&
        left.units.size() == right.units.size() && left.campaignUnits.size() == right.campaignUnits.size() &&
        std::equal(left.units.begin(), left.units.end(), right.units.begin(), SameUnit) &&
        std::equal(left.campaignUnits.begin(), left.campaignUnits.end(), right.campaignUnits.begin(), SameUnit)))
        return false;
    if (left.rosterHistoryIncomplete != right.rosterHistoryIncomplete ||
        left.currentBattleParticipants != right.currentBattleParticipants ||
        left.campaignRoster.size() != right.campaignRoster.size()) return false;
    for (size_t i = 0; i < left.campaignRoster.size(); ++i) {
        const BattleRosterEntry& a = left.campaignRoster[i];
        const BattleRosterEntry& b = right.campaignRoster[i];
        if (a.personId != b.personId || a.label != b.label || a.joined != b.joined ||
            a.tracking != b.tracking || a.changedFaction != b.changedFaction ||
            a.lastCamp != b.lastCamp || a.joinScenarioOrdinal != b.joinScenarioOrdinal ||
            a.joinCommandOrdinal != b.joinCommandOrdinal ||
            a.source != b.source) return false;
    }
    return true;
}

bool ChildPathIsIsolated(const wchar_t* path) {
    wchar_t temporary[32768] = {}, absolute[32768] = {};
    if (!GetTempPathW(_countof(temporary), temporary) ||
        !GetFullPathNameW(path, _countof(absolute), absolute, nullptr)) return false;
    const std::wstring prefix = std::wstring(temporary) + L"CczBattleStats-";
    return _wcsnicmp(absolute, prefix.c_str(), prefix.size()) == 0;
}

bool RunChild(const wchar_t* mode, const std::wstring& path) {
    wchar_t executable[32768] = {};
    if (!GetModuleFileNameW(nullptr, executable, _countof(executable))) return false;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " + mode + L" \"" + path + L"\"";
    STARTUPINFOW startup = {sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD code = 1;
    const bool okay = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code) && code == 0;
    if (wait == WAIT_TIMEOUT) { TerminateProcess(process.hProcess, 3); WaitForSingleObject(process.hProcess, 5000); }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return okay;
}

bool RefreshPayloadHash(std::vector<unsigned char>& bytes) {
    if (bytes.size() < sizeof(TestFooter)) return false;
    TestFooter footer = {};
    memcpy(&footer, bytes.data() + bytes.size() - sizeof(footer), sizeof(footer));
    if (footer.nativeBytes + footer.payloadBytes + sizeof(footer) != bytes.size()) return false;
    std::vector<unsigned char> payload(bytes.begin() + static_cast<size_t>(footer.nativeBytes),
        bytes.end() - sizeof(footer));
    std::wstring hash, error;
    if (!XiaoAnInstall::Sha256Bytes(payload, hash, error) || hash.size() != sizeof(footer.payloadSha256)) return false;
    for (size_t i = 0; i < hash.size(); ++i) footer.payloadSha256[i] = static_cast<char>(hash[i]);
    memcpy(bytes.data() + bytes.size() - sizeof(footer), &footer, sizeof(footer));
    return true;
}

template<class T> void AppendFixture(std::vector<unsigned char>& bytes, const T& value) {
    const auto* first = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(value));
}

void AppendFixtureText(std::vector<unsigned char>& bytes, const std::wstring& text) {
    const auto* first = reinterpret_cast<const unsigned char*>(text.data());
    bytes.insert(bytes.end(), first, first + text.size() * sizeof(wchar_t));
}

// Build the original schema-2/3 wire formats independently of the schema-4
// writer: schema 2 has no received counters; neither legacy format has roster.
bool WriteLegacyFixture(const std::wstring& path, uint32_t schema,
    const BattleStatsPersistedState& state) {
    std::vector<unsigned char> payload;
    TestHeader header = {};
    memcpy(header.magic, "XASTAT2P", 8);
    header.schema = schema;
    header.headerBytes = sizeof(header);
    header.flags = state.active ? 1u : 0u;
    header.unitCount = static_cast<uint32_t>(state.units.size());
    header.campaignUnitCount = static_cast<uint32_t>(state.campaignUnits.size());
    header.scenarioCharacters = static_cast<uint32_t>(state.scenario.size());
    header.statusCharacters = static_cast<uint32_t>(state.status.size());
    header.scenarioOrdinal = state.scenarioOrdinal;
    header.knownMetrics = schema == 2 ? state.knownMetrics & 0xFFu : state.knownMetrics;
    header.elapsedMilliseconds = state.elapsedMilliseconds;
    header.campaignId = state.campaignId;
    header.battleInstance = state.battleInstance;
    AppendFixture(payload, header);
    AppendFixtureText(payload, state.scenario);
    AppendFixtureText(payload, state.status);
    for (const auto* group : {&state.units, &state.campaignUnits}) {
        for (const BattleUnitTotals& unit : *group) {
            const TestLegacyRow row = {unit.personId, unit.slot, unit.defeatedEnemies,
                unit.damageDealt, unit.damageReceived, unit.recoveryDone, unit.criticalCount,
                unit.comboExtraHitCount, unit.attackBlockCount, unit.spellBlockCount,
                static_cast<uint32_t>(unit.label.size())};
            AppendFixture(payload, row);
            if (schema == 3) {
                AppendFixture(payload, unit.physicalReceivedCount);
                AppendFixture(payload, unit.strategyReceivedCount);
            }
            AppendFixtureText(payload, unit.label);
        }
    }
    std::vector<unsigned char> bytes(kNativePayload, kNativePayload + strlen(kNativePayload));
    TestFooter footer = {};
    memcpy(footer.magic, "XASTAT2F", 8);
    memcpy(footer.end, "XASTATEN", 8);
    footer.schema = schema;
    footer.footerBytes = sizeof(footer);
    footer.nativeBytes = bytes.size();
    footer.payloadBytes = payload.size();
    std::wstring hash, error;
    if (!XiaoAnInstall::Sha256Bytes(bytes, hash, error)) return false;
    for (size_t i = 0; i < 64; ++i) footer.nativeSha256[i] = static_cast<char>(hash[i]);
    if (!XiaoAnInstall::Sha256Bytes(payload, hash, error)) return false;
    for (size_t i = 0; i < 64; ++i) footer.payloadSha256[i] = static_cast<char>(hash[i]);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    AppendFixture(bytes, footer);
    return WriteBytes(path, bytes);
}

void ValidateLegacyEmbedded(const std::wstring& folder) {
    BattleStatsPersistedState expected = SampleState();
    expected.knownMetrics = 0x3FF;
    expected.units[0].physicalReceivedCount = 19;
    expected.units[0].strategyReceivedCount = 20;
    for (uint32_t schema : {2u, 3u}) {
        const std::wstring save = folder + L"\\schema" + std::to_wstring(schema) + L".E5S";
        BattleStatsPersistedState loaded;
        std::wstring error;
        Check(WriteLegacyFixture(save, schema, expected) &&
            ReadBattleStatsEmbedded(save, loaded, error) == BattleStatsReadResult::Loaded &&
            loaded.schemaVersion == schema && loaded.historyIncomplete && loaded.rosterHistoryIncomplete &&
            loaded.campaignRoster.empty() && loaded.currentBattleParticipants.empty() &&
            loaded.units.size() == expected.units.size() && loaded.campaignUnits.size() == expected.campaignUnits.size() &&
            loaded.units[0].damageDealt == expected.units[0].damageDealt &&
            loaded.units[0].physicalReceivedCount == (schema == 2 ? 0 : 19) &&
            loaded.units[0].strategyReceivedCount == (schema == 2 ? 0 : 20),
            schema == 2 ? "read an authentic schema-2 fixture and seed new counters at zero" :
                "read an authentic schema-3 fixture and retain received counts without inventing a roster");
    }
}

void ValidateLegacy(const std::wstring& folder) {
    const std::wstring save = folder + L"\\legacy.E5S";
    const std::wstring sidecar = folder + L"\\legacy.stats";
    BattleStatsPersistedState original = SampleState();
    original.units.resize(1);
    std::wstring error;
    BattleStatsPersistedState loaded;
    Check(WriteSample(save, "legacy-save") && SaveBattleStatsSidecar(save, sidecar, original, error) &&
        LoadBattleStatsSidecar(save, sidecar, loaded, error) && loaded.schemaVersion == 1 &&
        loaded.historyIncomplete && loaded.campaignUnits.empty() && loaded.units.size() == 1 &&
        loaded.units[0].personId == -1 && loaded.units[0].slot == original.units[0].slot &&
        loaded.units[0].damageDealt == original.units[0].damageDealt &&
        loaded.elapsedMilliseconds == original.elapsedMilliseconds,
        "read unchanged schema-1 sidecars explicitly as legacy data requiring identity migration");
    Check(WriteSample(save, "another-save") && !LoadBattleStatsSidecar(save, sidecar, loaded, error),
        "reject legacy statistics paired with another native save");
    Check(WriteSample(sidecar, "truncated") && !LoadBattleStatsSidecar(save, sidecar, loaded, error) &&
        loaded.units.empty() && loaded.campaignUnits.empty(), "reject truncated legacy data without partial state");
}

void ValidateEmbedded(const std::wstring& folder) {
    const std::wstring save = folder + L"\\roundtrip.E5S", changed = folder + L"\\changed.E5S";
    const std::wstring copied = folder + L"\\copied.E5S", renamed = folder + L"\\renamed-slot.E5S";
    std::wstring error;
    BattleStatsPersistedState loaded, expected = SampleState();
    Check(RunChild(L"--child-write", save) && RunChild(L"--child-read", save),
        "save and restore all fields in separate writer and reader processes");
    Check(ReadBattleStatsEmbedded(save, loaded, error) == BattleStatsReadResult::Loaded && SameState(expected, loaded),
        "restore both person-ID groups, metric bits, full counters, identities and low-uptime-independent elapsed time");
    const auto valid = ReadBytes(save);
    Check(valid.size() > sizeof(kNativePayload) && memcmp(valid.data(), kNativePayload, strlen(kNativePayload)) == 0,
        "preserve every byte of the native E5S payload prefix");
    Check(CopyFileW(save.c_str(), copied.c_str(), TRUE) && MoveFileW(copied.c_str(), renamed.c_str()) &&
        RunChild(L"--child-read", renamed), "restore copied and renamed E5S files without installation or sidecar dependencies");
    expected.units[0].damageDealt = 37;
    Check(WriteBattleStatsEmbedded(save, expected, error) && ReadBytes(save).size() == valid.size() &&
        ReadBattleStatsEmbedded(save, loaded, error) == BattleStatsReadResult::Loaded && SameState(expected, loaded),
        "replace a valid embedded block without accumulating stale trailers");
    expected.status.clear(); expected.units.clear(); expected.currentBattleParticipants.clear();
    Check(WriteBattleStatsEmbedded(save, expected, error) && ReadBytes(save).size() < valid.size() &&
        ReadBattleStatsEmbedded(save, loaded, error) == BattleStatsReadResult::Loaded &&
        loaded.units.empty() && loaded.campaignUnits.size() == 3,
        "shorten the current-battle block while retaining campaign history");
    auto damaged = valid;
    damaged[0] ^= 1;
    Check(WriteBytes(changed, damaged) && ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Corrupt &&
        loaded.units.empty() && loaded.campaignUnits.empty(), "reject changed native payloads without partial statistics");
    damaged = valid;
    damaged[strlen(kNativePayload) + sizeof(TestHeader)] ^= 1;
    Check(WriteBytes(changed, damaged) && ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Corrupt,
        "verify statistics payload integrity separately from the native save hash");
    const auto beforeRejectedWrite = ReadBytes(changed);
    Check(!WriteBattleStatsEmbedded(changed, SampleState(), error) && ReadBytes(changed) == beforeRejectedWrite,
        "refuse to overwrite a suspicious damaged extension");
    damaged = valid; damaged.resize(damaged.size() - 17);
    Check(WriteBytes(changed, damaged) && ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Corrupt,
        "recognize a truncated footer instead of treating it as a legacy save");
    damaged.resize(strlen(kNativePayload) + sizeof(TestHeader) + 3);
    Check(WriteBytes(changed, damaged) && ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Corrupt,
        "recognize a truncated payload after the entire footer is lost");
    damaged = valid;
    TestFooter footer = {};
    memcpy(&footer, damaged.data() + damaged.size() - sizeof(footer), sizeof(footer));
    footer.schema = 99;
    memcpy(damaged.data() + damaged.size() - sizeof(footer), &footer, sizeof(footer));
    Check(WriteBytes(changed, damaged) && ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Unsupported &&
        !WriteBattleStatsEmbedded(changed, SampleState(), error) && ReadBytes(changed) == damaged,
        "preserve unsupported future blocks instead of replacing their statistics");
    damaged = valid;
    TestHeader header = {};
    memcpy(&header, damaged.data() + strlen(kNativePayload), sizeof(header));
    header.campaignUnitCount = 65536;
    memcpy(damaged.data() + strlen(kNativePayload), &header, sizeof(header));
    Check(RefreshPayloadHash(damaged) && WriteBytes(changed, damaged) &&
        ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Corrupt,
        "reject excessive declared counts before reserving person data");
    damaged = valid;
    const auto sample = SampleState();
    const size_t firstRow = strlen(kNativePayload) + sizeof(TestHeader) +
        (sample.scenario.size() + sample.status.size()) * sizeof(wchar_t);
    // A schema-3 row has the persisted person ID, totals header and received
    // counters. Recompute the hash so this exercises structural
    // duplicate validation, rather than stopping at integrity verification.
    constexpr size_t rowBytes = 88;
    const size_t secondRow = firstRow + rowBytes + sample.units[0].label.size() * sizeof(wchar_t);
    memcpy(damaged.data() + secondRow, damaged.data() + firstRow, sizeof(int32_t));
    Check(RefreshPayloadHash(damaged) && WriteBytes(changed, damaged) &&
        ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Corrupt && loaded.units.empty(),
        "reject duplicate person IDs in a fully checksummed current-battle payload");
    damaged = valid;
    const size_t firstCampaignRow = secondRow + rowBytes + sample.units[1].label.size() * sizeof(wchar_t);
    const size_t secondCampaignRow = firstCampaignRow + rowBytes + sample.campaignUnits[0].label.size() * sizeof(wchar_t);
    memcpy(damaged.data() + secondCampaignRow, damaged.data() + firstCampaignRow, sizeof(int32_t));
    Check(RefreshPayloadHash(damaged) && WriteBytes(changed, damaged) &&
        ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Corrupt && loaded.campaignUnits.empty(),
        "reject duplicate person IDs in a fully checksummed campaign payload");
    damaged = valid;
    memcpy(&footer, damaged.data() + damaged.size() - sizeof(footer), sizeof(footer));
    footer.payloadBytes = (std::numeric_limits<uint64_t>::max)();
    memcpy(damaged.data() + damaged.size() - sizeof(footer), &footer, sizeof(footer));
    Check(WriteBytes(changed, damaged) && ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Corrupt,
        "reject overflowing footer lengths before allocation or addition");
    Check(WriteSample(changed, kNativePayload) &&
        ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Missing && error.empty(),
        "distinguish legitimate legacy E5S files from damaged extensions");
    Check(ReadBattleStatsEmbedded(folder + L"\\missing.E5S", loaded, error) == BattleStatsReadResult::Corrupt && !error.empty(),
        "report inaccessible native saves instead of silently displaying zero statistics");
    Check(WriteSample(changed, kNativePayload) && WriteBattleStatsEmbedded(changed, BattleStatsPersistedState(), error) &&
        ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Loaded &&
        SameState(BattleStatsPersistedState(), loaded), "round-trip a valid empty new-campaign statistics block");
    expected = SampleState(); expected.campaignUnits.clear(); expected.campaignRoster.clear();
    for (int id = 0; id < 1025; ++id) {
        expected.campaignUnits.push_back(SampleUnit(id, -1));
        BattleRosterEntry entry;
        entry.personId = id;
        entry.label = expected.campaignUnits.back().label;
        entry.joined = entry.tracking = true;
        entry.lastCamp = 0;
        entry.source = BattleRosterSource::ExistingCampaignLedger;
        expected.campaignRoster.push_back(entry);
    }
    Check(WriteSample(changed, kNativePayload) && WriteBattleStatsEmbedded(changed, expected, error) &&
        ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Loaded && SameState(expected, loaded),
        "persist over 250 and over 1024 global identities independently of battlefield slots");
    expected.active = false; expected.units.clear(); expected.currentBattleParticipants.clear();
    expected.scenario.clear(); expected.scenarioOrdinal = -1;
    Check(WriteBattleStatsEmbedded(changed, expected, error) &&
        ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Loaded && SameState(expected, loaded),
        "persist global totals in non-battle saves without a current S battlefield");
    const auto unchanged = ReadBytes(changed);
    auto invalid = SampleState(); invalid.units.push_back(invalid.units[0]);
    Check(!WriteBattleStatsEmbedded(changed, invalid, error) && ReadBytes(changed) == unchanged,
        "reject duplicate current-battle person IDs without touching the save");
    invalid = SampleState(); invalid.campaignUnits.push_back(invalid.campaignUnits[0]);
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject duplicate campaign person IDs separately");
    invalid = SampleState(); invalid.units[0].personId = 65535;
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject the engine empty-person sentinel");
    invalid = SampleState(); invalid.units[0].damageReceived = -1;
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject negative counters");
    invalid = SampleState(); invalid.units[0].label.assign(257, L'x');
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject overlong labels");
    invalid = SampleState(); invalid.status.assign(4097, L'x');
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject overlong status text");
    invalid = SampleState(); invalid.knownMetrics = 0x400;
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject undefined schema-4 metric bits");
    invalid = SampleState(); invalid.campaignRoster.pop_back();
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject campaign totals without a matching roster entry");
    invalid = SampleState(); invalid.campaignUnits.pop_back();
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject joined roster entries without their global zero or historical row");
    invalid = SampleState(); invalid.campaignRoster[0].tracking = true;
    invalid.campaignRoster[0].changedFaction = true;
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject a changed-faction roster that still permits tracking");
    invalid = SampleState(); invalid.campaignRoster[0].joinScenarioOrdinal = 3;
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject runtime 3B membership attributed to an S scene");
    invalid = SampleState(); invalid.campaignRoster[0].joinCommandOrdinal = 0xFFFFFFFFu;
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject runtime membership with no successful command source");
    invalid = SampleState(); invalid.currentBattleParticipants.push_back(7);
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject duplicate current participant identities");
    invalid = SampleState(); invalid.currentBattleParticipants.pop_back();
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject a current row outside the confirmed participant set");
    invalid = SampleState(); invalid.active = false;
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject R or inactive states retaining a current battlefield");
    invalid = SampleState(); invalid.campaignRoster[0].label += L"不同";
    Check(!WriteBattleStatsEmbedded(changed, invalid, error), "reject mismatched roster and campaign labels");
    size_t rosterOffset = strlen(kNativePayload) + sizeof(TestHeader) +
        (sample.scenario.size() + sample.status.size()) * sizeof(wchar_t);
    for (const auto* group : {&sample.units, &sample.campaignUnits})
        for (const auto& unit : *group) rosterOffset += 88 + unit.label.size() * sizeof(wchar_t);
    damaged = valid;
    TestRosterRow badRoster = {};
    memcpy(&badRoster, damaged.data() + rosterOffset + sizeof(TestRosterHeader), sizeof(badRoster));
    badRoster.source = 255;
    memcpy(damaged.data() + rosterOffset + sizeof(TestRosterHeader), &badRoster, sizeof(badRoster));
    Check(RefreshPayloadHash(damaged) && WriteBytes(changed, damaged) &&
        ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Corrupt && loaded.campaignRoster.empty(),
        "reject a checksummed invalid roster source without publishing partial state");
    damaged = valid;
    memcpy(&badRoster, damaged.data() + rosterOffset + sizeof(TestRosterHeader), sizeof(badRoster));
    badRoster.personId = 1234;
    memcpy(damaged.data() + rosterOffset + sizeof(TestRosterHeader), &badRoster, sizeof(badRoster));
    Check(RefreshPayloadHash(damaged) && WriteBytes(changed, damaged) &&
        ReadBattleStatsEmbedded(changed, loaded, error) == BattleStatsReadResult::Corrupt,
        "reject a checksummed roster-to-ledger identity mismatch");
    Check(WriteBytes(changed, unchanged), "restore the valid fixture before atomic replacement failure tests");
    invalid = SampleState(); invalid.units.assign(251, SampleUnit(0, 0));
    Check(!WriteBattleStatsEmbedded(changed, invalid, error) && ReadBytes(changed) == unchanged,
        "limit current battles to 250 people while allowing larger global histories");
    HANDLE locked = CreateFileW(changed.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE && !WriteBattleStatsEmbedded(changed, expected, error) &&
        ReadBytes(changed) == unchanged, "preserve complete old files when atomic replacement is locked");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && (!wcscmp(argv[1], L"--child-write") || !wcscmp(argv[1], L"--child-read"))) {
        if (!ChildPathIsIsolated(argv[2])) return 10;
        std::wstring error;
        if (!wcscmp(argv[1], L"--child-write"))
            return WriteSample(argv[2], kNativePayload) && WriteBattleStatsEmbedded(argv[2], SampleState(), error) ? 0 : 11;
        BattleStatsPersistedState loaded;
        return ReadBattleStatsEmbedded(argv[2], loaded, error) == BattleStatsReadResult::Loaded &&
            SameState(SampleState(), loaded) ? 0 : 12;
    }
    if (argc != 1) return 2;
    wchar_t temporary[32768] = {};
    if (!GetTempPathW(_countof(temporary), temporary)) return 3;
    const std::wstring folder = std::wstring(temporary) + L"CczBattleStats-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(folder.c_str(), nullptr)) return 4;
    ValidateLegacy(folder);
    ValidateLegacyEmbedded(folder);
    ValidateEmbedded(folder);
    std::error_code cleanupError;
    std::filesystem::remove_all(folder, cleanupError);
    Check(!cleanupError && !std::filesystem::exists(folder), "clean up only the unique temporary test directory");
    printf("BattleStatsPersistenceTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
