#pragma once

#include "Patch.h"

struct SaveEnhancerImageLayout {
    uintptr_t nativeAuto = 0;
    uintptr_t nativeSave = 0;
    uintptr_t nativeEndAuto = 0;
    uintptr_t startCall = 0;
    uintptr_t endCall = 0;
    uintptr_t nativeFilename = 0;
    uintptr_t saveFilenameCall = 0;
    uintptr_t saveResult = 0;
    uintptr_t loadObserve = 0;
    uintptr_t loadResult = 0;
    uintptr_t campaignByte = 0;
    uintptr_t loadContinuation = 0;
    uintptr_t commitSite = 0;
    uintptr_t commitContinuation = 0;
    uintptr_t commitFailure = 0;
    uintptr_t nativePostWrite = 0;
    uintptr_t loadStartSite = 0;
    uintptr_t loadStartContinuation = 0;
    uintptr_t nativeWrite = 0;
    uintptr_t nativeClose = 0;
};

bool InitializeSaveEnhancer(const wchar_t* gameRoot);
void PollSaveEnhancer();
void RestoreSaveEnhancer();
bool OpenSaveBrowser(HWND owner, const wchar_t* gameRoot);
void CloseSaveBrowser();
bool IsSaveBrowserWindow(HWND window);
bool HandleSaveBrowserMessage(MSG* message);
bool ResolveSaveEnhancerImageForTest(BYTE* image, uintptr_t virtualBase,
    SaveEnhancerImageLayout& layout);

// Native transaction harness; callers must supply an isolated test root.
void ConfigureSaveEnhancerForTest(const wchar_t* gameRoot, BYTE* campaign,
    uintptr_t nativeSave, uintptr_t nativeFilename);
void SavePrivateForTest(void* game, bool atStart);
void RouteSaveFilenameForTest(char* output, int rawSlot);
void CompleteSaveForTest(int rawSlot);
int EffectiveSaveSlotForTest(int rawSlot);
void ConfigureSaveEnhancerBypassForTest(BYTE* first, BYTE* second, WORD firstPatch, WORD secondPatch);
uintptr_t ConfigureSaveBoundaryBridgeForTest(bool start, bool nativeSaveCall, uintptr_t native);
void ConfigureSaveBoundaryEnableForTest(bool start, BYTE* enabled);
void ConfigureSaveLoadBridgesForTest(uintptr_t continuation,
    uintptr_t& start, uintptr_t& result);
void RouteLoadFilenameForTest(char* output, int rawSlot);
void ObserveSaveLoadForTest(int rawSlot);
void CompleteSaveLoadForTest(bool succeeded);
uintptr_t ConfigureSaveCommitBridgeForTest(uintptr_t post, uintptr_t continuation, uintptr_t failure);
uintptr_t SaveResultBridgeForTest();
void SetSaveTransactionalHooksForTest(bool enabled);
void SetSaveFailureDialogForTest(bool enabled);
