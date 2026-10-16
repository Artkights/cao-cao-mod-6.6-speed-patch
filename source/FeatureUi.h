#pragma once

#include "Patch.h"

bool OpenFeatureSettings(HWND owner, const wchar_t* gameRoot);
void CloseFeatureSettings();
bool IsFeatureSettingsWindow(HWND window);
bool HandleFeatureSettingsMessage(MSG* message);
