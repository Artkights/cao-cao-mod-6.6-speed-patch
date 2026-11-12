#pragma once

#include "Patch.h"
#include "EngineProfile.h"

#include <cstdint>
#include <string>
#include <vector>

enum BattleRuntimeCapability : unsigned {
    BattleCapabilityUnits = 1u << 0,
    BattleCapabilityPeople = 1u << 1,
    BattleCapabilityEquipment = 1u << 2,
    BattleCapabilityStatuses = 1u << 3,
    BattleCapabilityAi = 1u << 4,
    // Native deployed-state loop and its coordinate consumer were verified.
    BattleCapabilityDeployment = 1u << 5
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

enum class BattleResourceOrigin { Unavailable, Disk, Runtime, RuntimeWithDiskFallback };

// Immutable copies. Layout recognition and loaded-table readiness are separate:
// a valid item table must not make an unloaded person table appear ready.
struct BattleResourceSnapshot {
    EngineProfileId profile = EngineProfileId::Unknown;
    DWORD processId = 0;
    uint64_t processCreatedAt = 0;
    uintptr_t moduleBase = 0;
    uint64_t processGeneration = 0;
    uint64_t resourceGeneration = 0;
    ULONGLONG capturedAt = 0;
    bool runtimeProfileReady = false;
    bool peopleLayoutReady = false;
    bool itemsLayoutReady = false;
    bool peopleReady = false;
    bool itemsReady = false;
    BattleResourceOrigin peopleOrigin = BattleResourceOrigin::Unavailable;
    BattleResourceOrigin itemOrigin = BattleResourceOrigin::Unavailable;
    std::vector<std::wstring> people;
    std::vector<std::wstring> items;
    std::wstring diagnostic;
};

bool ReadBattleResourceSnapshot(BattleResourceSnapshot& snapshot);
bool RefreshBattleResourceSnapshot();
bool IsBattleResourceSnapshotCurrent(const BattleResourceSnapshot& snapshot);

bool InitializeBattleRuntime(const wchar_t* gameRoot);
void ResetBattleRuntime();
bool ReadBattleSnapshot(BattleSnapshot& snapshot);
// Copies validated names from the game's already-loaded runtime tables. The
// result owns its strings and never exposes native pointers. Callers enable
// this only after strict engine/profile validation has completed.
bool ReadBattleResourceNames(std::vector<std::wstring>& people,
    std::vector<std::wstring>& items);
bool BattleRuntimeResourceNamesEnabled() noexcept;
void ConfigureBattleRuntimeResourceNames(bool enabled) noexcept;
unsigned BattleRuntimeCapabilities();
std::wstring BattleCampName(int camp);
std::wstring BattleAiName(int policy);
std::wstring BattleStatusText(int status);
std::wstring BattleUnitLabel(const BattleUnitSnapshot& unit);
bool ReadBattlePersonName(uintptr_t peoplePointerSlot, int personId, std::wstring& name);

// Offline semantic resolver used by the native regression harness.
bool ResolveBattleRuntimeImageForTest(const BYTE* image, uintptr_t virtualBase,
    uintptr_t& unitBase, uintptr_t& personPointerSlot);
bool LoadBattleItemNameForTest(const wchar_t* gameRoot, int itemId, std::wstring& name);
bool ResolveBattleItemTableForTest(const BYTE* image, uintptr_t virtualBase,
    uintptr_t& itemBase);
bool ResolveBattleDeploymentForTest(const BYTE* image, uintptr_t virtualBase);
#ifdef XA66_TEST
void ConfigureBattleResourceLayoutForTest(uintptr_t personPointerSlot, uintptr_t itemBase);
bool ReadBattleResourceNamesForTest(uintptr_t personPointerSlot, uintptr_t itemBase,
    std::vector<std::wstring>& people, std::vector<std::wstring>& items);
#endif
