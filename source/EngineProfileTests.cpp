#include "Patch.h"
#include "EngineProfile.h"
#include "EngineLayout.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
bool Load(const wchar_t* path, std::vector<BYTE>& image, DWORD& base) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb")) return false;
    _fseeki64(file, 0, SEEK_END);
    const auto length = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    if (length < 256 || length > 64 * 1024 * 1024) { fclose(file); return false; }
    std::vector<BYTE> raw(static_cast<size_t>(length));
    const bool read = fread(raw.data(), 1, raw.size(), file) == raw.size();
    fclose(file);
    if (!read) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data() + dos->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage > 64 * 1024 * 1024) return false;
    image.assign(nt->OptionalHeader.SizeOfImage, 0);
    memcpy(image.data(), raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        memcpy(image.data() + sections[i].VirtualAddress, raw.data() + sections[i].PointerToRawData, sections[i].SizeOfRawData);
    base = nt->OptionalHeader.ImageBase;
    return true;
}
IMAGE_NT_HEADERS32* Nt(std::vector<BYTE>& image) {
    return reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data() + reinterpret_cast<IMAGE_DOS_HEADER*>(image.data())->e_lfanew);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    for (int index = 1; index <= 2; ++index) {
        const EngineProfileId expected = index == 1 ? EngineProfileId::Ccz65 : EngineProfileId::Ccz66;
        const std::wstring path = argv[index];
        const std::wstring root = path.substr(0, path.find_last_of(L"\\/"));
        EngineDetection detection;
        std::wstring error;
        Check(DetectEngineProfile(root, detection, error) && detection.profile == expected &&
            detection.exeSha256.size() == 64, "detect engine from code and resource, with SHA-256");
        std::vector<BYTE> image;
        DWORD base = 0;
        if (!Load(argv[index], image, base)) { Check(false, "load fixture"); continue; }
        const auto original = image;
        Check(DetectEngineProfileImage(image.data(), image.size(), base, detection, error) &&
            detection.profile == expected, "mapped image agrees with on-disk detection");
        auto& resources = Nt(image)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        resources.VirtualAddress = resources.Size = 0;
        Check(DetectEngineProfileImage(image.data(), image.size(), base, detection, error) &&
            detection.profile == expected, "missing version resource uses unique semantic call graph");
        image = original;
        const auto resource = Nt(image)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        bool changedVersion = false;
        for (size_t at = resource.VirtualAddress; at + sizeof(VS_FIXEDFILEINFO) <= resource.VirtualAddress + resource.Size; at += 4) {
            auto info = reinterpret_cast<VS_FIXEDFILEINFO*>(image.data() + at);
            if (info->dwSignature == VS_FFI_SIGNATURE && info->dwStrucVersion == VS_FFI_STRUCVERSION) {
                info->dwFileVersionLS = (info->dwFileVersionLS & 0xffff0000u) | (index == 1 ? 6u : 5u);
                changedVersion = true;
            }
        }
        Check(changedVersion && !DetectEngineProfileImage(image.data(), image.size(), base, detection, error),
            "conflicting version resource cannot select a payload");
        for (size_t at = resource.VirtualAddress; at + sizeof(VS_FIXEDFILEINFO) <= resource.VirtualAddress + resource.Size; at += 4) {
            auto info = reinterpret_cast<VS_FIXEDFILEINFO*>(image.data() + at);
            if (info->dwSignature == VS_FFI_SIGNATURE && info->dwStrucVersion == VS_FFI_STRUCVERSION)
                info->dwFileVersionLS = (info->dwFileVersionLS & 0xffff0000u) | 7u;
        }
        Check(!DetectEngineProfileImage(image.data(), image.size(), base, detection, error),
            "future generation resource conflicts with an older layout");
        image = original;
        EngineLayout layout;
        Check(ResolveEngineLayout(image.data(), layout, base), "resolve fixture identity graph");
        if (layout.waitWrapper) image[layout.waitWrapper - 0x400000] = 0x90;
        Check(!DetectEngineProfileImage(image.data(), image.size(), base, detection, error),
            "partial feature matches cannot masquerade as engine identity");
        image = original;
        Nt(image)->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        Check(!DetectEngineProfileImage(image.data(), image.size(), base, detection, error), "reject non-x86 target");
        image = original;
        Nt(image)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress = 0xfffffff0;
        Check(!DetectEngineProfileImage(image.data(), image.size(), base, detection, error), "reject out-of-range resource directory");
        Check(!DetectEngineProfileImage(image.data(), 64, base, detection, error), "reject truncated PE headers");
    }
    printf("EngineProfileTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
