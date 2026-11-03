#include "InstallCommon.h"
#include "ResourceIds.h"

#include <stdio.h>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

bool Run(const std::wstring& program, const std::wstring& arguments, bool inheritHandles, DWORD& code) {
    std::wstring command = L"\"" + program + L"\" " + arguments;
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(0);
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(program.c_str(), buffer.data(), nullptr, nullptr, inheritHandles ? TRUE : FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    bool result = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code);
    if (wait == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 9);
    CloseHandle(process.hProcess);
    return result;
}

bool FinishedProcess(HANDLE& inherited) {
    wchar_t systemDirectory[MAX_PATH] = {};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) return false;
    const std::wstring commandPath = std::wstring(systemDirectory) + L"\\cmd.exe";
    std::wstring command = L"\"" + commandPath + L"\" /d /c exit 0";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(0);
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(commandPath.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread);
    const bool result = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0 &&
        DuplicateHandle(GetCurrentProcess(), process.hProcess, GetCurrentProcess(), &inherited,
            SYNCHRONIZE, TRUE, 0);
    CloseHandle(process.hProcess);
    return result;
}

bool RunWorker(const std::wstring& worker, const std::wstring& root,
    const std::wstring& extra, DWORD& code) {
    HANDLE finished = nullptr;
    if (!FinishedProcess(finished)) return false;
    const std::wstring arguments = L"/root \"" + root + L"\" /handle " +
        std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(finished))) +
        L" /quiet" + extra;
    const bool result = Run(worker, arguments, true, code);
    CloseHandle(finished);
    return result;
}

bool Fixture(const std::wstring& root, const wchar_t* exe, const wchar_t* audio) {
    return CreateDirectoryW(root.c_str(), nullptr) &&
        CopyFileW(exe, XiaoAnInstall::JoinPath(root, L"Ekd5.exe").c_str(), TRUE) &&
        CopyFileW(audio, XiaoAnInstall::JoinPath(root, L"Koeicda.dll").c_str(), TRUE);
}

bool Restored(const std::wstring& root, const std::wstring& expected) {
    using namespace XiaoAnInstall;
    std::wstring actual, error;
    return Sha256File(JoinPath(root, L"Koeicda.dll"), actual, error) && expected == actual &&
        !FileExists(ManifestPath(root, EngineProfileId::Ccz66)) &&
        !FileExists(JoinPath(root, L"Koeicda_org.dll")) && !FileExists(JoinPath(root, L"GearNtKe.dll"));
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 6) {
        fprintf(stderr, "Usage: UninstallWorkerCompatibilityTests <old-installer> <new-installer> <exe66> <original-audio> <new-work-directory>\n");
        return 2;
    }
    using namespace XiaoAnInstall;
    const std::wstring workspace = CanonicalRoot(argv[5]);
    if (workspace.empty() || FileExists(workspace) || DirectoryExists(workspace) || !CreateDirectoryW(workspace.c_str(), nullptr)) {
        fprintf(stderr, "The test work directory must be new.\n");
        return 2;
    }
    DWORD code = 999;
    std::wstring error;
    HMODULE newInstaller = LoadLibraryExW(argv[2], nullptr, LOAD_LIBRARY_AS_DATAFILE);
    std::vector<unsigned char> newProxy, newWorker;
    Check(newInstaller && ReadResourceBytes(newInstaller, IDR_INSTALL_PROXY, newProxy, error),
          "extract the new 6.6 proxy as a data resource");
    if (newInstaller) FreeLibrary(newInstaller);
    const std::wstring proxyPath = JoinPath(workspace, L"current-proxy.dll");
    Check(WriteBytes(proxyPath, newProxy, error), "write the current proxy only inside the test workspace");
    HMODULE proxy = LoadLibraryExW(proxyPath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    Check(proxy && ReadResourceBytes(proxy, IDR_UNINSTALL_WORKER, newWorker, error),
          "extract the new standalone worker without executing the proxy");
    if (proxy) FreeLibrary(proxy);
    const std::wstring workerPath = JoinPath(workspace, L"current-worker.exe");
    Check(WriteBytes(workerPath, newWorker, error) && ValidatePe32Executable(workerPath, error),
          "new worker is a complete x86 PE32 executable");
    if (newWorker.empty()) return 1;
    std::wstring expected;
    Check(Sha256File(argv[4], expected, error), "hash the original audio baseline");

    const std::wstring historical = JoinPath(workspace, L"schema-one");
    Manifest manifest;
    Check(Fixture(historical, argv[3], argv[4]) &&
        Run(argv[1], L"\"" + historical + L"\" /install /quiet", false, code) && code == 0 &&
        LoadManifest(historical, manifest, error) && manifest.schemaVersion == 1,
        "historical installer creates an authentic schema-one fixture");
    Check(RunWorker(workerPath, historical, L"", code) && code == 0 && Restored(historical, expected),
        "new worker restores schema one using the unchanged root, handle, and quiet CLI");

    const std::wstring current = JoinPath(workspace, L"schema-two");
    Check(Fixture(current, argv[3], argv[4]) &&
        Run(argv[2], L"\"" + current + L"\" /install /quiet", false, code) && code == 0 &&
        LoadManifest(current, manifest, error) && manifest.schemaVersion == kManifestSchema,
        "new installer creates a schema-two all-feature fixture");
    Check(RunWorker(workerPath, current, L"", code) && code == 0 && Restored(current, expected),
        "new worker restores schema two through the original full-uninstall CLI");

    const std::wstring selective = JoinPath(workspace, L"selective");
    Check(Fixture(selective, argv[3], argv[4]) &&
        Run(argv[2], L"\"" + selective + L"\" /install /quiet", false, code) && code == 0,
        "create an all-feature fixture for numeric feature-mask arguments");
    const std::wstring selectiveManifest = ManifestPath(selective, EngineProfileId::Ccz66);
    Check(RunWorker(workerPath, selective, L" /features 2", code) && code == 0 &&
        LoadManifest(selective, manifest, error) &&
        manifest.featureMask == (FeatureAccelerator | FeatureBattleStats) &&
        FileExists(JoinPath(selective, L"GearNtKe.dll")) && FileExists(selectiveManifest),
        "numeric features two removes only guide and preserves accelerator and statistics");
    Check(RunWorker(workerPath, selective, L" /features 5", code) && code == 0 && Restored(selective, expected),
        "numeric features five removes the remaining functions and restores the original files");
    printf("UninstallWorkerCompatibilityTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
