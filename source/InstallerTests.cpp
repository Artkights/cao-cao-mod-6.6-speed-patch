#include "InstallCommon.h"
#include "ResourceIds.h"

#include <stdio.h>
#include <cstring>

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

bool SameHash(const std::vector<unsigned char>& bytes, const wchar_t* path) {
    std::wstring left, right, error;
    return XiaoAnInstall::Sha256Bytes(bytes, left, error) &&
           XiaoAnInstall::Sha256File(path, right, error) && !_wcsicmp(left.c_str(), right.c_str());
}

struct GearExports {
    IMAGE_EXPORT_DIRECTORY* directory = nullptr;
    DWORD* names = nullptr;
    WORD* ordinals = nullptr;
    DWORD* functions = nullptr;
    IMAGE_DATA_DIRECTORY range = {};
    size_t nameOffsets[6] = {};
};

bool InspectGear(std::vector<unsigned char>& bytes, GearExports& output) {
    output = {};
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    if (dos->e_lfanew <= 0 || static_cast<size_t>(dos->e_lfanew) > bytes.size() - sizeof(IMAGE_NT_HEADERS32)) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes.data() + dos->e_lfanew);
    auto raw = [&](DWORD rva, size_t length) -> unsigned char* {
        if (rva < nt->OptionalHeader.SizeOfHeaders && rva <= bytes.size() && length <= bytes.size() - rva)
            return bytes.data() + rva;
        const auto sections = IMAGE_FIRST_SECTION(nt);
        for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
            const auto& section = sections[index];
            if (rva < section.VirtualAddress || rva - section.VirtualAddress >= section.SizeOfRawData) continue;
            const size_t offset = static_cast<size_t>(section.PointerToRawData) + rva - section.VirtualAddress;
            if (offset <= bytes.size() && length <= bytes.size() - offset) return bytes.data() + offset;
        }
        return nullptr;
    };
    output.range = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    output.directory = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(raw(output.range.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)));
    if (!output.directory || output.directory->NumberOfNames != 6 || output.directory->NumberOfFunctions != 6) return false;
    output.names = reinterpret_cast<DWORD*>(raw(output.directory->AddressOfNames, 6 * sizeof(DWORD)));
    output.ordinals = reinterpret_cast<WORD*>(raw(output.directory->AddressOfNameOrdinals, 6 * sizeof(WORD)));
    output.functions = reinterpret_cast<DWORD*>(raw(output.directory->AddressOfFunctions, 6 * sizeof(DWORD)));
    if (!output.names || !output.ordinals || !output.functions) return false;
    for (size_t index = 0; index < 6; ++index) {
        const auto name = raw(output.names[index], 1);
        if (!name || !memchr(name, 0, bytes.size() - static_cast<size_t>(name - bytes.data()))) return false;
        output.nameOffsets[index] = static_cast<size_t>(name - bytes.data());
    }
    return true;
}

