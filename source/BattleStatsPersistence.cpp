#include "BattleStatsPersistence.h"

#include "InstallCommon.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {
constexpr std::array<char, 8> kMagic = {'X', 'A', '6', '6', 'S', 'T', 'A', 'T'};
constexpr uint32_t kSchema = 1;
constexpr size_t kMaxSidecarBytes = 1024 * 1024;
constexpr uint32_t kMaxUnits = 250;
constexpr uint32_t kMaxScenarioCharacters = 1024;
constexpr uint32_t kMaxLabelCharacters = 256;

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    uint32_t schema;
    uint32_t active;
    uint32_t unitCount;
    uint32_t scenarioCharacters;
    uint64_t elapsedMilliseconds;
    char saveSha256[64];
};

struct UnitHeader {
    int32_t slot;
    int32_t defeatedEnemies;
    int64_t damageDealt;
    int64_t damageReceived;
    int64_t recoveryDone;
    int64_t criticalCount;
    int64_t comboExtraHitCount;
    int64_t attackBlockCount;
    int64_t spellBlockCount;
    uint32_t labelCharacters;
};
#pragma pack(pop)

bool HashAscii(const std::wstring& savePath, std::array<char, 64>& result,
    std::wstring& error) {
    std::wstring hash;
    if (!XiaoAnInstall::Sha256File(savePath, hash, error) || hash.size() != result.size())
        return false;
    for (size_t index = 0; index < result.size(); ++index) {
        const wchar_t value = hash[index];
        if (!((value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f') ||
            (value >= L'A' && value <= L'F'))) {
            error = L"存档 SHA-256 格式无效。";
            return false;
        }
        result[index] = static_cast<char>(value >= L'A' && value <= L'F' ? value + 32 : value);
    }
    return true;
}

template <typename T>
void AppendValue(std::vector<unsigned char>& bytes, const T& value) {
    const auto* begin = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), begin, begin + sizeof(value));
}

void AppendWide(std::vector<unsigned char>& bytes, const std::wstring& value) {
    const auto* begin = reinterpret_cast<const unsigned char*>(value.data());
    bytes.insert(bytes.end(), begin, begin + value.size() * sizeof(wchar_t));
}

template <typename T>
bool ReadValue(const std::vector<unsigned char>& bytes, size_t& offset, T& value) {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) return false;
    memcpy(&value, bytes.data() + offset, sizeof(value));
    offset += sizeof(value);
    return true;
}

bool ReadWide(const std::vector<unsigned char>& bytes, size_t& offset,
    uint32_t characters, std::wstring& value) {
    const size_t byteCount = static_cast<size_t>(characters) * sizeof(wchar_t);
    if (offset > bytes.size() || byteCount > bytes.size() - offset) return false;
    value.resize(characters);
    if (byteCount) memcpy(value.data(), bytes.data() + offset, byteCount);
    offset += byteCount;
    return true;
}

bool ReadWholeFile(const std::wstring& path, std::vector<unsigned char>& bytes,
    std::wstring& error) {
    bytes.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法打开战斗统计旁路文件。";
        return false;
    }
    LARGE_INTEGER size = {};
    bool okay = GetFileSizeEx(file, &size) && size.QuadPart >= static_cast<LONGLONG>(sizeof(FileHeader)) &&
        size.QuadPart <= static_cast<LONGLONG>(kMaxSidecarBytes);
    if (okay) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        okay = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
            read == bytes.size();
    }
    const DWORD failure = okay ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!okay) {
        error = L"战斗统计旁路文件不完整，Windows 错误码：" + std::to_wstring(failure);
        bytes.clear();
    }
    return okay;
}

bool WriteAtomic(const std::wstring& path, const std::vector<unsigned char>& bytes,
    std::wstring& error) {
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法创建战斗统计临时文件，Windows 错误码：" +
            std::to_wstring(GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool okay = bytes.size() <= MAXDWORD &&
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    const DWORD failure = okay ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!okay || !MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveFailure = okay ? GetLastError() : failure;
        DeleteFileW(temporary.c_str());
        error = L"无法提交战斗统计旁路文件，Windows 错误码：" +
            std::to_wstring(moveFailure);
        return false;
    }
    return true;
}
} // namespace

