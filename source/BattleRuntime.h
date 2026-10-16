#pragma once

#include "Patch.h"

#include <string>
#include <vector>

enum BattleRuntimeCapability : unsigned {
    BattleCapabilityUnits = 1u << 0,
    BattleCapabilityPeople = 1u << 1,
    BattleCapabilityEquipment = 1u << 2,
    BattleCapabilityStatuses = 1u << 3,
    BattleCapabilityAi = 1u << 4
};

struct BattleUnitSnapshot {
    int slot = -1;
    int dataId = -1;
    int camp = -1;
    int x = -1;
    int y = -1;
    int targetSlot = -1;
    int targetX = -1;
    int targetY = -1;
    int actionOrder = -1;
    int visibility = 0;
    int actionState = 0;
    int aiPolicy = -1;
    int direction = -1;
    int hp = 0;
    int mp = 0;
    int status = 0;
    int level = -1;
    int weaponId = -1;
    int armorId = -1;
    int accessoryId = -1;
    int weaponLevel = -1;
    int armorLevel = -1;
    bool hasWeapon = false;
    bool hasArmor = false;
    bool hasAccessory = false;
    std::wstring name;
    std::wstring weaponName;
    std::wstring armorName;
    std::wstring accessoryName;
};

struct BattleSnapshot {
    unsigned capabilities = 0;
    ULONGLONG capturedAt = 0;
    std::vector<BattleUnitSnapshot> units;
};

bool InitializeBattleRuntime(const wchar_t* gameRoot);
void ResetBattleRuntime();
bool ReadBattleSnapshot(BattleSnapshot& snapshot);
unsigned BattleRuntimeCapabilities();
std::wstring BattleCampName(int camp);
std::wstring BattleAiName(int policy);
std::wstring BattleStatusText(int status);
std::wstring BattleUnitLabel(const BattleUnitSnapshot& unit);

// Offline semantic resolver used by the native regression harness.
bool ResolveBattleRuntimeImageForTest(const BYTE* image, uintptr_t virtualBase,
    uintptr_t& unitBase, uintptr_t& personPointerSlot);
bool LoadBattleItemNameForTest(const wchar_t* gameRoot, int itemId, std::wstring& name);
