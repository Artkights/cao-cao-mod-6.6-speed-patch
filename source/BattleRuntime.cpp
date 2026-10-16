#include "BattleRuntime.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace {

bool DecodeWithCodePage(UINT codePage, DWORD flags, const BYTE* bytes, size_t length,
    std::wstring& result) {
    if (!bytes || !length || length > static_cast<size_t>(INT_MAX)) return false;
    const int count = MultiByteToWideChar(codePage, flags,
        reinterpret_cast<const char*>(bytes), static_cast<int>(length), nullptr, 0);
    if (count <= 0) return false;
    result.assign(static_cast<size_t>(count), L'\0');
    return MultiByteToWideChar(codePage, flags, reinterpret_cast<const char*>(bytes),
        static_cast<int>(length), result.data(), count) == count;
}

// Game data is normally stored as GBK, but a few 6.6x tables were exported
// as UTF-8.  Do not use CP_ACP here: the proxy must render the same names on
// a machine whose system locale is not Chinese.  UTF-8 is accepted only when
// it is explicitly marked or validates completely; otherwise CP936 is the
// stable game-data fallback.
std::wstring DecodeGameName(const BYTE* bytes, size_t length) {
    if (!bytes || !length) return {};
    const BYTE* terminator = static_cast<const BYTE*>(memchr(bytes, 0, length));
    if (terminator) length = static_cast<size_t>(terminator - bytes);
    while (length && (bytes[length - 1] == 0 || bytes[length - 1] == ' ')) --length;
    while (length && (*bytes == 0 || *bytes == ' ')) {
        ++bytes;
        --length;
    }
    if (!bytes || !length) return {};

    bool utf8Marked = length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
    if (utf8Marked) {
        bytes += 3;
        length -= 3;
    }
    std::wstring result;
    if ((utf8Marked || (length >= 2 && std::any_of(bytes, bytes + length,
            [](BYTE value) { return value >= 0x80; }))) &&
        DecodeWithCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, length, result)) {
        return result;
    }
    if (!DecodeWithCodePage(936, MB_ERR_INVALID_CHARS, bytes, length, result) &&
        !DecodeWithCodePage(936, 0, bytes, length, result)) return {};
    return result;
}

struct PeImage {
    const BYTE* bytes = nullptr;
    uintptr_t runtimeBase = 0;
    const IMAGE_NT_HEADERS32* nt = nullptr;
    const IMAGE_SECTION_HEADER* sections = nullptr;
    unsigned sectionCount = 0;

    bool Open(const BYTE* image, uintptr_t virtualBase) {
        bytes = image;
        if (!bytes) return false;
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
            return false;
        nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            return false;
        runtimeBase = virtualBase ? virtualBase : reinterpret_cast<uintptr_t>(image);
        sections = IMAGE_FIRST_SECTION(nt);
        sectionCount = nt->FileHeader.NumberOfSections;
        return sectionCount > 0 && sectionCount < 96;
    }

    bool Range(const char* name, const BYTE*& begin, size_t& size) const {
        for (unsigned i = 0; i < sectionCount; ++i) {
            char sectionName[9] = {};
            memcpy(sectionName, sections[i].Name, 8);
            if (strcmp(sectionName, name)) continue;
            begin = bytes + sections[i].VirtualAddress;
            size = std::max(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData);
            if (sections[i].VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
                size > nt->OptionalHeader.SizeOfImage - sections[i].VirtualAddress) return false;
            return true;
        }
        return false;
    }

    bool ExecutableRange(const BYTE*& begin, size_t& size) const {
        begin = nullptr;
        size = 0;
        for (unsigned i = 0; i < sectionCount; ++i) {
            if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const size_t candidate = std::max(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData);
            if (!candidate || sections[i].VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
                candidate > nt->OptionalHeader.SizeOfImage - sections[i].VirtualAddress) continue;
            if (begin) continue;
            begin = bytes + sections[i].VirtualAddress;
            size = candidate;
        }
        return begin != nullptr && size != 0;
    }

    bool InImage(uintptr_t address, size_t size = 1) const {
        return address >= runtimeBase && size <= nt->OptionalHeader.SizeOfImage &&
            address - runtimeBase <= nt->OptionalHeader.SizeOfImage - size;
    }
};

struct RuntimeLayout {
    uintptr_t unitBase = 0;
    uintptr_t personPointerSlot = 0;
    unsigned capabilities = 0;
};

