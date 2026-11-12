#include "BattleRuntime.h"
#include "EngineProfile.h"

#include <stdio.h>
#include <vector>

namespace {
int failures = 0;
void Check(bool condition, const char* name) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
bool LoadImage(const wchar_t* path, std::vector<BYTE>& image, DWORD& base) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb")) return false;
    _fseeki64(file, 0, SEEK_END);
    const __int64 length = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    if (length < 256 || length > 64 * 1024 * 1024) { fclose(file); return false; }
    std::vector<BYTE> raw(static_cast<size_t>(length));
    const bool read = fread(raw.data(), 1, raw.size(), file) == raw.size();
    fclose(file);
    if (!read) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage > 64 * 1024 * 1024 ||
        nt->OptionalHeader.SizeOfHeaders > raw.size()) return false;
    image.assign(nt->OptionalHeader.SizeOfImage, 0);
    memcpy(image.data(), raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        if (section.PointerToRawData + static_cast<size_t>(section.SizeOfRawData) > raw.size() ||
            section.VirtualAddress + static_cast<size_t>(section.SizeOfRawData) > image.size()) return false;
        memcpy(image.data() + section.VirtualAddress, raw.data() + section.PointerToRawData,
            section.SizeOfRawData);
    }
    base = nt->OptionalHeader.ImageBase;
    return true;
}

bool LoadRuntimeImage(const wchar_t* path, std::vector<BYTE>& image, DWORD& base) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb")) return false;
    _fseeki64(file, 0, SEEK_END);
    const __int64 length = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    if (length < 4096 || length > 64 * 1024 * 1024) { fclose(file); return false; }
    image.resize(static_cast<size_t>(length));
    const bool read = fread(image.data(), 1, image.size(), file) == image.size();
    fclose(file);
    if (!read) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) > image.size() - sizeof(IMAGE_NT_HEADERS32)) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.SizeOfImage != image.size()) return false;
    base = nt->OptionalHeader.ImageBase;
    return true;
}

