#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ScenarioCondition {
    unsigned id = 0;
    std::vector<int32_t> values;
    // Some engine extensions encode a typed expression as a NUL-terminated
    // text parameter.  Keep it alongside the numeric fields when the
    // condition is inherited into a child event; dropping it makes the
    // condition indistinguishable from a malformed/unknown command.
    std::vector<std::wstring> texts;
};

struct ScenarioCommand {
    unsigned id = 0;
    std::vector<int32_t> values;
    std::vector<std::wstring> texts;
    std::vector<uint16_t> parameterTags;
    std::vector<ScenarioCondition> conditions;
    size_t ordinal = 0;
    size_t block = 0;
    size_t event = 0;
    size_t offset = 0;
    size_t section = 0;
    size_t scene = 0;
};

// Shared, bounded EEX parser used by guide analysis and legacy roster migration.
// Malformed sections are diagnosed and skipped; valid sections remain usable.
bool ParseScenarioScript(const std::vector<unsigned char>& bytes,
    std::vector<ScenarioCommand>& commands, const std::atomic<bool>* cancelled = nullptr,
    std::wstring* diagnostics = nullptr);

// Static candidates are deliberately not proof of executed joins. Callers must
// mark them as LegacyStaticR and preserve the history-incomplete warning.
bool ExtractScenarioJoinCandidates(const std::wstring& path,
    std::vector<int>& personIds, std::wstring& diagnostics);