void CheckGearContract(std::vector<unsigned char> bytes) {
    GearExports table;
    Check(InspectGear(bytes, table), "inspect the six-export Gear fixture");
    if (!table.directory) return;
    bool configureAtSix = false;
    size_t configureIndex = 0;
    for (size_t index = 0; index < 6; ++index)
        if (!strcmp(reinterpret_cast<const char*>(bytes.data() + table.nameOffsets[index]), "ConfigureNativeImports")) {
            configureAtSix = table.ordinals[index] == 5 && table.directory->Base == 1;
            configureIndex = index;
        }
    Check(configureAtSix, "current Gear exposes ConfigureNativeImports at ordinal 6");
    const auto current = bytes;
    std::wstring error;
    Check(XiaoAnInstall::ValidateGearPayloadBytes(bytes, error), "current payload requires the verified six-export contract");

    // Model the historical artifact by removing only the new named export and
    // truncating the function array to ordinals 1..5. The original code and
    // lookup layout remain those of an actual compiled Gear DLL.
    for (size_t index = configureIndex; index + 1 < 6; ++index) {
        table.names[index] = table.names[index + 1];
        table.ordinals[index] = table.ordinals[index + 1];
    }
    table.directory->NumberOfFunctions = table.directory->NumberOfNames = 5;
    Check(XiaoAnInstall::ValidateGearBytes(bytes, error), "historical five-export Gear remains valid for legacy recognition");
    Check(!XiaoAnInstall::ValidateGearPayloadBytes(bytes, error), "historical Gear cannot be embedded as a current payload");

    bytes = current; InspectGear(bytes, table);
    bytes[table.nameOffsets[configureIndex]] = 'X';
    Check(!XiaoAnInstall::ValidateGearPayloadBytes(bytes, error), "reject renamed or missing ConfigureNativeImports");
    bytes = current; InspectGear(bytes, table);
    table.ordinals[configureIndex] = 0;
    Check(!XiaoAnInstall::ValidateGearPayloadBytes(bytes, error), "reject a wrong ConfigureNativeImports ordinal");
    bytes = current; InspectGear(bytes, table);
    table.directory->NumberOfNames = 5;
    Check(!XiaoAnInstall::ValidateGearPayloadBytes(bytes, error), "reject incomplete export counts");
    bytes = current; InspectGear(bytes, table);
    table.functions[5] = table.range.VirtualAddress;
    Check(!XiaoAnInstall::ValidateGearPayloadBytes(bytes, error), "reject a forwarded Gear export");
    bytes = current; InspectGear(bytes, table);
    table.functions[5] = 0;
    Check(!XiaoAnInstall::ValidateGearPayloadBytes(bytes, error), "reject a null Gear export");
    bytes = current; InspectGear(bytes, table);
    table.directory->AddressOfFunctions = 0xfffffff0;
    Check(!XiaoAnInstall::ValidateGearPayloadBytes(bytes, error), "reject an out-of-bounds export function array");
    bytes = current; InspectGear(bytes, table);
    table.functions[5] = 0xfffffff0;
    Check(!XiaoAnInstall::ValidateGearPayloadBytes(bytes, error), "reject an out-of-bounds export code address");
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 5 || argc > 7) {
        fprintf(stderr, "Usage: InstallerTests <installer> <proxy66> <gear> <original-audio> [proxy65] [proxy64]\n");
        return 2;
    }
    std::wstring error, hash;
    const std::vector<unsigned char> abc = {'a', 'b', 'c'};
    Check(XiaoAnInstall::Sha256Bytes(abc, hash, error) &&
          hash == L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
          "CNG SHA-256 matches the standard abc vector");
    Check(XiaoAnInstall::ValidatePe32Executable(argv[1], error), "installer is an x86 PE32 executable");
    Check(XiaoAnInstall::ValidateProxyAudio(argv[2], error), "built proxy has the exact 20 forwarders");
    Check(XiaoAnInstall::ValidateGear(argv[3], error), "built gear module has the current six exports");
    Check(XiaoAnInstall::ValidateOriginalAudio(argv[4], error), "sample original audio has 20 implementations");

    HMODULE installer = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE);
    std::vector<unsigned char> proxy, proxy65, proxy64, gear, config, features;
    Check(installer && XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_PROXY, proxy, error) &&
          XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_GEAR, gear, error) &&
          XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_CONFIG, config, error) &&
          XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_FEATURES, features, error),
          "installer contains runtime payloads and feature defaults");
    Check(installer && XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_PROXY65, proxy65, error) &&
          XiaoAnInstall::ValidateProxyAudioBytes(proxy65, error),
          "installer contains a valid 6.5 proxy with the exact 20 forwarders");
    if (argc == 6) Check(SameHash(proxy65, argv[5]), "embedded 6.5 proxy is byte-identical to its final build");
    if (argc == 7) {
        Check(SameHash(proxy65, argv[5]), "embedded 6.5 proxy is byte-identical to its final build");
        Check(installer && XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_PROXY64, proxy64, error) &&
            XiaoAnInstall::ValidateProxyAudioBytes(proxy64, error),
            "installer contains a valid 6.4 proxy with the exact 20 forwarders");
        Check(SameHash(proxy64, argv[6]), "embedded 6.4 proxy is byte-identical to its final build");
    }
    Check(proxy != proxy65, "6.5 and 6.6 runtime resources are distinct");
    Check(SameHash(proxy, argv[2]), "embedded proxy is byte-identical to the final build");
    Check(SameHash(gear, argv[3]), "embedded gear module is byte-identical to the final build");
    CheckGearContract(gear);
    size_t lines = 0;
    for (unsigned char ch : config) if (ch == '\n') ++lines;
    if (!config.empty() && config.back() != '\n') ++lines;
    Check(lines == 74, "embedded private configuration contains exactly 74 lines");
    Check(features.size() > 32, "embedded feature configuration is present");
    if (installer) FreeLibrary(installer);

    HMODULE proxyModule = LoadLibraryExW(argv[2], nullptr, LOAD_LIBRARY_AS_DATAFILE);
    std::vector<unsigned char> worker;
    Check(proxyModule && XiaoAnInstall::ReadResourceBytes(proxyModule, IDR_UNINSTALL_WORKER, worker, error),
          "proxy contains the standalone uninstall worker");
    wchar_t temporary[MAX_PATH] = {}, workerPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temporary);
    GetTempFileNameW(temporary, L"xa6", 0, workerPath);
    Check(XiaoAnInstall::WriteBytes(workerPath, worker, error) &&
          XiaoAnInstall::ValidatePe32Executable(workerPath, error),
          "embedded uninstall worker is an x86 PE32 executable");
    DeleteFileW(workerPath);
    if (proxyModule) FreeLibrary(proxyModule);
    if (argc >= 6) {
        HMODULE proxy65Module = LoadLibraryExW(argv[5], nullptr, LOAD_LIBRARY_AS_DATAFILE);
        std::vector<unsigned char> worker65;
        Check(proxy65Module && XiaoAnInstall::ReadResourceBytes(proxy65Module, IDR_UNINSTALL_WORKER, worker65, error),
              "6.5 proxy contains the standalone uninstall worker");
        GetTempFileNameW(temporary, L"xa5", 0, workerPath);
        Check(XiaoAnInstall::WriteBytes(workerPath, worker65, error) &&
              XiaoAnInstall::ValidatePe32Executable(workerPath, error),
              "6.5 embedded uninstall worker is an x86 PE32 executable");
        DeleteFileW(workerPath);
        if (proxy65Module) FreeLibrary(proxy65Module);
    }
    printf("InstallerTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