void CheckRuntimeSnapshots() {
    std::vector<std::wstring> people, items;
    ResetBattleRuntime();
    Check(!ReadBattleResourceNames(people, items) && people.empty() && items.empty(),
        "unconfigured runtime never supplies resource names");
    ConfigureBattleRuntimeResourceNames(true);
    Check(!ReadBattleResourceNames(people, items), "enabled but uninitialized runtime stays unavailable");
    ResetBattleRuntime();
    BYTE* memory = static_cast<BYTE*>(VirtualAlloc(nullptr, 0x15000,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(memory != nullptr, "allocate independent name-table fixture");
    if (!memory) return;
    uintptr_t table = reinterpret_cast<uintptr_t>(memory);
    BYTE* itemTable = memory + 0x12000;
    for (unsigned i = 0; i < 1024; ++i) {
        const uint16_t tag = static_cast<uint16_t>(0x1000 + i);
        memcpy(memory + i * 0x48, &tag, sizeof(tag));
    }
    const BYTE personName[] = {0xB2,0xDC,0xB2,0xD9};
    const BYTE itemName[] = {0xB6,0xCC,0xBD,0xA3};
    memcpy(memory + 8, personName, sizeof(personName));
    memcpy(memory + 1023 * 0x48 + 8, "Last", 4);
    memcpy(itemTable, itemName, sizeof(itemName));
    memcpy(itemTable + 254 * 25, "Final item", 10);
    Check(ReadBattleResourceNamesForTest(reinterpret_cast<uintptr_t>(&table),
        reinterpret_cast<uintptr_t>(itemTable), people, items) && people.size() == 1024 &&
        items.size() == 255 && people[0] == L"曹操" && people[1023] == L"Last" &&
        items[0] == L"短剑" && items[254] == L"Final item" && items[1].empty(),
        "copy loaded GBK names without disk data");
    ConfigureBattleResourceLayoutForTest(reinterpret_cast<uintptr_t>(&table), reinterpret_cast<uintptr_t>(itemTable));
    BattleResourceSnapshot before;
    Check(ReadBattleResourceSnapshot(before) && before.peopleLayoutReady && before.itemsLayoutReady &&
        before.peopleReady && before.itemsReady && before.peopleOrigin == BattleResourceOrigin::Runtime &&
        before.itemOrigin == BattleResourceOrigin::Runtime && before.processId == GetCurrentProcessId() &&
        before.processCreatedAt != 0 && IsBattleResourceSnapshotCurrent(before),
        "public resource snapshot records independent readiness, origin and stable process identity");
    memory[8] = 'X'; itemTable[0] = 'Y';
    Check(people[0] == L"曹操" && items[0] == L"短剑",
        "returned snapshot owns strings independently of native memory");
    Check(!IsBattleResourceSnapshotCurrent(before) && before.people[0] == L"曹操" && before.items[0] == L"短剑",
        "name changes invalidate the generation without changing a previously returned snapshot");
    const uintptr_t savedTable = table;
    table = 0;
    BattleResourceSnapshot partial;
    Check(ReadBattleResourceSnapshot(partial) && !partial.peopleReady && partial.itemsReady &&
        partial.peopleLayoutReady && partial.people.empty() && partial.items[254] == L"Final item",
        "a not-yet-loaded person pointer does not hide the independently ready item table");
    table = savedTable;
    DWORD oldProtection = 0, ignored = 0;
    Check(VirtualProtect(itemTable + 0x1000, 0x1000, PAGE_NOACCESS, &oldProtection) != FALSE,
        "protect an interior page of the item table");
    BattleResourceSnapshot hole;
    Check(ReadBattleResourceSnapshot(hole) && hole.peopleReady && !hole.itemsReady && hole.items.empty(),
        "an unreadable item-table hole preserves people and rejects the entire affected table");
    VirtualProtect(itemTable + 0x1000, 0x1000, oldProtection, &ignored);
    BattleResourceSnapshot resumed;
    Check(ReadBattleResourceSnapshot(resumed) && resumed.peopleReady && resumed.itemsReady &&
        resumed.resourceGeneration != partial.resourceGeneration,
        "table readiness recovers after pages are restored and advances the resource generation");
    ConfigureBattleResourceLayoutForTest(reinterpret_cast<uintptr_t>(&table), reinterpret_cast<uintptr_t>(itemTable));
    Check(!IsBattleResourceSnapshotCurrent(resumed), "reinitialization invalidates identical table addresses from the old session");
    memset(memory + 8, 0, 8); memory[8] = 0x81;
    memset(itemTable, 0, 17); itemTable[0] = 1;
    Check(ReadBattleResourceNamesForTest(reinterpret_cast<uintptr_t>(&table),
        reinterpret_cast<uintptr_t>(itemTable), people, items) && people[0].empty() && items[0].empty(),
        "malformed runtime names remain unresolved");
    VirtualFree(memory, 0, MEM_RELEASE);
    Check(!ReadBattleResourceNamesForTest(reinterpret_cast<uintptr_t>(&table),
        reinterpret_cast<uintptr_t>(itemTable), people, items) && people.empty() && items.empty(),
        "freed native tables cannot yield stale snapshots");
    BattleResourceSnapshot freed;
    Check(!ReadBattleResourceSnapshot(freed) && !freed.peopleReady && !freed.itemsReady &&
        !IsBattleResourceSnapshotCurrent(before), "freed tables invalidate public snapshots and never return cached names");
    ResetBattleRuntime();
}
}

int wmain(int argc, wchar_t** argv) {
    const bool runtime = argc == 4 && !_wcsicmp(argv[1], L"--runtime-rva");
    if (argc != 3 && !runtime) return 2;
    std::vector<BYTE> image;
    DWORD base = 0;
    Check(runtime ? LoadRuntimeImage(argv[2], image, base) : LoadImage(argv[1], image, base),
        "load x86 game image");
    uintptr_t units = 0, people = 0;
    Check(ResolveBattleRuntimeImageForTest(image.data(), base, units, people),
        "resolve unit/person runtime anchors");
    Check(units == 0x4A7B20 && people == 0x4CEA00,
        "resolve the 6.6 ShanZei unit array and person pointer slot");
    CheckRuntimeSnapshots();
    std::wstring itemName;
    std::vector<std::wstring> peopleNames, itemNames;
    if (runtime) {
        uintptr_t itemBase = 0;
        Check(ResolveBattleItemTableForTest(image.data(), base, itemBase) && itemBase == 0x4A1140,
            "resolve the protected runtime item table");
        Check(!LoadBattleItemNameForTest(argv[3], 0, itemName),
            "physical Data.e5 absence does not fabricate low item names");
        Check(ReadBattleResourceNamesForTest(0, reinterpret_cast<uintptr_t>(image.data() + 0xA1140),
            peopleNames, itemNames) && itemNames.size() == 255 && itemNames[0] == L"短剑" &&
            itemNames[104] == L"虎豹嘶风铠", "runtime rows supply names without physical Data.e5");
    }
    const bool engine64 = CompiledEngineProfile() == EngineProfileId::Ccz64;
    const bool engine65 = CompiledEngineProfile() == EngineProfileId::Ccz65;
    const wchar_t* gameRoot = runtime ? argv[3] : argv[2];
    if (!runtime) Check(LoadBattleItemNameForTest(gameRoot, 0, itemName) && itemName ==
        (engine64 ? L"饰剑" : engine65 ? L"短剑" : L"铁剑"),
        "read the current MOD's low item table as Unicode");
    Check(LoadBattleItemNameForTest(gameRoot, 104, itemName) && itemName ==
        (engine64 ? L"68护具" : engine65 ? L"虎豹嘶风铠" : L"昭武明光铠"),
        "read the current MOD's high item table as Unicode");
    if (!runtime && !engine64 && !engine65) Check(LoadBattleItemNameForTest(gameRoot, 30, itemName) && itemName == L"古今双绝" &&
        LoadBattleItemNameForTest(gameRoot, 71, itemName) && itemName == L"鱼鳞铠",
        "resolve live ShanZei equipment IDs to their actual weapon and armor names");
    printf("BattleRuntimeTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
