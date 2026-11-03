#include "StatsDiagnostics.h"
#include "EngineProfile.h"
#include "InstallCommon.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <thread>

namespace {
thread_local bool failNextAllocation = false;
}

void* operator new(size_t size) {
    if (failNextAllocation) {
        failNextAllocation = false;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }

namespace {
int failures = 0;
void Check(bool okay, const char* message) {
    printf("%s %s\n", okay ? "PASS" : "FAIL", message);
    if (!okay) ++failures;
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string Utf8(const std::wstring& input) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, &result[0], length, nullptr, nullptr);
    result.pop_back();
    return result;
}
std::string LoggedPath(const std::wstring& path) {
    std::string result;
    for (const char value : Utf8(path)) {
        if (value == '\\') result += '\\';
        result += value;
    }
    return result;
}
std::string FieldInStage(const std::string& log, const std::string& stage, const std::string& field) {
    const size_t line = log.find("stage=\"" + stage + "\"");
    if (line == std::string::npos) return {};
    const size_t value = log.find(field + "=", line);
    const size_t end = log.find("\r\n", line);
    if (value == std::string::npos || value >= end) return {};
    const size_t start = value + field.size() + 1;
    const size_t stop = log.find_first_of(" \r\n", start);
    return log.substr(start, stop - start);
}
} // namespace

