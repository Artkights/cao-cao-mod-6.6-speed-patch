#include "Features.h"
#include "EngineProfile.h"

#include <algorithm>
#include <cstdio>
#include <vector>

FeatureSettings gFeatureSettings;

namespace {
std::wstring gFeaturePath;

bool EnsureDirectory(const std::wstring& path) {
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

int ReadValue(const wchar_t* section, const wchar_t* key, int fallback, int low, int high) {
    const int value = GetPrivateProfileIntW(section, key, fallback, gFeaturePath.c_str());
    return std::max(low, std::min(high, value));
}

void AppendValue(std::wstring& text, const wchar_t* key, int value) {
    text += key;
    text += L"=";
    text += std::to_wstring(value);
    text += L"\r\n";
}
}

bool LoadFeatureSettings(const wchar_t* gameRoot) {
    if (!gameRoot || !*gameRoot) return false;
    gFeaturePath = PatchPrivatePath(gameRoot, CompiledEngineProfile()) + L"\\features.ini";
    FeatureSettings settings;
    static const wchar_t* campKeys[] = {L"Player", L"Friend", L"Enemy"};
    static const wchar_t* kindKeys[] = {
        L"Movement", L"Physical", L"Strategy", L"Status", L"Numbers", L"BattlefieldText"
    };
    for (size_t i = 0; i < _countof(campKeys); ++i)
        settings.animationCamps[i] = ReadValue(L"Animation", campKeys[i], 1, 0, 1) != 0;
    for (size_t i = 0; i < _countof(kindKeys); ++i)
        settings.animationKinds[i] = ReadValue(L"Animation", kindKeys[i], 1, 0, 1) != 0;
    settings.autoSaveTurnStart = ReadValue(L"Save", L"TurnStart", 0, 0, 1) != 0;
    settings.autoSaveTurnEnd = ReadValue(L"Save", L"TurnEnd", 0, 0, 1) != 0;
    settings.saveScreenshots = ReadValue(L"Save", L"Screenshots", 1, 0, 1) != 0;
    settings.autoSaveSlots = ReadValue(L"Save", L"Slots", 10, 1, 100);
    settings.battleRefreshMilliseconds = ReadValue(L"BattleInfo", L"RefreshMilliseconds", 250, 100, 2000);
    gFeatureSettings = settings;
    return true;
}

bool SaveFeatureSettings(const wchar_t* gameRoot) {
    if (!gameRoot || !*gameRoot) return false;
    if (gFeaturePath.empty()) gFeaturePath = PatchPrivatePath(gameRoot, CompiledEngineProfile()) + L"\\features.ini";
    const std::wstring patch = std::wstring(gameRoot) + L"\\patch";
    const std::wstring folder = PatchPrivatePath(gameRoot, CompiledEngineProfile());
    if (!EnsureDirectory(patch) || !EnsureDirectory(folder)) return false;

    std::wstring text = L"[Features]\r\nSchemaVersion=1\r\n\r\n[Animation]\r\n";
    static const wchar_t* campKeys[] = {L"Player", L"Friend", L"Enemy"};
    static const wchar_t* kindKeys[] = {
        L"Movement", L"Physical", L"Strategy", L"Status", L"Numbers", L"BattlefieldText"
    };
    for (size_t i = 0; i < _countof(campKeys); ++i)
        AppendValue(text, campKeys[i], gFeatureSettings.animationCamps[i] ? 1 : 0);
    for (size_t i = 0; i < _countof(kindKeys); ++i)
        AppendValue(text, kindKeys[i], gFeatureSettings.animationKinds[i] ? 1 : 0);
    text += L"\r\n[Save]\r\n";
    AppendValue(text, L"TurnStart", gFeatureSettings.autoSaveTurnStart ? 1 : 0);
    AppendValue(text, L"TurnEnd", gFeatureSettings.autoSaveTurnEnd ? 1 : 0);
    AppendValue(text, L"Screenshots", gFeatureSettings.saveScreenshots ? 1 : 0);
    AppendValue(text, L"Slots", Bound(gFeatureSettings.autoSaveSlots, 1, 100));
    text += L"\r\n[BattleInfo]\r\n";
    AppendValue(text, L"RefreshMilliseconds", Bound(gFeatureSettings.battleRefreshMilliseconds, 100, 2000));

    const std::wstring temporary = gFeaturePath + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool success = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    success = success && WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes;
    success = success && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (success) success = MoveFileExW(temporary.c_str(), gFeaturePath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!success) DeleteFileW(temporary.c_str());
    return success;
}

const std::wstring& FeatureSettingsPath() {
    return gFeaturePath;
}