bool SaveBattleStatsSidecar(const std::wstring& savePath,
    const std::wstring& sidecarPath, const BattleStatsPersistedState& state,
    std::wstring& error) {
    error.clear();
    if (state.units.size() > kMaxUnits || state.scenario.size() > kMaxScenarioCharacters) {
        error = L"战斗统计数据超过旁路格式限制。";
        return false;
    }
    std::array<char, 64> hash = {};
    if (!HashAscii(savePath, hash, error)) return false;
    FileHeader header = {};
    memcpy(header.magic, kMagic.data(), kMagic.size());
    header.schema = kSchema;
    header.active = state.active ? 1u : 0u;
    header.unitCount = static_cast<uint32_t>(state.units.size());
    header.scenarioCharacters = static_cast<uint32_t>(state.scenario.size());
    header.elapsedMilliseconds = state.elapsedMilliseconds;
    memcpy(header.saveSha256, hash.data(), hash.size());
    std::vector<unsigned char> bytes;
    bytes.reserve(sizeof(header) + state.scenario.size() * sizeof(wchar_t) +
        state.units.size() * sizeof(UnitHeader));
    AppendValue(bytes, header);
    AppendWide(bytes, state.scenario);
    for (const BattleUnitTotals& unit : state.units) {
        if (unit.slot < 0 || unit.slot >= static_cast<int>(kMaxUnits) ||
            unit.label.size() > kMaxLabelCharacters || unit.damageDealt < 0 ||
            unit.damageReceived < 0 || unit.recoveryDone < 0 || unit.criticalCount < 0 ||
            unit.comboExtraHitCount < 0 || unit.attackBlockCount < 0 ||
            unit.spellBlockCount < 0 || unit.defeatedEnemies < 0) {
            error = L"战斗统计单位数据无效。";
            return false;
        }
        UnitHeader row = {};
        row.slot = unit.slot;
        row.defeatedEnemies = unit.defeatedEnemies;
        row.damageDealt = unit.damageDealt;
        row.damageReceived = unit.damageReceived;
        row.recoveryDone = unit.recoveryDone;
        row.criticalCount = unit.criticalCount;
        row.comboExtraHitCount = unit.comboExtraHitCount;
        row.attackBlockCount = unit.attackBlockCount;
        row.spellBlockCount = unit.spellBlockCount;
        row.labelCharacters = static_cast<uint32_t>(unit.label.size());
        AppendValue(bytes, row);
        AppendWide(bytes, unit.label);
    }
    if (bytes.size() > kMaxSidecarBytes) {
        error = L"战斗统计旁路文件过大。";
        return false;
    }
    return WriteAtomic(sidecarPath, bytes, error);
}

bool LoadBattleStatsSidecar(const std::wstring& savePath,
    const std::wstring& sidecarPath, BattleStatsPersistedState& state,
    std::wstring& error) {
    state = BattleStatsPersistedState();
    error.clear();
    std::vector<unsigned char> bytes;
    if (!ReadWholeFile(sidecarPath, bytes, error)) return false;
    size_t offset = 0;
    FileHeader header = {};
    if (!ReadValue(bytes, offset, header) || memcmp(header.magic, kMagic.data(), kMagic.size()) ||
        header.schema != kSchema || header.active > 1 || header.unitCount > kMaxUnits ||
        header.scenarioCharacters > kMaxScenarioCharacters) {
        error = L"战斗统计旁路文件头无效。";
        return false;
    }
    std::array<char, 64> hash = {};
    if (!HashAscii(savePath, hash, error)) return false;
    if (memcmp(header.saveSha256, hash.data(), hash.size())) {
        error = L"战斗统计旁路文件与当前存档不匹配。";
        return false;
    }
    BattleStatsPersistedState decoded;
    decoded.schemaVersion = 1;
    decoded.historyIncomplete = true;
    decoded.active = header.active != 0;
    decoded.elapsedMilliseconds = header.elapsedMilliseconds;
    if (!ReadWide(bytes, offset, header.scenarioCharacters, decoded.scenario)) {
        error = L"战斗统计剧本字段不完整。";
        return false;
    }
    std::array<bool, kMaxUnits> seen = {};
    decoded.units.reserve(header.unitCount);
    for (uint32_t index = 0; index < header.unitCount; ++index) {
        UnitHeader row = {};
        BattleUnitTotals unit;
        if (!ReadValue(bytes, offset, row) || row.slot < 0 ||
            row.slot >= static_cast<int32_t>(kMaxUnits) || seen[static_cast<size_t>(row.slot)] ||
            row.defeatedEnemies < 0 || row.damageDealt < 0 || row.damageReceived < 0 ||
            row.recoveryDone < 0 || row.criticalCount < 0 || row.comboExtraHitCount < 0 ||
            row.attackBlockCount < 0 || row.spellBlockCount < 0 ||
            row.labelCharacters > kMaxLabelCharacters ||
            !ReadWide(bytes, offset, row.labelCharacters, unit.label)) {
            error = L"战斗统计单位字段无效。";
            state = BattleStatsPersistedState();
            return false;
        }
        seen[static_cast<size_t>(row.slot)] = true;
        unit.slot = row.slot;
        unit.defeatedEnemies = row.defeatedEnemies;
        unit.damageDealt = row.damageDealt;
        unit.damageReceived = row.damageReceived;
        unit.recoveryDone = row.recoveryDone;
        unit.criticalCount = row.criticalCount;
        unit.comboExtraHitCount = row.comboExtraHitCount;
        unit.attackBlockCount = row.attackBlockCount;
        unit.spellBlockCount = row.spellBlockCount;
        decoded.units.push_back(std::move(unit));
    }
    if (offset != bytes.size()) {
        error = L"战斗统计旁路文件包含多余数据。";
        state = BattleStatsPersistedState();
        return false;
    }
    state = std::move(decoded);
    return true;
}

