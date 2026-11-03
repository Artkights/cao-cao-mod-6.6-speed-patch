#pragma once

#include "BattleStats.h"

struct StatsPersistenceInfo {
    bool available = false;
    bool nativeExtension = false;
    std::wstring capability;
    std::wstring lastSave;
    std::wstring lastLoad;
    std::wstring lastReset;
};

// Diagnostic failures must never affect saving, loading, or the statistics model.
void InitializeStatsDiagnostics(const wchar_t* root) noexcept;
void SetStatsPersistenceCapability(bool available, bool nativeExtension,
    const std::wstring& detail) noexcept;
void SetStatsPersistenceCapability(bool available, bool nativeExtension,
    const wchar_t* detail) noexcept;
uint64_t BeginStatsOperation(const wchar_t* event, const std::wstring& path) noexcept;
void RecordStatsDiagnostic(uint64_t operation, const wchar_t* stage,
    const std::wstring& path, const BattleStatsPersistedState* state,
    const std::wstring& detail) noexcept;
void RecordStatsDiagnostic(uint64_t operation, const wchar_t* stage,
    const std::wstring& path, const BattleStatsPersistedState* state = nullptr,
    const wchar_t* detail = L"") noexcept;
void SetStatsLastSave(bool succeeded, const std::wstring& path,
    const std::wstring& detail) noexcept;
void SetStatsLastSave(bool succeeded, const std::wstring& path,
    const wchar_t* detail) noexcept;
void SetStatsLastLoad(bool succeeded, const std::wstring& path,
    const std::wstring& detail) noexcept;
void SetStatsLastLoad(bool succeeded, const std::wstring& path,
    const wchar_t* detail) noexcept;
StatsPersistenceInfo ReadStatsPersistenceInfo();
