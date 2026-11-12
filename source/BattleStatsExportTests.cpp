#include "BattleStats.h"
#include "EngineProfile.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

bool GetObservedGuideScenario(std::wstring& name) {
    name.clear();
    return false;
}

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

std::wstring ReadUtf16(const std::filesystem::path& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb")) return {};
    if (_fseeki64(file, 0, SEEK_END)) { fclose(file); return {}; }
    const __int64 length = _ftelli64(file);
    if (length < 2 || length > 1024 * 1024 || (length & 1) || _fseeki64(file, 0, SEEK_SET)) {
        fclose(file);
        return {};
    }
    std::wstring value(static_cast<size_t>(length / 2), L'\0');
    const bool okay = fread(value.data(), 1, static_cast<size_t>(length), file) == static_cast<size_t>(length);
    fclose(file);
    if (!okay || value[0] != 0xFEFF) return {};
    value.erase(value.begin());
    return value;
}

std::vector<std::vector<std::wstring>> ParseCsv(const std::wstring& text) {
    std::vector<std::vector<std::wstring>> rows;
    std::vector<std::wstring> row;
    std::wstring field;
    bool quoted = false;
    for (size_t index = 0; index < text.size(); ++index) {
        const wchar_t character = text[index];
        if (character == L'"') {
            if (quoted && index + 1 < text.size() && text[index + 1] == L'"') {
                field += character;
                ++index;
            } else quoted = !quoted;
        } else if (!quoted && character == L',') {
            row.push_back(field);
            field.clear();
        } else if (!quoted && character == L'\r' && index + 1 < text.size() && text[index + 1] == L'\n') {
            row.push_back(field);
            rows.push_back(row);
            row.clear();
            field.clear();
            ++index;
        } else field += character;
    }
    if (quoted || !field.empty() || !row.empty()) return {};
    return rows;
}

struct Records {
    std::vector<std::filesystem::path> text, csv;
};

Records FindRecords(const std::wstring& root) {
    Records result;
    const std::filesystem::path folder = PatchPrivatePath(root, CompiledEngineProfile()) + L"\\records";
    if (!std::filesystem::exists(folder)) return result;
    for (const auto& item : std::filesystem::directory_iterator(folder)) {
        if (item.path().extension() == L".txt") result.text.push_back(item.path());
        if (item.path().extension() == L".csv") result.csv.push_back(item.path());
    }
    return result;
}

BattleStatsPersistedState SampleState() {
    BattleStatsPersistedState state;
    state.active = true;
    state.scenarioOrdinal = 1;
    state.scenario = L"S_00,\"战场\".eex";
    state.campaignId = 77;
    state.battleInstance = 88;
    state.knownMetrics = 0x3FF;
    state.elapsedMilliseconds = 12000;
    BattleUnitTotals unit;
    unit.personId = 7;
    unit.slot = 2;
    unit.label = L"张,\"辽\"\r\n将军";
    unit.damageDealt = 101;
    unit.damageReceived = 102;
    unit.recoveryDone = 103;
    unit.criticalCount = 104;
    unit.comboExtraHitCount = 105;
    unit.attackBlockCount = 106;
    unit.spellBlockCount = 107;
    unit.defeatedEnemies = 108;
    unit.physicalReceivedCount = (std::numeric_limits<long long>::max)();
    unit.strategyReceivedCount = 0;
    state.units.push_back(unit);
    unit.physicalReceivedCount = 42;
    unit.strategyReceivedCount = 43;
    state.campaignUnits.push_back(unit);
    BattleRosterEntry roster;
    roster.personId = unit.personId;
    roster.label = unit.label;
    roster.joined = roster.tracking = true;
    roster.lastCamp = 0;
    roster.source = BattleRosterSource::ExistingCampaignLedger;
    state.campaignRoster.push_back(roster);
    state.currentBattleParticipants.push_back(unit.personId);
    return state;
}

void BeginCase(const std::wstring& root, bool available) {
    std::filesystem::create_directories(root);
    InitializeBattleStats(root.c_str());
    if (available) InitializeBattleStatsModelForTest(0, 0, 0);
}

