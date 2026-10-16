#include "BattleRuntime.h"

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
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    std::vector<BYTE> image;
    DWORD base = 0;
    Check(LoadImage(argv[1], image, base), "load x86 game image");
    uintptr_t units = 0, people = 0;
    Check(ResolveBattleRuntimeImageForTest(image.data(), base, units, people),
        "resolve unit/person runtime anchors");
    Check(units == 0x4A7B20 && people == 0x4CEA00,
        "resolve the 6.6 ShanZei unit array and person pointer slot");
    std::wstring itemName;
    Check(LoadBattleItemNameForTest(argv[2], 0, itemName) && itemName == L"铁剑",
        "read the current MOD's low item table as Unicode");
    Check(LoadBattleItemNameForTest(argv[2], 104, itemName) && itemName == L"昭武明光铠",
        "read the current MOD's high item table as Unicode");
    Check(LoadBattleItemNameForTest(argv[2], 30, itemName) && itemName == L"古今双绝" &&
        LoadBattleItemNameForTest(argv[2], 71, itemName) && itemName == L"鱼鳞铠",
        "resolve live ShanZei equipment IDs to their actual weapon and armor names");
    printf("BattleRuntimeTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
