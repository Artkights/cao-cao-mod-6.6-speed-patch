#include "PeImage.h"
#include <winioctl.h>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
std::vector<BYTE> Fixture(bool zeroVirtualSize = false) {
    std::vector<BYTE> file(0x600, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(file.data() + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE; nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 1; nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC; nt->OptionalHeader.ImageBase = 0x400000;
    nt->OptionalHeader.SizeOfImage = 0x3000; nt->OptionalHeader.SizeOfHeaders = 0x400;
    auto* section = IMAGE_FIRST_SECTION(nt);
    memcpy(section->Name, ".text", 5); section->VirtualAddress = 0x1000;
    section->Misc.VirtualSize = zeroVirtualSize ? 0 : 0x180;
    section->SizeOfRawData = 0x200; section->PointerToRawData = 0x400;
    section->Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
    for (size_t i = 0x400; i < file.size(); ++i) file[i] = static_cast<BYTE>(i);
    return file;
}
void Put(const std::wstring& path, const std::vector<BYTE>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0; WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr); CloseHandle(file);
}
}

int wmain() {
    std::wstring error;
    auto fileBytes = Fixture();
    PeImageMetadata metadata;
    Check(ParsePeImageMetadata(fileBytes.data(), 0x400, fileBytes.size(), true, metadata, error) &&
        metadata.sections.size() == 1 && PeLogicalSectionSize(metadata.sections[0]) == 0x180,
        "parse standard PE headers and virtual section extent");
    auto zero = Fixture(true);
    Check(ParsePeImageMetadata(zero.data(), 0x400, zero.size(), true, metadata, error) &&
        PeLogicalSectionSize(metadata.sections[0]) == 0x200,
        "zero VirtualSize falls back to SizeOfRawData");
    auto overlap = Fixture();
    auto* overlapNt = reinterpret_cast<IMAGE_NT_HEADERS32*>(overlap.data() + 0x80);
    overlapNt->FileHeader.NumberOfSections = 2;
    auto* sections = IMAGE_FIRST_SECTION(overlapNt);
    sections[1] = sections[0]; sections[1].VirtualAddress = 0x1100;
    Check(!ParsePeImageMetadata(overlap.data(), 0x400, overlap.size(), true, metadata, error),
        "reject overlapping virtual sections");
    auto overflow = Fixture();
    auto* overflowSection = IMAGE_FIRST_SECTION(reinterpret_cast<IMAGE_NT_HEADERS32*>(overflow.data() + 0x80));
    overflowSection->VirtualAddress = 0x2FFF; overflowSection->Misc.VirtualSize = 2;
    Check(!ParsePeImageMetadata(overflow.data(), 0x400, overflow.size(), true, metadata, error),
        "reject virtual section overflow");
    auto rawOverflow = Fixture();
    auto* rawSection = IMAGE_FIRST_SECTION(reinterpret_cast<IMAGE_NT_HEADERS32*>(rawOverflow.data() + 0x80));
    rawSection->PointerToRawData = 0x500; rawSection->SizeOfRawData = 0x200;
    Check(!ParsePeImageMetadata(rawOverflow.data(), 0x400, rawOverflow.size(), true, metadata, error),
        "reject file raw section overflow");
    auto notExecutable = Fixture();
    reinterpret_cast<IMAGE_NT_HEADERS32*>(notExecutable.data() + 0x80)->FileHeader.Characteristics = 0;
    Check(!ParsePeImageMetadata(notExecutable.data(), 0x400, notExecutable.size(), true, metadata, error),
        "reject PE headers that do not declare an executable image");

    wchar_t temp[MAX_PATH] = {};
    wchar_t path[MAX_PATH] = {};
    const bool tempReady = GetTempPathW(MAX_PATH, temp) && GetTempFileNameW(temp, L"pea", 0, path);
    if (tempReady) {
        Put(path, fileBytes);
        PeFileView source; PeAnalysisView mapped;
        const bool opened = source.Open(path, error);
        Check(opened && source.Size() == fileBytes.size(), "open 64-bit-size streaming PE file view");
        Check(opened && mapped.Map(source, error) && mapped.Size() == 0x3000 &&
            memcmp(mapped.Data() + 0x1000, fileBytes.data() + 0x400, 0x180) == 0 &&
            mapped.Data()[0x1180] == 0,
            "map bounded virtual bytes and leave section tail zero-filled");
        std::wstring hash, hashError;
        Check(opened && source.HashSha256(hash, hashError) && hash.size() == 64,
            "stream SHA-256 over file without allocating whole artifact");
        BYTE tail[2] = {};
        Check(opened && source.Read(source.Size() - sizeof(tail), tail, sizeof(tail), error) &&
            !source.Read(source.Size() - 1, tail, sizeof(tail), error),
            "bounded random reads reject ranges beyond file size");
        HANDLE blocked = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(opened && blocked == INVALID_HANDLE_VALUE,
            "open read handle prevents concurrent file writes while validating");
        if (blocked != INVALID_HANDLE_VALUE) CloseHandle(blocked);
        source.Close();
        const std::vector<BYTE> abc = {'a','b','c'};
        Put(path, abc);
        Check(Sha256FileStream(path, hash, error) &&
            hash == L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
            "streaming SHA-256 matches the standard abc test vector");
        auto packed = Fixture(); packed.resize(0x200400, 0xA5);
        auto* packedSection = IMAGE_FIRST_SECTION(reinterpret_cast<IMAGE_NT_HEADERS32*>(packed.data() + 0x80));
        packedSection->Misc.VirtualSize = 0x1000; packedSection->SizeOfRawData = 0x200000;
        Put(path, packed);
        Check(source.Open(path, error) && mapped.Map(source, error) && mapped.Size() == 0x3000 &&
            mapped.Data()[0x1FFF] == 0xA5 && mapped.Data()[0x2000] == 0,
            "packed raw storage larger than virtual span is bounded without copying its container tail");
        source.Close();
        HANDLE sparse = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD returned = 0, written = 0;
        LARGE_INTEGER beyond32 = {}; beyond32.QuadPart = 0x100000100ll;
        const BYTE marker = 0x5A;
        const bool sparseReady = sparse != INVALID_HANDLE_VALUE &&
            DeviceIoControl(sparse, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) &&
            SetFilePointerEx(sparse, beyond32, nullptr, FILE_BEGIN) &&
            WriteFile(sparse, &marker, sizeof(marker), &written, nullptr) && written == sizeof(marker);
        if (sparse != INVALID_HANDLE_VALUE) CloseHandle(sparse);
        BYTE observed = 0;
        Check(sparseReady && source.Open(path, error) && source.Size() == 0x100000101ull &&
            source.Read(0x100000100ull, &observed, sizeof(observed), error) && observed == marker &&
            !source.Read(0x100000101ull, &observed, sizeof(observed), error) &&
            mapped.Map(source, error) && mapped.Size() == 0x3000,
            "sparse file beyond 4 GiB supports 64-bit bounded reads without growing the analysis image");
        source.Close();
        DeleteFileW(path);
    } else Check(false, "create temporary file for streaming view tests");
    BYTE* runtime = static_cast<BYTE*>(VirtualAlloc(nullptr, 0x3000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (runtime) {
        const auto raw = Fixture();
        memcpy(runtime, raw.data(), 0x400); memcpy(runtime + 0x1000, raw.data() + 0x400, 0x180);
        DWORD previous = 0;
        Check(VirtualProtect(runtime + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous) != FALSE,
            "prepare executable runtime page");
        Check(VirtualProtect(runtime + 0x2000, 0x1000, PAGE_NOACCESS, &previous) != FALSE,
            "prepare inaccessible runtime page");
        RuntimeImageView view;
        Check(view.Capture(runtime, 0x3000, error) && view.Size() == 0x3000 &&
            view.IsReadable(0, 0x400) && view.IsExecutable(0x1000, 0x180) &&
            !view.IsReadable(0x2000, 1) && view.Data()[0x2000] == 0,
            "runtime capture records actual executable pages and zero-fills unreadable holes");
        BYTE one = 0;
        Check(!ReadRuntimeMemory(runtime + 0x1FFF, &one, 2, error),
            "runtime read spanning an inaccessible boundary fails before copying");
        Check(VirtualProtect(runtime + 0x1000, 0x1000, PAGE_READWRITE | PAGE_GUARD, &previous) != FALSE &&
            view.Capture(runtime, 0x3000, error) && !view.IsReadable(0x1000, 1) &&
            !view.IsExecutable(0x1000, 1),
            "guard pages are skipped without triggering the guard");
        Check(VirtualFree(runtime + 0x2000, 0x1000, MEM_DECOMMIT) != FALSE &&
            view.Capture(runtime, 0x3000, error) && !view.IsReadable(0x2000, 1),
            "uncommitted runtime pages remain unavailable in captured view");
        // A PE-looking buffer in an existing allocation is not an HMODULE.
        Check(VirtualProtect(runtime + 0x1000, 0x1000, PAGE_READWRITE, &previous) != FALSE,
            "prepare interior runtime header fixture");
        memcpy(runtime + 0x1000, raw.data(), 0x400);
        Check(!view.Capture(runtime + 0x1000, 0x3000, error),
            "runtime capture requires the original image allocation base");
        VirtualFree(runtime, 0, MEM_RELEASE);
    } else Check(false, "allocate runtime page fixture");
    printf("PeImageTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