RuntimeLayout gLayout;
std::array<std::wstring, 255> gItemNames;
SRWLOCK gRuntimeLock = SRWLOCK_INIT;

bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    const bool validSize = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
        size.QuadPart <= 16 * 1024 * 1024;
    if (!validSize) {
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool okay = bytes.empty() ||
        (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
            read == bytes.size());
    CloseHandle(file);
    if (!okay) bytes.clear();
    return okay;
}

bool LoadItemTable(const std::wstring& path, size_t offset, size_t count, int firstId,
    std::array<std::wstring, 255>& names) {
    constexpr size_t kRowSize = 25;
    constexpr size_t kNameBytes = 17;
    std::vector<BYTE> bytes;
    if (!ReadFileBytes(path, bytes) || offset > bytes.size() ||
        count > (bytes.size() - offset) / kRowSize || firstId < 0 ||
        static_cast<size_t>(firstId) + count > names.size()) return false;
    size_t readable = 0;
    for (size_t index = 0; index < count; ++index) {
        const BYTE* raw = bytes.data() + offset + index * kRowSize;
        const std::wstring name = DecodeGameName(raw, kNameBytes);
        if (name.empty() || name.size() > 32) continue;
        bool valid = true;
        for (wchar_t ch : name) if (ch < 0x20) { valid = false; break; }
        if (!valid) continue;
        names[static_cast<size_t>(firstId) + index] = name;
        ++readable;
    }
    return readable >= count / 5;
}

bool LoadItemNames(const wchar_t* gameRoot, std::array<std::wstring, 255>& names) {
    names = {};
    if (!gameRoot || !*gameRoot) return false;
    const std::wstring root(gameRoot);
    const bool low = LoadItemTable(root + L"\\Data.e5", 33164, 104, 0, names);
    const bool high = LoadItemTable(root + L"\\Star.e5", 0, 151, 104, names);
    return low || high;
}

bool ReadMemory(uintptr_t address, void* output, size_t size) {
    if (!address || !output || !size) return false;
    bool ok = false;
    __try {
        memcpy(output, reinterpret_cast<const void*>(address), size);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

bool IsReadable(uintptr_t address, size_t size) {
    if (!address || !size) return false;
    MEMORY_BASIC_INFORMATION information = {};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &information, sizeof(information)) ||
        information.State != MEM_COMMIT || (information.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (information.Protect & readable) != 0 && address >= reinterpret_cast<uintptr_t>(information.BaseAddress) &&
        size <= information.RegionSize - (address - reinterpret_cast<uintptr_t>(information.BaseAddress));
}

bool ResolveLayout(const BYTE* image, uintptr_t virtualBase, RuntimeLayout& layout) {
    PeImage pe;
    if (!pe.Open(image, virtualBase)) return false;
    const BYTE* text = nullptr;
    size_t textSize = 0;
    if (!pe.ExecutableRange(text, textSize)) return false;

    uintptr_t unitBase = 0;
    uintptr_t personSlot = 0;
    size_t unitMatches = 0;
    size_t personMatches = 0;
    for (size_t i = 0; i + 20 <= textSize; ++i) {
        const BYTE* p = text + i;
        // push ebp; mov ebp,esp; movzx ecx,cl; imul ecx,ecx,30h; add ecx,imm32; pop ebp; ret
        if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x0F && p[4] == 0xB6 &&
            p[5] == 0xC9 && p[6] == 0x6B && p[7] == 0xC9 && p[8] == 0x30 &&
            p[9] == 0x81 && p[10] == 0xC1 && p[15] == 0x5D && p[16] == 0xC3) {
            uint32_t value = 0;
            memcpy(&value, p + 11, sizeof(value));
            if (pe.InImage(value, 0x30u * 250u)) {
                unitBase = value;
                ++unitMatches;
            }
        }
        // push ebp; mov ebp,esp; movzx ecx,word [ebp+8]; imul ecx,48h; add ecx,[slot]
        if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x0F && p[4] == 0xB7 &&
            p[5] == 0x4D && p[6] == 0x08 && p[7] == 0x6B && p[8] == 0xC9 && p[9] == 0x48 &&
            p[10] == 0x03 && p[11] == 0x0D && p[16] == 0x5D && p[17] == 0xC2 &&
            p[18] == 0x04 && p[19] == 0x00) {
            uint32_t value = 0;
            memcpy(&value, p + 12, sizeof(value));
            if (pe.InImage(value, sizeof(uintptr_t))) {
                personSlot = value;
                ++personMatches;
            }
        }
    }
    if (unitMatches != 1) return false;
    layout = RuntimeLayout();
    layout.unitBase = unitBase;
    layout.capabilities = BattleCapabilityUnits | BattleCapabilityStatuses | BattleCapabilityAi;
    if (personMatches == 1) {
        layout.personPointerSlot = personSlot;
        layout.capabilities |= BattleCapabilityPeople | BattleCapabilityEquipment;
    }
    return true;
}

