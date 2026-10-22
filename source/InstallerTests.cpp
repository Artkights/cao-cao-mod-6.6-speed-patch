#include "InstallCommon.h"
#include "ResourceIds.h"

#include <stdio.h>

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
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "Usage: InstallerTests <installer> <proxy66> <gear> <original-audio> [proxy65]\n");
        return 2;
    }
    std::wstring error, hash;
    const std::vector<unsigned char> abc = {'a', 'b', 'c'};
    Check(XiaoAnInstall::Sha256Bytes(abc, hash, error) &&
          hash == L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
          "CNG SHA-256 matches the standard abc vector");
    Check(XiaoAnInstall::ValidatePe32Executable(argv[1], error), "installer is an x86 PE32 executable");
    Check(XiaoAnInstall::ValidateProxyAudio(argv[2], error), "built proxy has the exact 20 forwarders");
    Check(XiaoAnInstall::ValidateGear(argv[3], error), "built gear module has the exact five exports");
    Check(XiaoAnInstall::ValidateOriginalAudio(argv[4], error), "sample original audio has 20 implementations");

    HMODULE installer = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE);
    std::vector<unsigned char> proxy, proxy65, gear, config, features;
    Check(installer && XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_PROXY, proxy, error) &&
          XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_GEAR, gear, error) &&
          XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_CONFIG, config, error) &&
          XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_FEATURES, features, error),
          "installer contains runtime payloads and feature defaults");
    Check(installer && XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_PROXY65, proxy65, error) &&
          XiaoAnInstall::ValidateProxyAudioBytes(proxy65, error),
          "installer contains a valid 6.5 proxy with the exact 20 forwarders");
    if (argc == 6) Check(SameHash(proxy65, argv[5]), "embedded 6.5 proxy is byte-identical to its final build");
    Check(proxy != proxy65, "6.5 and 6.6 runtime resources are distinct");
    Check(SameHash(proxy, argv[2]), "embedded proxy is byte-identical to the final build");
    Check(SameHash(gear, argv[3]), "embedded gear module is byte-identical to the final build");
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
    if (argc == 6) {
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
