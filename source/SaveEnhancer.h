#pragma once

#include "Patch.h"

bool InitializeSaveEnhancer(const wchar_t* gameRoot);
void RestoreSaveEnhancer();
bool OpenSaveBrowser(HWND owner, const wchar_t* gameRoot);
void CloseSaveBrowser();
bool IsSaveBrowserWindow(HWND window);
bool HandleSaveBrowserMessage(MSG* message);