std::wstring DecodeName(const BYTE* bytes, size_t length) {
    size_t used = 0;
    while (used < length && bytes[used]) ++used;
    while (used && bytes[used - 1] == ' ') --used;
    if (!used) return {};
    std::wstring result = DecodeGameName(bytes, used);
    if (result.empty() || result.size() > 32) return {};
    for (wchar_t ch : result) if (ch < 0x20) return {};
    return result;
}

bool ReadPerson(uintptr_t table, const std::array<std::wstring, 255>& itemNames,
    BattleUnitSnapshot& unit) {
    if (unit.dataId < 0 || unit.dataId >= 1024) return false;
    BYTE person[0x48] = {};
    const uintptr_t address = table + static_cast<uintptr_t>(unit.dataId) * sizeof(person);
    if (!IsReadable(address, sizeof(person)) || !ReadMemory(address, person, sizeof(person))) return false;
    unit.name = DecodeName(person + 8, 8);
    unit.level = person[0x2C];
    unit.weaponId = person[0x2E];
    unit.hasWeapon = unit.weaponId != 0xFF;
    if (!unit.hasWeapon) unit.weaponId = -1;
    unit.weaponLevel = unit.hasWeapon ? person[0x2F] : -1;
    unit.armorId = person[0x31];
    unit.hasArmor = unit.armorId != 0xFF;
    if (!unit.hasArmor) unit.armorId = -1;
    unit.armorLevel = unit.hasArmor ? person[0x32] : -1;
    unit.accessoryId = person[0x34];
    unit.hasAccessory = unit.accessoryId != 0xFF;
    if (!unit.hasAccessory) unit.accessoryId = -1;
    if (unit.weaponId >= 0 && unit.weaponId < static_cast<int>(itemNames.size()))
        unit.weaponName = itemNames[static_cast<size_t>(unit.weaponId)];
    if (unit.armorId >= 0 && unit.armorId < static_cast<int>(itemNames.size()))
        unit.armorName = itemNames[static_cast<size_t>(unit.armorId)];
    if (unit.accessoryId >= 0 && unit.accessoryId < static_cast<int>(itemNames.size()))
        unit.accessoryName = itemNames[static_cast<size_t>(unit.accessoryId)];
    return !unit.name.empty() || unit.level <= 99;
}
}

bool InitializeBattleRuntime(const wchar_t* gameRoot) {
    RuntimeLayout layout;
    std::array<std::wstring, 255> itemNames;
    LoadItemNames(gameRoot, itemNames);
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module || !ResolveLayout(reinterpret_cast<const BYTE*>(module),
        reinterpret_cast<uintptr_t>(module), layout)) return false;
    AcquireSRWLockExclusive(&gRuntimeLock);
    gLayout = layout;
    gItemNames = std::move(itemNames);
    ReleaseSRWLockExclusive(&gRuntimeLock);
    return true;
}

void ResetBattleRuntime() {
    AcquireSRWLockExclusive(&gRuntimeLock);
    gLayout = RuntimeLayout();
    gItemNames = {};
    ReleaseSRWLockExclusive(&gRuntimeLock);
}

unsigned BattleRuntimeCapabilities() {
    AcquireSRWLockShared(&gRuntimeLock);
    const unsigned result = gLayout.capabilities;
    ReleaseSRWLockShared(&gRuntimeLock);
    return result;
}

