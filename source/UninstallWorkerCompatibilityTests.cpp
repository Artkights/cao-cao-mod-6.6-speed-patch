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
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 6) {
        fprintf(stderr, "Usage: UninstallWorkerCompatibilityTests <old-installer> <new-installer> <exe66> <original-audio> <new-work-directory>\n");
        return 2;
    }
    using namespace XiaoAnInstall;
    const std::wstring root = CanonicalRoot(argv[5]);
    if (root.empty() || FileExists(root) || DirectoryExists(root) || !CreateDirectoryW(root.c_str(), nullptr)) {
        fprintf(stderr, "The test work directory must be new.\n");
        return 2;
    }
    Check(CopyFileW(argv[3], JoinPath(root, L"Ekd5.exe").c_str(), TRUE) &&
        CopyFileW(argv[4], JoinPath(root, L"Koeicda.dll").c_str(), TRUE),
        "create an isolated 6.6 fixture without starting the game");
    DWORD code = 999;
    Check(Run(argv[2], L"\"" + root + L"\" /install /quiet", false, code) && code == 0,
          "new installer creates a 6.6 schema 1 manifest");
    std::wstring error;
    HMODULE oldInstaller = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE);
    std::vector<unsigned char> oldProxy, oldWorker;
    Check(oldInstaller && ReadResourceBytes(oldInstaller, IDR_INSTALL_PROXY, oldProxy, error),
          "extract the historical 6.6 proxy as a data resource");
    if (oldInstaller) FreeLibrary(oldInstaller);
    const std::wstring proxyPath = JoinPath(root, L"historical-proxy.dll");
    Check(WriteBytes(proxyPath, oldProxy, error), "write the historical proxy only inside the fixture");
    HMODULE proxy = LoadLibraryExW(proxyPath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    Check(proxy && ReadResourceBytes(proxy, IDR_UNINSTALL_WORKER, oldWorker, error),
          "extract the historical standalone worker without executing the proxy");
    if (proxy) FreeLibrary(proxy);
    const std::wstring workerPath = JoinPath(root, L"historical-worker.exe");
    Check(WriteBytes(workerPath, oldWorker, error) && ValidatePe32Executable(workerPath, error),
          "historical worker is a complete x86 PE32 executable");
    HANDLE finished = nullptr;
    Check(FinishedProcess(finished), "create an inheritable signaled process handle without launching a game");
    const std::wstring arguments = L"/root \"" + root + L"\" /handle " +
        std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(finished))) + L" /quiet";
    Check(finished && Run(workerPath, arguments, true, code) && code == 0,
          "historical worker restores a new 6.6 manifest using its unchanged internal CLI");
    if (finished) CloseHandle(finished);
    std::wstring expected, actual;
    Check(Sha256File(argv[4], expected, error) && Sha256File(JoinPath(root, L"Koeicda.dll"), actual, error) &&
        expected == actual && !FileExists(ManifestPath(root, EngineProfileId::Ccz66)) &&
        !FileExists(JoinPath(root, L"Koeicda_org.dll")) && !FileExists(JoinPath(root, L"GearNtKe.dll")),
        "historical worker restores original bytes and file existence from the new manifest");
    printf("UninstallWorkerCompatibilityTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