namespace {
static_assert(sizeof(wchar_t) == 2, "Statistics strings use UTF-16 code units");
constexpr std::array<char, 8> kPayloadMagic = {'X','A','S','T','A','T','2','P'};
constexpr std::array<char, 8> kFooterMagic = {'X','A','S','T','A','T','2','F'};
constexpr std::array<char, 8> kFooterEnd = {'X','A','S','T','A','T','E','N'};
constexpr uint32_t kEmbeddedSchema = 2;
constexpr uint32_t kPersonCount = 65535; // 0xFFFF is the engine's empty ID.
constexpr uint32_t kKnownMetricsMask = 0xFF;
constexpr uint32_t kMaxStatusCharacters = 4096;
constexpr uint64_t kMaxNativeBytes = 64ull * 1024 * 1024;
constexpr uint64_t kMaxPayloadBytes = 64ull * 1024 * 1024;

#pragma pack(push, 1)
struct EmbeddedHeader {
    char magic[8];
    uint32_t schema;
    uint32_t headerBytes;
    uint32_t flags;
    uint32_t unitCount;
    uint32_t campaignUnitCount;
    uint32_t scenarioCharacters;
    uint32_t statusCharacters;
    int32_t scenarioOrdinal;
    uint32_t knownMetrics;
    uint64_t elapsedMilliseconds;
    uint64_t campaignId;
    uint64_t battleInstance;
};
struct EmbeddedUnitHeader {
    int32_t personId;
    UnitHeader totals;
};
struct EmbeddedFooter {
    char magic[8];
    uint32_t schema;
    uint32_t footerBytes;
    uint64_t nativeBytes;
    uint64_t payloadBytes;
    char nativeSha256[64];
    char payloadSha256[64];
    char end[8];
};
#pragma pack(pop)

bool HashBuffer(const std::vector<unsigned char>& bytes, std::array<char, 64>& hash,
    std::wstring& error) {
    std::wstring text;
    if (!XiaoAnInstall::Sha256Bytes(bytes, text, error) || text.size() != hash.size()) return false;
    for (size_t i = 0; i < hash.size(); ++i) hash[i] = static_cast<char>(text[i]);
    return true;
}

bool ReadSaveBytes(const std::wstring& path, std::vector<unsigned char>& bytes,
    std::wstring& error) {
    bytes.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法打开存档读取战场统计，Windows 错误码：" + std::to_wstring(GetLastError());
        return false;
    }
    LARGE_INTEGER length = {};
    bool okay = GetFileSizeEx(file, &length) && length.QuadPart > 0 &&
        static_cast<uint64_t>(length.QuadPart) <= kMaxNativeBytes + kMaxPayloadBytes + sizeof(EmbeddedFooter);
    if (okay) {
        bytes.resize(static_cast<size_t>(length.QuadPart));
        DWORD read = 0;
        okay = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
            read == bytes.size();
    }
    const DWORD failure = okay ? ERROR_SUCCESS : GetLastError();
    const bool closed = CloseHandle(file) != FALSE;
    if (!okay || !closed) {
        error = L"存档长度无效或未完整读取，Windows 错误码：" + std::to_wstring(failure);
        bytes.clear();
        return false;
    }
    return true;
}

