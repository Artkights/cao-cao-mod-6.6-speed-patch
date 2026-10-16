#pragma once

#include "Patch.h"

bool OpenBattleInfo(HWND owner);
void CloseBattleInfo();
bool IsBattleInfoWindow(HWND window);
bool HandleBattleInfoMessage(MSG* message);