int wmain() {
    wchar_t temporary[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temporary);
    const std::filesystem::path root = std::filesystem::path(temporary) /
        (L"XiaoAnStatsDiagnostics-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(root);
    const auto privateRoot = root / PatchPrivateRelative(CompiledEngineProfile());
    const auto diagnostics = privateRoot / L"diagnostics";
    const auto log = diagnostics / L"stats.log";
    InitializeStatsDiagnostics(root.c_str());
    SetStatsPersistenceCapability(true, false, L"原生存档内嵌统计");
    BattleUnitTotals first;
    first.personId = 7;
    first.slot = 2;
    first.damageDealt = 123;
    first.damageReceived = 9;
    first.recoveryDone = 44;
    first.criticalCount = 2;
    first.comboExtraHitCount = 1;
    first.attackBlockCount = 3;
    first.spellBlockCount = 4;
    first.defeatedEnemies = 5;
    first.physicalReceivedCount = 11;
    first.strategyReceivedCount = 13;
    BattleUnitTotals second = first;
    second.personId = 3;
    BattleStatsPersistedState state;
    state.campaignId = 91;
    state.battleInstance = 4;
    state.scenarioOrdinal = 8;
    state.elapsedMilliseconds = 98765;
    state.knownMetrics = 1023;
    state.units = {first, second};
    state.campaignUnits = {first};
    const std::wstring save = (root / L"存档.E5S").wstring();
    const uint64_t operation = BeginStatsOperation(L"load", save);
    RecordStatsDiagnostic(operation, L"load-verified", save, &state, L"一行\r\n下一行\t结束");
    std::reverse(state.units.begin(), state.units.end());
    RecordStatsDiagnostic(0, L"model-restored", L"", &state);
    RecordStatsDiagnostic(0, L"reset_after", L"", &state, L"开始新战场");
    SetStatsLastSave(true, save, L"已验证");
    SetStatsLastLoad(false, save, L"统计损坏，保留文件");
    const auto info = ReadStatsPersistenceInfo();
    Check(info.available && !info.nativeExtension && info.capability == L"原生存档内嵌统计" &&
        info.lastSave.find(L"成功") != std::wstring::npos &&
        info.lastLoad.find(L"统计损坏") != std::wstring::npos && info.lastReset == L"开始新战场",
        "UI state distinguishes persistence capability, save success, load failure, and reset reason");
    const wchar_t* allocationDetail = L"Diagnostic literal conversion must remain inside the noexcept boundary even when an allocation fails during a game save or load commit.";
    const auto allocationSafe = [](auto action) {
        failNextAllocation = true;
        bool escaped = false;
        try { action(); } catch (...) { escaped = true; }
        const bool consumed = !failNextAllocation;
        failNextAllocation = false;
        return consumed && !escaped;
    };
    const bool capabilitySafe = allocationSafe([&] {
        SetStatsPersistenceCapability(false, false, allocationDetail);
    });
    const bool recordSafe = allocationSafe([&] {
        RecordStatsDiagnostic(operation, L"allocation-failure", save, &state, allocationDetail);
    });
    const bool saveSafe = allocationSafe([&] { SetStatsLastSave(true, save, allocationDetail); });
    const bool loadSafe = allocationSafe([&] { SetStatsLastLoad(true, save, allocationDetail); });
    Check(capabilitySafe && recordSafe && saveSafe && loadSafe &&
        ReadStatsPersistenceInfo().lastSave == info.lastSave &&
        ReadStatsPersistenceInfo().lastLoad == info.lastLoad,
        "literal detail allocation failures stay inside all diagnostic noexcept boundaries and cannot reverse a committed save");
    std::string content = Read(log);
    Check(content.find("build=" + Utf8(XiaoAnInstall::kPackageVersion)) != std::string::npos &&
        content.find(LoggedPath(save)) != std::string::npos &&
        content.find(Utf8(L"一行\\r\\n下一行\\t结束")) != std::string::npos,
        "UTF-8 log records build, actual Unicode save path, and escaped single-line diagnostics");
    const size_t capture = content.find("schema=3");
    const size_t restored = content.find("schema=3", capture + 1);
    const size_t endCapture = content.find(" detail=", capture);
    const size_t endRestored = content.find("\r\n", restored);
    Check(capture != std::string::npos && restored != std::string::npos &&
        content.substr(capture, endCapture - capture) == content.substr(restored, endRestored - restored) &&
        content.find("campaign.count=1") != std::string::npos &&
        content.find("current.count=2") != std::string::npos &&
        content.find("7/2:123,9,44,2,1,3,4,5,11,13") != std::string::npos &&
        content.find("elapsedMs=98765") != std::string::npos,
        "capture and restore produce identical two-ledger summaries despite vector order");
    Check(content.find("op=" + std::to_string(operation) + " build=" + Utf8(XiaoAnInstall::kPackageVersion) +
            " engine=" + Utf8(EngineProfileName(CompiledEngineProfile())) +
            " stage=\"model-restored\" path=\"" + LoggedPath(save)) != std::string::npos &&
        content.find("op=0 build=" + Utf8(XiaoAnInstall::kPackageVersion) + " engine=" +
            Utf8(EngineProfileName(CompiledEngineProfile())) + " stage=\"reset_after\"") != std::string::npos,
        "model restore inherits operation identity and path while unrelated reset stays independent");

    std::thread worker([&save] {
        const auto id = BeginStatsOperation(L"save-worker", save);
        for (int i = 0; i < 40; ++i) RecordStatsDiagnostic(id, L"worker", save);
    });
    for (int i = 0; i < 40; ++i) RecordStatsDiagnostic(operation, L"main", save);
    worker.join();
    content = Read(log);
    Check(static_cast<size_t>(std::count(content.begin(), content.end(), '\n')) == 87,
        "concurrent diagnostics append complete individual records");

    state.units.assign(65535, first);
    for (size_t i = 0; i < state.units.size(); ++i) state.units[i].personId = static_cast<int>(i);
    state.units[0].damageDealt = (std::numeric_limits<long long>::max)();
    state.units[1].damageDealt = (std::numeric_limits<long long>::max)();
    RecordStatsDiagnostic(operation, L"large-first", save, &state);
    state.units.front().slot += 100;
    state.units.front().label = L"读取后角色名称";
    RecordStatsDiagnostic(operation, L"large-slot", save, &state);
    ++state.units.back().physicalReceivedCount;
    RecordStatsDiagnostic(operation, L"large-changed", save, &state);
    content = Read(log);
    const std::string originalDigest = FieldInStage(content, "large-first", "current.digest");
    Check(!originalDigest.empty() &&
        originalDigest == FieldInStage(content, "large-slot", "current.digest") &&
        originalDigest != FieldInStage(content, "large-changed", "current.digest") &&
        FieldInStage(content, "large-first", "current.count") == "65535" &&
        FieldInStage(content, "large-first", "current.totals").find("9223372036854775807,") == 1 &&
        content.size() < 64 * 1024 && content.find("65534/2:") == std::string::npos,
        "maximum campaign ledger has bounded samples, saturated totals, and a digest including physical received counts independent of slots and labels");

    ++state.units.back().strategyReceivedCount;
    RecordStatsDiagnostic(operation, L"strategy-received-changed", save, &state);
    content = Read(log);
    Check(FieldInStage(content, "strategy-received-changed", "current.digest") !=
        FieldInStage(content, "large-changed", "current.digest") &&
        FieldInStage(content, "strategy-received-changed", "current.totals") !=
        FieldInStage(content, "large-changed", "current.totals"),
        "strategy received counts affect full-ledger digest and saturated diagnostic totals");

    const auto nested = BeginStatsOperation(L"load-nested", L"inner.E5S");
    RecordStatsDiagnostic(operation, L"load-outer-resumed", save);
    RecordStatsDiagnostic(0, L"scene-ready", L"");
    content = Read(log);
    Check(nested != operation && content.find("op=" + std::to_string(operation) + " build=" +
        Utf8(XiaoAnInstall::kPackageVersion) + " engine=" + Utf8(EngineProfileName(CompiledEngineProfile())) +
        " stage=\"scene-ready\" path=\"" + LoggedPath(save)) != std::string::npos,
        "resuming an explicit outer load restores operation and actual path for later model and scene events");
    {
        std::ofstream full(log, std::ios::binary | std::ios::trunc);
        const std::string chunk(1024, 'x');
        for (int i = 0; i < 2048; ++i) full.write(chunk.data(), chunk.size());
    }
    RecordStatsDiagnostic(operation, L"rotation", save);
    const auto previous = std::filesystem::path(log.wstring() + L".1");
    Check(std::filesystem::file_size(previous) == 2 * 1024 * 1024 &&
        std::filesystem::file_size(log) < 4096 && Read(log).find("stage=\"rotation\"") != std::string::npos,
        "bounded rotation retains one previous log and starts a readable current log");

    // A regular file cannot contain a diagnostics directory. Disk failure must not
    // prevent model status updates or unwind into a game save/load callback.
    const auto blocked = root / L"not-a-directory";
    { std::ofstream file(blocked); file << "blocked"; }
    InitializeStatsDiagnostics(blocked.c_str());
    SetStatsPersistenceCapability(false, false, L"无法安装保存钩子");
    RecordStatsDiagnostic(0, L"unwritable", save, &state);
    SetStatsLastLoad(false, save, L"没有统计数据");
    Check(!ReadStatsPersistenceInfo().available &&
        ReadStatsPersistenceInfo().lastLoad.find(L"没有统计数据") != std::wstring::npos,
        "unwritable diagnostics are non-fatal and preserve visible failure status");
    InitializeStatsDiagnostics(nullptr);
    std::filesystem::remove(blocked);
    std::filesystem::remove(log);
    std::filesystem::remove(previous);
    std::filesystem::remove(diagnostics);
    std::filesystem::remove(privateRoot);
    std::filesystem::remove(root / L"patch");
    std::filesystem::remove(root);
    printf("StatsDiagnosticsTests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