bool HasExtensionMarker(const std::vector<unsigned char>& bytes) {
    const size_t span = static_cast<size_t>(kMaxPayloadBytes + sizeof(EmbeddedFooter));
    const auto first = bytes.begin() + (bytes.size() > span ? bytes.size() - span : 0);
    // Recognize truncated/damaged footer magic by its intact payload marker,
    // and a truncated payload by the intact footer marker. Never silently
    // append another block over a suspicious, partially written extension.
    return std::search(first, bytes.end(), kPayloadMagic.begin(), kPayloadMagic.end()) != bytes.end() ||
        std::search(first, bytes.end(), kFooterMagic.begin(), kFooterMagic.end()) != bytes.end();
}

bool ValidTotals(const BattleUnitTotals& unit) {
    return unit.personId >= 0 && unit.personId < static_cast<int>(kPersonCount) &&
        unit.slot >= -1 && unit.slot < static_cast<int>(kMaxUnits) &&
        unit.label.size() <= kMaxLabelCharacters && unit.defeatedEnemies >= 0 &&
        unit.damageDealt >= 0 && unit.damageReceived >= 0 && unit.recoveryDone >= 0 &&
        unit.criticalCount >= 0 && unit.comboExtraHitCount >= 0 &&
        unit.attackBlockCount >= 0 && unit.spellBlockCount >= 0;
}

bool AppendGroup(std::vector<unsigned char>& bytes, const std::vector<BattleUnitTotals>& units,
    std::wstring& error) {
    std::vector<bool> seen(kPersonCount, false);
    for (const BattleUnitTotals& unit : units) {
        if (!ValidTotals(unit) || seen[static_cast<size_t>(unit.personId)]) {
            error = L"战场统计存在无效计数、人物编号、显示槽位或重复人物。";
            return false;
        }
        seen[static_cast<size_t>(unit.personId)] = true;
        EmbeddedUnitHeader row = {};
        row.personId = unit.personId;
        row.totals.slot = unit.slot;
        row.totals.defeatedEnemies = unit.defeatedEnemies;
        row.totals.damageDealt = unit.damageDealt;
        row.totals.damageReceived = unit.damageReceived;
        row.totals.recoveryDone = unit.recoveryDone;
        row.totals.criticalCount = unit.criticalCount;
        row.totals.comboExtraHitCount = unit.comboExtraHitCount;
        row.totals.attackBlockCount = unit.attackBlockCount;
        row.totals.spellBlockCount = unit.spellBlockCount;
        row.totals.labelCharacters = static_cast<uint32_t>(unit.label.size());
        AppendValue(bytes, row);
        AppendWide(bytes, unit.label);
    }
    return true;
}

bool ReadGroup(const std::vector<unsigned char>& bytes, size_t& offset, uint32_t count,
    std::vector<BattleUnitTotals>& units) {
    if (offset > bytes.size() || count > (bytes.size() - offset) / sizeof(EmbeddedUnitHeader)) return false;
    std::vector<bool> seen(kPersonCount, false);
    units.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        EmbeddedUnitHeader row = {};
        BattleUnitTotals unit;
        if (!ReadValue(bytes, offset, row) || row.totals.labelCharacters > kMaxLabelCharacters ||
            !ReadWide(bytes, offset, row.totals.labelCharacters, unit.label)) return false;
        unit.personId = row.personId;
        unit.slot = row.totals.slot;
        unit.defeatedEnemies = row.totals.defeatedEnemies;
        unit.damageDealt = row.totals.damageDealt;
        unit.damageReceived = row.totals.damageReceived;
        unit.recoveryDone = row.totals.recoveryDone;
        unit.criticalCount = row.totals.criticalCount;
        unit.comboExtraHitCount = row.totals.comboExtraHitCount;
        unit.attackBlockCount = row.totals.attackBlockCount;
        unit.spellBlockCount = row.totals.spellBlockCount;
        if (!ValidTotals(unit) || seen[static_cast<size_t>(unit.personId)]) return false;
        seen[static_cast<size_t>(unit.personId)] = true;
        units.push_back(std::move(unit));
    }
    return true;
}

