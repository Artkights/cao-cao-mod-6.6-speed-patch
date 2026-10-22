#pragma once

#include "BattleStats.h"

#include <string>

enum class BattleStatsReadResult { Loaded, Missing, Corrupt, Unsupported };

// The E5S native prefix is preserved byte for byte. Call the writer only after
// the native writer has closed its fresh transaction file; the save coordinator
// remains responsible for committing that file to the player's save slot.
bool WriteBattleStatsEmbedded(const std::wstring& savePath,
    const BattleStatsPersistedState& state, std::wstring& error);
BattleStatsReadResult ReadBattleStatsEmbedded(const std::wstring& savePath,
    BattleStatsPersistedState& state, std::wstring& error);

// Legacy schema 1 only. A successful legacy read explicitly returns
// state.schemaVersion == 1 and leaves person identity migration to the model.
bool SaveBattleStatsSidecar(const std::wstring& savePath,
    const std::wstring& sidecarPath, const BattleStatsPersistedState& state,
    std::wstring& error);
bool LoadBattleStatsSidecar(const std::wstring& savePath,
    const std::wstring& sidecarPath, BattleStatsPersistedState& state,
    std::wstring& error);