void VerifyAvailableExport(const std::wstring& root) {
    BeginCase(root, true);
    Check(RestoreBattleStatsState(SampleState(), 1000), "restore both ledgers before production battle export");
    NotifyBattleStatsScenarioInstalled(2, false, false, 1000);
    const auto records = FindRecords(root);
    Check(records.text.size() == 1 && records.csv.size() == 1 &&
        records.text[0].stem() == records.csv[0].stem(), "leaving a battlefield creates one TXT and CSV pair with the same identity");
    if (records.text.size() != 1 || records.csv.size() != 1) return;
    const auto csv = ParseCsv(ReadUtf16(records.csv[0]));
    Check(csv.size() == 2 && csv[0].size() == 16 && csv[1].size() == 16,
        "Excel-readable CSV has one header and one current-battle row with all ten metrics");
    if (csv.size() == 2 && csv[0].size() == 16 && csv[1].size() == 16) {
        Check(csv[1][0] == SampleState().scenario && csv[1][4] == SampleState().units[0].label &&
            csv[1][1] == L"77" && csv[1][2] == L"88" && csv[1][3] == L"7",
            "CSV quoting preserves Chinese labels, commas, quotes, newlines and battle/person identities");
        Check(csv[0][10] == L"攻击闪避" && csv[0][11] == L"策略闪避" &&
            csv[1][5] == L"101" && csv[1][6] == L"102" && csv[1][7] == L"103" &&
            csv[1][8] == L"104" && csv[1][9] == L"105" && csv[1][10] == L"106" &&
            csv[1][11] == L"107" && csv[1][12] == L"108" &&
            csv[0][13] == L"物理受击" && csv[1][13] == L"9223372036854775807" &&
            csv[0][14] == L"策略受击" && csv[1][14] == L"0" && csv[1][15] == L"否",
            "export preserves existing dodge semantics, full 64-bit received counts and a genuine known zero");
    }
    const auto text = ReadUtf16(records.text[0]);
    Check(text.find(L"物理受击：9223372036854775807") != std::wstring::npos &&
        text.find(L"策略受击：0") != std::wstring::npos && text.find(L"攻击闪避：106") != std::wstring::npos &&
        text.find(L"历史不完整：否") != std::wstring::npos,
        "TXT battle record carries the same received, dodge and completeness values");
    NotifyBattleStatsScenarioInstalled(2, false, false, 1001);
    const auto repeated = FindRecords(root);
    Check(repeated.text.size() == 1 && repeated.csv.size() == 1, "repeated scene notification does not export the battle twice");
}

void VerifyUnavailableExport(const std::wstring& root) {
    BeginCase(root, false);
    Check(RestoreBattleStatsState(SampleState(), 1000), "restore historical counters with unsupported current collection layout");
    NotifyBattleStatsScenarioInstalled(2, false, false, 1000);
    const auto records = FindRecords(root);
    const auto rows = records.csv.size() == 1 ? ParseCsv(ReadUtf16(records.csv[0])) :
        std::vector<std::vector<std::wstring>>();
    Check(rows.size() == 2 && rows[1].size() == 16 && rows[1][10] == L"106" &&
        rows[1][13] == L"--" && rows[1][14] == L"--",
        "unsupported received layouts export unavailable markers independently of retained dodge history");
    Check(records.text.size() == 1 && ReadUtf16(records.text[0]).find(L"物理受击：--") != std::wstring::npos &&
        ReadUtf16(records.text[0]).find(L"策略受击：--") != std::wstring::npos,
        "TXT distinguishes unavailable received metrics from real zero counts");
}

void VerifyMigrationAndLoading(const std::wstring& root) {
    BeginCase(root, true);
    auto state = SampleState();
    state.schemaVersion = 2;
    state.campaignRoster.clear();
    state.currentBattleParticipants.clear();
    state.knownMetrics = 0xFF;
    Check(RestoreBattleStatsState(state, 1000), "restore schema 2 for production migration export");
    SetBattleStatsLoading(true);
    NotifyBattleStatsScenarioInstalled(2, false, false, 1000);
    Check(FindRecords(root).csv.empty() && FindRecords(root).text.empty(), "loading transaction suppresses stale battle export");
    SetBattleStatsLoading(false);
    NotifyBattleStatsScenarioInstalled(2, false, false, GetTickCount64());
    const auto records = FindRecords(root);
    const auto rows = records.csv.size() == 1 ? ParseCsv(ReadUtf16(records.csv[0])) :
        std::vector<std::vector<std::wstring>>();
    Check(rows.size() == 2 && rows[1].size() == 16 && rows[1][10] == L"106" &&
        rows[1][11] == L"107" && rows[1][13] == L"0" && rows[1][14] == L"0" && rows[1][15] == L"是",
        "old histories retain dodge values, seed received at zero and export incomplete history");
    BattleStatsPersistedState after;
    CaptureBattleStatsState(after, GetTickCount64());
    Check(after.units.empty() && after.currentBattleParticipants.empty() &&
        after.campaignUnits.size() == 1 && after.campaignUnits[0].physicalReceivedCount == 0 &&
        after.campaignUnits[0].strategyReceivedCount == 0,
        "R scene export clears current participants while preserving migrated campaign totals");
}
} // namespace

int wmain() {
    wchar_t temporary[MAX_PATH] = {}, seed[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, temporary) || !GetTempFileNameW(temporary, L"xar", 0, seed)) return 2;
    DeleteFileW(seed);
    const std::filesystem::path root(seed);
    std::filesystem::create_directory(root);
    VerifyAvailableExport((root / L"available").wstring());
    VerifyUnavailableExport((root / L"unavailable").wstring());
    VerifyMigrationAndLoading((root / L"migration").wstring());
    ShutdownBattleStats();
    std::filesystem::remove_all(root);
    std::printf("BattleStatsExportTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