BattleStatsReadResult DecodeEmbedded(const std::vector<unsigned char>& bytes,
    BattleStatsPersistedState& state, size_t& nativeBytes, std::wstring& error) {
    nativeBytes = bytes.size();
    EmbeddedFooter footer = {};
    if (bytes.size() < sizeof(footer) ||
        (memcpy(&footer, bytes.data() + bytes.size() - sizeof(footer), sizeof(footer)),
         memcmp(footer.magic, kFooterMagic.data(), kFooterMagic.size()))) {
        if (!HasExtensionMarker(bytes)) return BattleStatsReadResult::Missing;
        error = L"存档中的战场统计扩展块被截断或尾部标记已损坏。";
        return BattleStatsReadResult::Corrupt;
    }
    if (footer.schema != kEmbeddedSchema) {
        error = L"存档使用了当前补丁不支持的战场统计版本。";
        return BattleStatsReadResult::Unsupported;
    }
    if (footer.footerBytes != sizeof(footer) || memcmp(footer.end, kFooterEnd.data(), kFooterEnd.size()) ||
        !footer.nativeBytes || footer.nativeBytes > kMaxNativeBytes ||
        footer.payloadBytes < sizeof(EmbeddedHeader) || footer.payloadBytes > kMaxPayloadBytes ||
        footer.nativeBytes + footer.payloadBytes + sizeof(footer) != bytes.size()) {
        error = L"存档中的战场统计扩展长度或结束标记无效。";
        return BattleStatsReadResult::Corrupt;
    }
    nativeBytes = static_cast<size_t>(footer.nativeBytes);
    std::vector<unsigned char> native(bytes.begin(), bytes.begin() + nativeBytes);
    std::vector<unsigned char> payload(bytes.begin() + nativeBytes, bytes.end() - sizeof(footer));
    std::array<char, 64> hash = {};
    if (!HashBuffer(native, hash, error) || memcmp(hash.data(), footer.nativeSha256, hash.size()) ||
        !HashBuffer(payload, hash, error) || memcmp(hash.data(), footer.payloadSha256, hash.size())) {
        error = L"存档本体或战场统计扩展校验失败，不能使用其中的统计数据。";
        return BattleStatsReadResult::Corrupt;
    }
    size_t offset = 0;
    EmbeddedHeader header = {};
    if (!ReadValue(payload, offset, header) ||
        memcmp(header.magic, kPayloadMagic.data(), kPayloadMagic.size()) ||
        header.schema != kEmbeddedSchema || header.headerBytes != sizeof(header) ||
        (header.flags & ~3u) || header.unitCount > kMaxUnits || header.campaignUnitCount > kPersonCount ||
        header.scenarioCharacters > kMaxScenarioCharacters || header.statusCharacters > kMaxStatusCharacters ||
        header.scenarioOrdinal < -1 || header.scenarioOrdinal >= static_cast<int32_t>(kPersonCount) ||
        (header.knownMetrics & ~kKnownMetricsMask)) {
        error = L"存档中的战场统计版本字段、人物数量或文本长度无效。";
        return BattleStatsReadResult::Corrupt;
    }
    BattleStatsPersistedState decoded;
    decoded.schemaVersion = kEmbeddedSchema;
    decoded.active = (header.flags & 1) != 0;
    decoded.historyIncomplete = (header.flags & 2) != 0;
    decoded.scenarioOrdinal = header.scenarioOrdinal;
    decoded.knownMetrics = header.knownMetrics;
    decoded.elapsedMilliseconds = header.elapsedMilliseconds;
    decoded.campaignId = header.campaignId;
    decoded.battleInstance = header.battleInstance;
    if (!ReadWide(payload, offset, header.scenarioCharacters, decoded.scenario) ||
        !ReadWide(payload, offset, header.statusCharacters, decoded.status) ||
        !ReadGroup(payload, offset, header.unitCount, decoded.units) ||
        !ReadGroup(payload, offset, header.campaignUnitCount, decoded.campaignUnits) || offset != payload.size()) {
        error = L"存档中的战场统计人物数据不完整、重复或包含非法计数。";
        return BattleStatsReadResult::Corrupt;
    }
    state = std::move(decoded);
    return BattleStatsReadResult::Loaded;
}

bool ReplaceWithBytes(const std::wstring& path, const std::vector<unsigned char>& bytes,
    std::wstring& error) {
    static volatile LONG serial = 0;
    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 32 && file == INVALID_HANDLE_VALUE; ++attempt) {
        temporary = path + L".stats-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(static_cast<unsigned long>(InterlockedIncrement(&serial))) + L".tmp";
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法创建战场统计扩展事务文件，Windows 错误码：" + std::to_wstring(GetLastError());
        return false;
    }
    DWORD written = 0;
    bool okay = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    DWORD failure = okay ? ERROR_SUCCESS : GetLastError();
    if (!CloseHandle(file)) { okay = false; failure = GetLastError(); }
    if (okay && !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        okay = false; failure = GetLastError();
    }
    if (!okay) {
        DeleteFileW(temporary.c_str());
        error = L"无法提交战场统计扩展，原存档保持不变，Windows 错误码：" + std::to_wstring(failure);
    }
    return okay;
}
} // namespace

