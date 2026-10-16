#pragma once

#include "Patch.h"

#include <cstddef>
#include <string>

enum class AnimationKind : unsigned char {
    Movement,
    Physical,
    Strategy,
    Status,
    Numbers,
    BattlefieldText,
    Count
};

struct FeatureSettings {
    bool animationCamps[3] = {true, true, true};
    bool animationKinds[static_cast<size_t>(AnimationKind::Count)] = {true, true, true, true, true, true};
    bool autoSaveTurnStart = false;
    bool autoSaveTurnEnd = false;
    bool saveScreenshots = true;
    int autoSaveSlots = 10;
    int battleRefreshMilliseconds = 250;
};

extern FeatureSettings gFeatureSettings;

bool LoadFeatureSettings(const wchar_t* gameRoot);
bool SaveFeatureSettings(const wchar_t* gameRoot);
const std::wstring& FeatureSettingsPath();