bool ReadBattleSnapshot(BattleSnapshot& snapshot) {
    RuntimeLayout layout;
    std::array<std::wstring, 255> itemNames;
    AcquireSRWLockShared(&gRuntimeLock);
    layout = gLayout;
    itemNames = gItemNames;
    ReleaseSRWLockShared(&gRuntimeLock);
    snapshot = BattleSnapshot();
    if (!(layout.capabilities & BattleCapabilityUnits) ||
        !IsReadable(layout.unitBase, 0x30u * 250u)) return false;

    uintptr_t people = 0;
    bool peopleReady = layout.personPointerSlot && ReadMemory(layout.personPointerSlot, &people, sizeof(people)) &&
        people && IsReadable(people, 0x48);
    snapshot.capabilities = layout.capabilities;
    if (!peopleReady) snapshot.capabilities &= ~(BattleCapabilityPeople | BattleCapabilityEquipment);
    snapshot.capturedAt = GetTickCount64();
    snapshot.units.reserve(64);
    for (int slot = 0; slot < 250; ++slot) {
        BYTE record[0x30] = {};
        if (!ReadMemory(layout.unitBase + static_cast<uintptr_t>(slot) * sizeof(record), record, sizeof(record)))
            return false;
        uint16_t dataId = 0;
        memcpy(&dataId, record, sizeof(dataId));
        if (dataId == 0xFFFF) continue;
        const int camp = record[5];
        const int visibility = record[0x0C];
        int hp = 0, mp = 0;
        memcpy(&hp, record + 0x10, sizeof(hp));
        memcpy(&mp, record + 0x14, sizeof(mp));
        // The native 6.6 HP setter clamps at 9999.  Larger values here are
        // stale pre-battle bytes that happen to resemble TacticalUnit rows.
        if (camp > 15 || visibility > 7 || hp < -1 || hp > 9999 || mp < -1 || mp > 9999)
            continue;
        BattleUnitSnapshot unit;
        unit.slot = slot;
        unit.dataId = dataId;
        unit.camp = camp;
        unit.x = record[6];
        unit.y = record[7];
        unit.targetSlot = record[8] == 0xFF ? -1 : record[8];
        unit.targetX = record[9];
        unit.targetY = record[0x0A];
        unit.actionOrder = record[0x0B];
        unit.visibility = visibility;
        unit.actionState = record[0x0D];
        unit.aiPolicy = record[0x0E];
        unit.direction = record[0x0F];
        unit.hp = hp;
        unit.mp = mp;
        unit.status = record[0x1E];
        if (peopleReady) ReadPerson(people, itemNames, unit);
        snapshot.units.push_back(std::move(unit));
    }
    return true;
}

std::wstring BattleCampName(int camp) {
    if (camp == 0) return L"我军";
    if (camp == 1) return L"友军";
    return L"敌军";
}

std::wstring BattleAiName(int policy) {
    switch (policy) {
    case 0: return L"指定目标";
    case 1: return L"主动攻击";
    case 2: return L"坚守";
    case 3: return L"被动攻击";
    case 4: return L"跟随或撤离";
    case 7: return L"玩家控制";
    default: return {};
    }
}

std::wstring BattleStatusText(int status) {
    struct State { int shift; const wchar_t* name; } states[] = {
        {0, L"麻痹"}, {2, L"禁咒"}, {4, L"混乱"}, {6, L"中毒"}
    };
    std::wstring result;
    for (const State& state : states) {
        const int turns = (status >> state.shift) & 3;
        if (!turns) continue;
        if (!result.empty()) result += L"、";
        result += state.name;
        result += L"（" + std::to_wstring(turns) + L"）";
    }
    return result;
}

std::wstring BattleUnitLabel(const BattleUnitSnapshot& unit) {
    if (!unit.name.empty()) return unit.name;
    return BattleCampName(unit.camp) + L"单位（初始槽位 " + std::to_wstring(unit.slot + 1) + L"）";
}

bool ResolveBattleRuntimeImageForTest(const BYTE* image, uintptr_t virtualBase,
    uintptr_t& unitBase, uintptr_t& personPointerSlot) {
    RuntimeLayout layout;
    if (!ResolveLayout(image, virtualBase, layout)) return false;
    unitBase = layout.unitBase;
    personPointerSlot = layout.personPointerSlot;
    return true;
}

bool LoadBattleItemNameForTest(const wchar_t* gameRoot, int itemId, std::wstring& name) {
    name.clear();
    if (itemId < 0 || itemId >= 255) return false;
    std::array<std::wstring, 255> names;
    if (!LoadItemNames(gameRoot, names)) return false;
    name = names[static_cast<size_t>(itemId)];
    return !name.empty();
}