BattleStatsReadResult ReadBattleStatsEmbedded(const std::wstring& savePath,
    BattleStatsPersistedState& state, std::wstring& error) {
    state = BattleStatsPersistedState();
    error.clear();
    std::vector<unsigned char> bytes;
    if (!ReadSaveBytes(savePath, bytes, error)) return BattleStatsReadResult::Corrupt;
    size_t nativeBytes = 0;
    return DecodeEmbedded(bytes, state, nativeBytes, error);
}

bool WriteBattleStatsEmbedded(const std::wstring& savePath,
    const BattleStatsPersistedState& state, std::wstring& error) {
    error.clear();
    if (state.schemaVersion != kEmbeddedSchema || state.units.size() > kMaxUnits ||
        state.campaignUnits.size() > kPersonCount || state.scenario.size() > kMaxScenarioCharacters ||
        state.status.size() > kMaxStatusCharacters || state.scenarioOrdinal < -1 ||
        state.scenarioOrdinal >= static_cast<int>(kPersonCount) || (state.knownMetrics & ~kKnownMetricsMask)) {
        error = L"战场统计状态版本、数量、文本长度或已知指标字段超过格式限制。";
        return false;
    }
    std::vector<unsigned char> payload;
    EmbeddedHeader header = {};
    memcpy(header.magic, kPayloadMagic.data(), kPayloadMagic.size());
    header.schema = kEmbeddedSchema;
    header.headerBytes = sizeof(header);
    header.flags = (state.active ? 1u : 0u) | (state.historyIncomplete ? 2u : 0u);
    header.unitCount = static_cast<uint32_t>(state.units.size());
    header.campaignUnitCount = static_cast<uint32_t>(state.campaignUnits.size());
    header.scenarioCharacters = static_cast<uint32_t>(state.scenario.size());
    header.statusCharacters = static_cast<uint32_t>(state.status.size());
    header.scenarioOrdinal = state.scenarioOrdinal;
    header.knownMetrics = state.knownMetrics;
    header.elapsedMilliseconds = state.elapsedMilliseconds;
    header.campaignId = state.campaignId;
    header.battleInstance = state.battleInstance;
    AppendValue(payload, header);
    AppendWide(payload, state.scenario);
    AppendWide(payload, state.status);
    if (!AppendGroup(payload, state.units, error) || !AppendGroup(payload, state.campaignUnits, error)) return false;
    if (payload.size() > kMaxPayloadBytes) {
        error = L"战场统计扩展超过大小上限。";
        return false;
    }
    std::vector<unsigned char> bytes;
    if (!ReadSaveBytes(savePath, bytes, error)) return false;
    BattleStatsPersistedState previous;
    size_t nativeBytes = 0;
    const auto existing = DecodeEmbedded(bytes, previous, nativeBytes, error);
    if (existing == BattleStatsReadResult::Corrupt || existing == BattleStatsReadResult::Unsupported) return false;
    if (!nativeBytes || nativeBytes > kMaxNativeBytes) {
        error = L"原生存档载荷长度超过战场统计格式上限。";
        return false;
    }
    bytes.resize(nativeBytes);
    EmbeddedFooter footer = {};
    memcpy(footer.magic, kFooterMagic.data(), kFooterMagic.size());
    memcpy(footer.end, kFooterEnd.data(), kFooterEnd.size());
    footer.schema = kEmbeddedSchema;
    footer.footerBytes = sizeof(footer);
    footer.nativeBytes = nativeBytes;
    footer.payloadBytes = payload.size();
    std::array<char, 64> hash = {};
    if (!HashBuffer(bytes, hash, error)) return false;
    memcpy(footer.nativeSha256, hash.data(), hash.size());
    if (!HashBuffer(payload, hash, error)) return false;
    memcpy(footer.payloadSha256, hash.data(), hash.size());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    AppendValue(bytes, footer);
    if (!ReplaceWithBytes(savePath, bytes, error)) return false;
    // Production calls this on the transaction's native staging E5S, before
    // replacing the player's save. Confirm the actual closed file contains
    // precisely the native prefix, payload and hashes that were prepared.
    std::vector<unsigned char> verified;
    if (!ReadSaveBytes(savePath, verified, error)) return false;
    if (verified != bytes) {
        error = L"战场统计临时存档复读校验失败，未提交玩家存档。";
        return false;
    }
    return true;
}

