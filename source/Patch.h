#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <stdint.h>
#include <math.h>

struct Settings {
    bool superEnabled = false;
    bool superFlags[5] = {true, true, true, true, true};
    bool phaseEnabled = false;
    bool phaseFlags[3] = {true, true, true};
    bool wheelEnabled = false;
    int phase[3] = {60, 60, 60};
    int wheelStep = 5;
    bool gearEnabled = false;
    int gearRaw = 0;
    bool shiftKey = true;
    WORD hotkeys[5] = {'Z', 'X', 'V', 'W', 'Q'};
};

extern Settings gSettings;
extern HMODULE gInstance;
extern uintptr_t gGameBase;
extern HWND gMainWindow;
extern HWND gDialog;

bool InitializeEngine();
bool ApplyEngineState(const Settings& settings);
void RestoreEngine();
void SetNativeToggleHandler(void (__cdecl* handler)());
const wchar_t* EngineError();
int CurrentGamePhase();
int StageDelay(int value, int& counter);
void CommitSettings();
bool SaveSettings();
bool OpenConfig(HWND owner);
void SyncConfig();
bool HandleConfigMessage(MSG* message);

inline int Bound(int value, int lo, int hi) { return value < lo ? lo : value > hi ? hi : value; }
inline int GearStep(int raw, int direction) {
    if (direction > 0) {
        if (raw < 0) return raw / 2;
        if (raw == 0) return 1;
        return raw <= 64 ? raw * 2 : raw;
    }
    if (raw > 0) return raw / 2;
    if (raw == 0) return -1;
    return raw >= -64 ? raw * 2 : raw;
}
inline int GearPosition(int raw) {
    if (!raw) return 8;
    return Bound(static_cast<int>(raw > 0 ? log2(static_cast<double>(raw)) + 9.0 :
        7.0 - log2(-static_cast<double>(raw))), 0, 16);
}
inline int GearValue(int position) {
    static const int values[17] = {-128, -64, -32, -16, -8, -4, -2, -1, 0, 1, 2, 4, 8, 16, 32, 64, 128};
    return values[Bound(position, 0, 16)];
}
