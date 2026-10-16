#pragma once

#include "Patch.h"
#include <cstddef>
#include <string>
#include <vector>

// The guide is deliberately independent from the accelerator state.  It reads
// the current game's script/data files and owns its own modeless window.
bool OpenGuide(HWND owner, const wchar_t* gameRoot);
void CloseGuide();
bool IsGuideWindow(HWND window);
bool HandleGuideMessage(MSG* message);

// Called by the EXE CreateFileA import hook.  The guide keeps only the last
// successfully shaped RS/R_XX.eex or RS/S_XX.eex name; it never changes what
// the game opens.
void ObserveGuideScenarioPathA(const char* path);
bool GetObservedGuideScenario(std::wstring& name);

// Used by the native regression harness; the shipped DLL does not call this
// entry point from its game hooks.
bool BuildGuideTextForTest(const wchar_t* scenarioPath, const wchar_t* gameRoot, std::wstring& text);
bool ResolveGuideScenarioForTest(const wchar_t* scenarioPath, const wchar_t* gameRoot,
    std::wstring& battlefieldPath);
bool ListGuideScenarioJumpsForTest(const wchar_t* scenarioPath, std::vector<std::wstring>& targets);
bool FormatGuideScenarioJumpForTest(int rawValue, std::wstring& target);
bool EnumerateGuideScenariosForTest(const wchar_t* gameRoot, std::vector<std::wstring>& names);
bool SelectObservedGuideScenarioForTest(const wchar_t* gameRoot, std::wstring& selectedName,
    std::wstring& battlefieldPath);
bool FormatGuideStateSummaryForTest(unsigned attributeMask, int change, int abnormalMask,
    int hpLoss, int mpLoss, std::wstring& text);

struct GuideTestStats {
    size_t commands = 0;
    size_t ambushCommands = 0;
    size_t aiCommands = 0;
    size_t conditionedAmbushCommands = 0;
    size_t matchedAmbushCommands = 0;
    size_t conditionedAiCommands = 0;
    size_t matchedAiCommands = 0;
    size_t changingAiCommands = 0;
    size_t dialogEntries = 0;
    size_t ambushEntries = 0;
    size_t aiEntries = 0;
    size_t stateEntries = 0;
    size_t rewardEntries = 0;
    size_t ambushConditions[124] = {};
    size_t aiConditions[124] = {};
};

bool BuildGuideStatsForTest(const wchar_t* scenarioPath, const wchar_t* gameRoot, GuideTestStats& stats);

