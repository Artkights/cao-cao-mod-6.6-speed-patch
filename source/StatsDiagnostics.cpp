#include "StatsDiagnostics.h"

#include "InstallCommon.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <sstream>
#include <tuple>

namespace {
SRWLOCK gLock = SRWLOCK_INIT;
std::wstring gLogPath;
StatsPersistenceInfo gInfo;
uint64_t gNextOperation = 0;
thread_local uint64_t gThreadOperation = 0;
thread_local std::wstring gThreadPath;
constexpr uint64_t kMaximumLogBytes = 2 * 1024 * 1024;

struct Lock {
    Lock() { AcquireSRWLockExclusive(&gLock); }
    ~Lock() { ReleaseSRWLockExclusive(&gLock); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

std::wstring Join(const std::wstring& directory, const std::wstring& leaf) {
    return directory.empty() ? leaf : directory +
        (directory.back() == L'\\' || directory.back() == L'/' ? L"" : L"\\") + leaf;
}

std::wstring Escape(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (const wchar_t character : text) {
        switch (character) {
        case L'\\': result += L"\\\\"; break;
        case L'\r': result += L"\\r"; break;
        case L'\n': result += L"\\n"; break;
        case L'\t': result += L"\\t"; break;
        case L'"': result += L"\\\""; break;
        default: result += character < L' ' ? L'?' : character; break;
        }
    }
    return result;
}

std::wstring Timestamp() {
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    wchar_t text[40] = {};
    swprintf_s(text, L"%04u-%02u-%02uT%02u:%02u:%02u.%03u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds);
    return text;
}

void Append(uint64_t operation, const wchar_t* stage, const std::wstring& path,
    const std::wstring& summary, const std::wstring& detail) {
    if (gLogPath.empty()) return;
    std::wstring line = Timestamp() + L" pid=" + std::to_wstring(GetCurrentProcessId()) +
        L" op=" + std::to_wstring(operation) + L" build=" + XiaoAnInstall::kPackageVersion +
        L" engine=" + EngineProfileName(CompiledEngineProfile()) + L" stage=\"" +
        Escape(stage ? stage : L"") + L"\" path=\"" + Escape(path) + L"\"";
    if (!summary.empty()) line += L" " + summary;
    if (!detail.empty()) line += L" detail=\"" + Escape(detail) + L"\"";
    line += L"\r\n";
    if (line.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return;
    const int inputLength = static_cast<int>(line.size());
    const int length = WideCharToMultiByte(CP_UTF8, 0, line.data(), inputLength,
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) return;
    std::string bytes(static_cast<size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, line.data(), inputLength,
        &bytes[0], length, nullptr, nullptr)) return;
    if (bytes.size() > kMaximumLogBytes) return;

    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExW(gLogPath.c_str(), GetFileExInfoStandard, &attributes)) {
        const uint64_t size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) |
            attributes.nFileSizeLow;
        if (size + static_cast<uint64_t>(bytes.size()) > kMaximumLogBytes) {
            // One previous log is retained; failure to rotate is non-fatal.
            if (!MoveFileExW(gLogPath.c_str(), (gLogPath + L".1").c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
        }
    }
    HANDLE file = CreateFileW(gLogPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

void WriteUnits(std::wostringstream& output, const wchar_t* name,
    const std::vector<BattleUnitTotals>& units) {
    std::vector<const BattleUnitTotals*> sorted;
    sorted.reserve(units.size());
    for (const auto& unit : units) sorted.push_back(&unit);
    std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
        return std::tie(left->personId, left->damageDealt, left->damageReceived,
            left->recoveryDone, left->criticalCount, left->comboExtraHitCount,
            left->attackBlockCount, left->spellBlockCount, left->defeatedEnemies,
            left->physicalReceivedCount, left->strategyReceivedCount, left->slot) <
            std::tie(right->personId, right->damageDealt, right->damageReceived,
                right->recoveryDone, right->criticalCount, right->comboExtraHitCount,
                right->attackBlockCount, right->spellBlockCount, right->defeatedEnemies,
                right->physicalReceivedCount, right->strategyReceivedCount, right->slot);
    });
    std::array<int64_t, 10> totals = {};
    uint64_t digest = UINT64_C(14695981039346656037);
    const auto hashNumber = [&digest](uint64_t value) {
        for (unsigned byte = 0; byte < 8; ++byte) {
            digest ^= value & 0xff;
            digest *= UINT64_C(1099511628211);
            value >>= 8;
        }
    };
    hashNumber(units.size());
    for (const auto* unit : sorted) {
        const std::array<int64_t, 10> values = {{unit->damageDealt, unit->damageReceived,
            unit->recoveryDone, unit->criticalCount, unit->comboExtraHitCount,
            unit->attackBlockCount, unit->spellBlockCount, unit->defeatedEnemies,
            unit->physicalReceivedCount, unit->strategyReceivedCount}};
        hashNumber(static_cast<uint64_t>(unit->personId));
        for (size_t metric = 0; metric < values.size(); ++metric) {
            hashNumber(static_cast<uint64_t>(values[metric]));
            const int64_t maximum = (std::numeric_limits<int64_t>::max)();
            const int64_t minimum = (std::numeric_limits<int64_t>::min)();
            if (values[metric] > 0 && totals[metric] > maximum - values[metric])
                totals[metric] = maximum;
            else if (values[metric] < 0 && totals[metric] < minimum - values[metric])
                totals[metric] = minimum;
            else totals[metric] += values[metric];
        }
    }
    output << L" " << name << L".count=" << units.size() << L" " << name << L".digest=" <<
        std::hex << std::setfill(L'0') << std::setw(16) << digest << std::dec <<
        L" " << name << L".totals=[";
    for (size_t metric = 0; metric < totals.size(); ++metric) {
        if (metric) output << L",";
        output << totals[metric];
    }
    output << L"] " << name << L".sample=[";
    const size_t sampleCount = std::min<size_t>(16, sorted.size());
    for (size_t index = 0; index < sampleCount; ++index) {
        const auto* unit = sorted[index];
        if (index) output << L";";
        // Fixed metric order makes capture/verification/restore directly comparable.
        output << unit->personId << L"/" << unit->slot << L":" <<
            unit->damageDealt << L"," << unit->damageReceived << L"," <<
            unit->recoveryDone << L"," << unit->criticalCount << L"," <<
            unit->comboExtraHitCount << L"," << unit->attackBlockCount << L"," <<
            unit->spellBlockCount << L"," << unit->defeatedEnemies << L"," <<
            unit->physicalReceivedCount << L"," << unit->strategyReceivedCount;
    }
    output << L"]";
}

std::wstring Summarize(const BattleStatsPersistedState& state) {
    std::wostringstream result;
    result << L"schema=" << state.schemaVersion << L" campaign=" << state.campaignId <<
        L" battle=" << state.battleInstance << L" scenario=" << state.scenarioOrdinal <<
        L" active=" << state.active << L" incomplete=" << state.historyIncomplete <<
        L" metrics=" << state.knownMetrics << L" elapsedMs=" << state.elapsedMilliseconds;
    WriteUnits(result, L"current", state.units);
    WriteUnits(result, L"campaign", state.campaignUnits);
    return result.str();
}

std::wstring ResultText(bool succeeded, const std::wstring& path, const std::wstring& detail) {
    std::wstring leaf = path;
    const size_t separator = leaf.find_last_of(L"\\/");
    if (separator != std::wstring::npos) leaf.erase(0, separator + 1);
    std::wstring result = succeeded ? L"成功" : L"失败";
    if (!leaf.empty()) result += L"（" + leaf + L"）";
    if (!detail.empty()) result += L"：" + detail;
    return result;
}
} // namespace

void InitializeStatsDiagnostics(const wchar_t* root) noexcept {
    try {
        Lock lock;
        gLogPath.clear();
        if (!root || !*root) return;
        const std::wstring patch = Join(root, L"patch");
        const std::wstring privateRoot = Join(root, PatchPrivateRelative(CompiledEngineProfile()));
        const std::wstring diagnostics = Join(privateRoot, L"diagnostics");
        CreateDirectoryW(patch.c_str(), nullptr);
        CreateDirectoryW(privateRoot.c_str(), nullptr);
        CreateDirectoryW(diagnostics.c_str(), nullptr);
        gLogPath = Join(diagnostics, L"stats.log");
        Append(0, L"diagnostics-initialized", root, L"", L"摘要指标顺序：伤害、承伤、回复、暴击、连击、攻击闪避、策略闪避、击退、物理受击、策略受击；digest=FNV64(排序后人物ID和十指标，不含槽位/名称)，sample最多16人");
    } catch (...) {}
}

void SetStatsPersistenceCapability(bool available, bool nativeExtension,
    const std::wstring& detail) noexcept {
    try {
        Lock lock;
        gInfo.available = available;
        gInfo.nativeExtension = nativeExtension;
        gInfo.capability = detail;
        Append(0, L"persistence-capability", L"",
            L"available=" + std::to_wstring(available) + L" nativeExtension=" +
            std::to_wstring(nativeExtension), detail);
    } catch (...) {}
}

void SetStatsPersistenceCapability(bool available, bool nativeExtension,
    const wchar_t* detail) noexcept {
    try {
        SetStatsPersistenceCapability(available, nativeExtension,
            std::wstring(detail ? detail : L""));
    } catch (...) {}
}

uint64_t BeginStatsOperation(const wchar_t* event, const std::wstring& path) noexcept {
    try {
        Lock lock;
        const uint64_t operation = ++gNextOperation;
        gThreadOperation = operation;
        gThreadPath = path;
        Append(operation, event, path, L"", L"");
        return operation;
    } catch (...) { return 0; }
}

void RecordStatsDiagnostic(uint64_t operation, const wchar_t* stage,
    const std::wstring& path, const BattleStatsPersistedState* state,
    const std::wstring& detail) noexcept {
    try {
        Lock lock;
        const std::wstring event = stage ? stage : L"";
        const bool standalone = event.find(L"reset") != std::wstring::npos ||
            event.find(L"clear") != std::wstring::npos ||
            event.find(L"new-game") != std::wstring::npos ||
            event.find(L"new-battle") != std::wstring::npos ||
            event.find(L"startup") != std::wstring::npos ||
            event.find(L"initializ") != std::wstring::npos;
        if (event.find(L"reset") != std::wstring::npos ||
            event.find(L"clear") != std::wstring::npos ||
            event.find(L"new-game") != std::wstring::npos ||
            event.find(L"new-battle") != std::wstring::npos) {
            gInfo.lastReset = detail.empty() ? event : detail;
        }
        if (operation && !standalone) {
            gThreadOperation = operation;
            if (!path.empty()) gThreadPath = path;
        }
        if (!operation && !standalone) operation = gThreadOperation;
        Append(operation, stage,
            path.empty() && operation && operation == gThreadOperation ? gThreadPath : path,
            state ? Summarize(*state) : L"", detail);
    } catch (...) {}
}

void RecordStatsDiagnostic(uint64_t operation, const wchar_t* stage,
    const std::wstring& path, const BattleStatsPersistedState* state,
    const wchar_t* detail) noexcept {
    try {
        RecordStatsDiagnostic(operation, stage, path, state,
            std::wstring(detail ? detail : L""));
    } catch (...) {}
}

void SetStatsLastSave(bool succeeded, const std::wstring& path,
    const std::wstring& detail) noexcept {
    try { Lock lock; gInfo.lastSave = ResultText(succeeded, path, detail); } catch (...) {}
}

void SetStatsLastSave(bool succeeded, const std::wstring& path,
    const wchar_t* detail) noexcept {
    try { SetStatsLastSave(succeeded, path, std::wstring(detail ? detail : L"")); }
    catch (...) {}
}

void SetStatsLastLoad(bool succeeded, const std::wstring& path,
    const std::wstring& detail) noexcept {
    try { Lock lock; gInfo.lastLoad = ResultText(succeeded, path, detail); } catch (...) {}
}

void SetStatsLastLoad(bool succeeded, const std::wstring& path,
    const wchar_t* detail) noexcept {
    try { SetStatsLastLoad(succeeded, path, std::wstring(detail ? detail : L"")); }
    catch (...) {}
}

StatsPersistenceInfo ReadStatsPersistenceInfo() {
    Lock lock;
    return gInfo;
}
