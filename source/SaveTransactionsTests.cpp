#include "SaveTransactions.h"
#include "BattleStatsPersistence.h"
#include "EngineProfile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {
int failures = 0;
const char* kChunks[] = {"native-header|", "native-main-body|", "native-person-table|", "native-tail|"};

void Check(bool condition, const char* name) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    fflush(stdout);
    if (!condition) ++failures;
}

bool WriteText(const std::wstring& path, const char* text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const DWORD bytes = static_cast<DWORD>(strlen(text));
    DWORD written = 0;
    bool okay = WriteFile(file, text, bytes, &written, nullptr) && written == bytes && FlushFileBuffers(file);
    if (!CloseHandle(file)) okay = false;
    return okay;
}

std::vector<BYTE> ReadBytes(const std::wstring& path) {
    std::vector<BYTE> bytes;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return bytes;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart < 16 * 1024 * 1024) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
            read != bytes.size()) bytes.clear();
    }
    CloseHandle(file);
    return bytes;
}

std::vector<BYTE> Bytes(const char* text) {
    return std::vector<BYTE>(text, text + strlen(text));
}

BattleStatsPersistedState SampleState(bool newer) {
    BattleStatsPersistedState state;
    state.active = true;
    state.scenario = L"S_07.eex";
    state.scenarioOrdinal = 15;
    state.campaignId = 123456;
    state.battleInstance = newer ? 10 : 9;
    state.elapsedMilliseconds = newer ? 200000 : 100000;
    state.knownMetrics = 0xFF;
    BattleUnitTotals unit;
    unit.personId = 1002;
    unit.slot = 2;
    unit.label = L"曹操";
    unit.damageDealt = newer ? 99 : 10;
    unit.recoveryDone = newer ? 20 : 2;
    unit.criticalCount = newer ? 3 : 1;
    state.units.push_back(unit);
    unit.damageDealt = newer ? 999 : 100;
    state.campaignUnits.push_back(unit);
    unit.personId = 65000;
    unit.slot = -1;
    unit.label = L"已退场角色";
    unit.damageDealt = newer ? 555 : 500;
    state.campaignUnits.push_back(unit);
    return state;
}

struct Fixture {
    std::wstring root, target, nativeExtension, targetExtension;
    std::vector<BYTE> originalSave, originalNativeExtension, originalTargetExtension;
    bool privateSave = false;
};

Fixture Paths(const std::wstring& root, bool privateSave) {
    Fixture fixture;
    fixture.root = root;
    fixture.privateSave = privateSave;
    fixture.nativeExtension = root + L"\\SV\\SV001.EXS";
    const std::wstring base = privateSave ?
        PatchPrivatePath(root, CompiledEngineProfile()) + L"\\autosave\\start-001" :
        root + L"\\SV\\SV001";
    fixture.target = base + L".E5S";
    fixture.targetExtension = base + L".EXS";
    return fixture;
}

Fixture Prepare(const std::wstring& root, bool privateSave) {
    Fixture fixture = Paths(root, privateSave);
    std::error_code errorCode;
    std::filesystem::create_directories(root + L"\\SV", errorCode);
    std::filesystem::create_directories(std::filesystem::path(fixture.target).parent_path(), errorCode);
    std::wstring error;
    Check(!errorCode && WriteText(fixture.target, "previous-native-save") &&
        WriteBattleStatsEmbedded(fixture.target, SampleState(false), error),
        "prepare an independent temporary save with older embedded statistics");
    Check(WriteText(fixture.nativeExtension, "previous-numbered-extension"), "prepare numbered EXS");
    if (privateSave)
        Check(WriteText(fixture.targetExtension, "previous-private-extension"), "prepare private EXS");
    fixture.originalSave = ReadBytes(fixture.target);
    fixture.originalNativeExtension = ReadBytes(fixture.nativeExtension);
    fixture.originalTargetExtension = ReadBytes(fixture.targetExtension);
    return fixture;
}

// Writes the same four native segments that the production observer expects.
// A short write is physical, and the observer receives its actual byte count.
bool SimulateNativeWriter(const Fixture& fixture, const char* shortFilename,
    bool shortWrite = false, bool closeFailure = false, unsigned segmentCount = 4,
    bool observeClose = true, bool writeExtension = true, bool observeExtension = true,
    bool extensionSucceeded = true) {
    const std::wstring temporary = fixture.root + L"\\" +
        std::wstring(shortFilename, shortFilename + strlen(shortFilename));
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool okay = true;
    for (unsigned index = 0; index < segmentCount; ++index) {
        const DWORD requested = static_cast<DWORD>(strlen(kChunks[index]));
        const DWORD actualRequest = shortWrite && index == 1 ? requested - 1 : requested;
        DWORD written = 0;
        const bool result = WriteFile(file, kChunks[index], actualRequest, &written, nullptr) != FALSE;
        ObserveSaveWrite(requested, result ? written : 0);
        okay = result && okay;
    }
    okay = FlushFileBuffers(file) != FALSE && okay;
    const bool closed = CloseHandle(file) != FALSE;
    if (observeClose) ObserveSaveClose(closed && !closeFailure);
    const bool extensionWritten = !writeExtension || WriteText(fixture.nativeExtension, "new-native-extension");
    if (observeExtension) ObserveSaveExtension(extensionWritten && extensionSucceeded);
    return extensionWritten && closed && okay;
}

bool OriginalPairRestored(const Fixture& fixture) {
    return ReadBytes(fixture.target) == fixture.originalSave &&
        ReadBytes(fixture.nativeExtension) == fixture.originalNativeExtension &&
        ReadBytes(fixture.targetExtension) == fixture.originalTargetExtension;
}

bool Begin(const Fixture& fixture, char (&shortFilename)[16]) {
    std::wstring error;
    if (!RestoreBattleStatsState(SampleState(true), GetTickCount64())) return false;
    const bool okay = BeginSaveTransaction(fixture.root, fixture.target, fixture.nativeExtension,
        fixture.targetExtension, shortFilename, error, true);
    return okay && SaveTransactionActive() && strlen(shortFilename) < sizeof(shortFilename);
}

void ValidateSuccess(const std::wstring& root, bool privateSave) {
    const Fixture fixture = Prepare(root, privateSave);
    char shortFilename[16] = {};
    Check(Begin(fixture, shortFilename), "begin a transaction without requiring collection hooks");
    Check(GetFileAttributesW(fixture.nativeExtension.c_str()) == INVALID_FILE_ATTRIBUTES,
        "remove stale numbered EXS only after its durable recovery backup is ready");
    Check(SimulateNativeWriter(fixture, shortFilename), "observe four complete native writes and successful close");
    Check(ReadBytes(fixture.target) == fixture.originalSave,
        "keep the original E5S intact until a complete save can be committed");
    std::wstring error;
    Check(FinishSaveTransaction(error) && !SaveTransactionActive(), "commit a complete native save and statistics together");
    const auto saved = ReadBytes(fixture.target);
    std::vector<BYTE> native;
    for (const char* chunk : kChunks) {
        const auto bytes = Bytes(chunk);
        native.insert(native.end(), bytes.begin(), bytes.end());
    }
    Check(saved.size() > native.size() && std::equal(native.begin(), native.end(), saved.begin()),
        "preserve every byte of the four native segments before the embedded statistics");
    BattleStatsPersistedState loaded;
    Check(ReadBattleStatsEmbedded(fixture.target, loaded, error) == BattleStatsReadResult::Loaded &&
        loaded.units.size() == 1 && loaded.campaignUnits.size() == 2 &&
        loaded.units[0].damageDealt == 99 && loaded.campaignUnits[0].damageDealt == 999 &&
        loaded.campaignUnits[1].personId == 65000 && loaded.battleInstance == 10,
        "read both statistic ledgers belonging to the newly committed native save");
    Check(ReadBytes(fixture.targetExtension) == Bytes("new-native-extension"),
        "commit the matching native EXS alongside the E5S");
    if (privateSave)
        Check(ReadBytes(fixture.nativeExtension) == fixture.originalNativeExtension,
            "restore the numbered source EXS after committing a private autosave");
    Check(RecoverSaveTransactions(root, error), "recovery after a committed transaction is an idempotent no-op");
}

void ValidateFailure(const std::wstring& root, int fault, bool shortWrite,
    bool closeFailure, bool privateSave, unsigned segments = 4, bool observeClose = true) {
    const Fixture fixture = Prepare(root, privateSave);
    char shortFilename[16] = {};
    Check(Begin(fixture, shortFilename), "begin isolated failure-injection transaction");
    Check(SimulateNativeWriter(fixture, shortFilename, shortWrite, closeFailure, segments, observeClose),
        "run native writes under failure injection");
    SetSaveTransactionFaultForTest(fault);
    std::wstring error;
    const bool result = FinishSaveTransaction(error);
    SetSaveTransactionFaultForTest(0);
    Check(!result && !error.empty() && !SaveTransactionActive(),
        "reject short writes, failed closes, incomplete writes, or failed replacement");
    Check(OriginalPairRestored(fixture), "rollback restores the previous E5S and every affected EXS byte for byte");
    Check(RecoverSaveTransactions(root, error) && OriginalPairRestored(fixture),
        "failure cleanup leaves recovery idempotent and preserves the original save pair");
}

DWORD RunCrashChild(const std::wstring& root) {
    wchar_t executable[32768] = {};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(_countof(executable)));
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --crash-private \"" + root + L"\"";
    STARTUPINFOW startup = {sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, root.c_str(), &startup, &process)) return MAXDWORD;
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD code = MAXDWORD;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    else TerminateProcess(process.hProcess, 92);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

int CrashChild(const std::wstring& root) {
    const Fixture fixture = Paths(root, true);
    char shortFilename[16] = {};
    if (!Begin(fixture, shortFilename) || !SimulateNativeWriter(fixture, shortFilename)) return 93;
    SetSaveTransactionFaultForTest(3);
    std::wstring error;
    FinishSaveTransaction(error); // Terminates at the real post-replace, pre-commit crash point.
    return 94;
}

void ValidateCrashRecovery(const std::wstring& root) {
    const Fixture fixture = Prepare(root, true);
    Check(RunCrashChild(root) == 91, "a separate process crashes after E5S replacement and before journal commit");
    Check(ReadBytes(fixture.target) != fixture.originalSave,
        "the crash fixture really replaced E5S before terminating");
    const std::wstring journal = PatchPrivatePath(root, CompiledEngineProfile()) + L"\\transactions\\active.ini";
    Check(GetFileAttributesW(journal.c_str()) != INVALID_FILE_ATTRIBUTES,
        "retain the uncommitted journal across process termination");
    std::wstring error;
    Check(RecoverSaveTransactions(root, error) && OriginalPairRestored(fixture),
        "a fresh process state recovers E5S, private EXS, and numbered EXS from durable backups");
    Check(GetFileAttributesW(journal.c_str()) == INVALID_FILE_ATTRIBUTES &&
        RecoverSaveTransactions(root, error) && OriginalPairRestored(fixture),
        "completed crash recovery cleans its journal and can safely run again");
    BattleStatsPersistedState loaded;
    Check(ReadBattleStatsEmbedded(fixture.target, loaded, error) == BattleStatsReadResult::Loaded &&
        loaded.units[0].damageDealt == 10 && loaded.campaignUnits[0].damageDealt == 100 &&
        loaded.battleInstance == 9,
        "crash recovery restores statistics from the same earlier native save");
}

void ValidateExtensionFailure(const std::wstring& root, bool writeExtension,
    bool observeExtension, bool succeeded) {
    const Fixture fixture = Prepare(root, true);
    char shortFilename[16] = {};
    Check(Begin(fixture, shortFilename), "begin isolated extension freshness failure transaction");
    Check(SimulateNativeWriter(fixture, shortFilename, false, false, 4, true,
        writeExtension, observeExtension, succeeded), "finish native E5S while injecting an extension failure");
    std::wstring error;
    Check(!FinishSaveTransaction(error) && !error.empty() && OriginalPairRestored(fixture),
        "reject missing, unobserved, or unsuccessful EXS callback and restore the previous complete pair");
}

void ValidateStrictJournal(const std::wstring& root, const wchar_t* field,
    const wchar_t* badValue, const wchar_t* correctValue) {
    const Fixture fixture = Prepare(root, true);
    Check(RunCrashChild(root) == 91, "create a real interrupted transaction before journal validation");
    const std::wstring journal = PatchPrivatePath(root, CompiledEngineProfile()) + L"\\transactions\\active.ini";
    Check(WritePrivateProfileStringW(L"Transaction", field, badValue, journal.c_str()) != FALSE,
        "inject a missing or invalid required journal field");
    const auto damagedJournal = ReadBytes(journal);
    const auto crashedSave = ReadBytes(fixture.target);
    const auto crashedNativeExtension = ReadBytes(fixture.nativeExtension);
    const auto crashedTargetExtension = ReadBytes(fixture.targetExtension);
    std::wstring error;
    Check(!RecoverSaveTransactions(root, error) && !error.empty() &&
        ReadBytes(fixture.target) == crashedSave &&
        ReadBytes(fixture.nativeExtension) == crashedNativeExtension &&
        ReadBytes(fixture.targetExtension) == crashedTargetExtension &&
        ReadBytes(journal) == damagedJournal,
        "reject malformed recovery metadata without deleting files or the pending journal");
    char shortFilename[16] = {};
    Check(!BeginSaveTransaction(root, fixture.target, fixture.nativeExtension, fixture.targetExtension,
        shortFilename, error, true) && ReadBytes(journal) == damagedJournal,
        "a new save attempt must preserve an earlier transaction whose recovery failed");
    Check(WritePrivateProfileStringW(L"Transaction", field, correctValue, journal.c_str()) != FALSE &&
        RecoverSaveTransactions(root, error) && OriginalPairRestored(fixture),
        "retain enough original backups to recover after repairing the invalid journal field");
}

void ValidateLockedRecovery(const std::wstring& root) {
    const Fixture fixture = Prepare(root, true);
    Check(RunCrashChild(root) == 91, "create an interrupted save for locked-target recovery");
    const std::wstring journal = PatchPrivatePath(root, CompiledEngineProfile()) + L"\\transactions\\active.ini";
    HANDLE locked = CreateFileW(fixture.target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, 0, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock the replaced native save against recovery replacement");
    const auto pending = ReadBytes(journal);
    std::wstring error;
    char shortFilename[16] = {};
    Check(!RecoverSaveTransactions(root, error) &&
        !BeginSaveTransaction(root, fixture.target, fixture.nativeExtension, fixture.targetExtension,
            shortFilename, error, true) && ReadBytes(journal) == pending,
        "failed recovery and a subsequent save attempt preserve the old recovery journal");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    Check(RecoverSaveTransactions(root, error) && OriginalPairRestored(fixture),
        "recover the previous complete save after releasing the target lock");
}

int HoldRootLockChild(const std::wstring& root, const wchar_t* readyName, const wchar_t* releaseName,
    bool groupRead) {
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
    HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, releaseName);
    char shortFilename[16] = {};
    const Fixture fixture = Paths(root, true);
    std::wstring error;
    const bool begun = groupRead ? BeginSaveGroupRead(root, error) : Begin(fixture, shortFilename);
    if (!ready || !release || !begun) return 95;
    SetEvent(ready);
    const DWORD result = WaitForSingleObject(release, 30000);
    if (groupRead) EndSaveGroupRead(); else AbortSaveTransaction();
    CloseHandle(ready); CloseHandle(release);
    return result == WAIT_OBJECT_0 ? 0 : 96;
}

void ValidateCrossProcessLock(const std::wstring& root, bool groupRead = false) {
    const Fixture fixture = Prepare(root, true);
    const std::wstring eventBase = L"Local\\CczSaveTxnTest-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    const std::wstring readyName = eventBase + L"-ready", releaseName = eventBase + L"-release";
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
    wchar_t executable[32768] = {};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(_countof(executable)));
    std::wstring command = L"\"" + std::wstring(executable) +
        (groupRead ? L"\" --hold-read-lock \"" : L"\" --hold-root-lock \"") + root +
        L"\" \"" + readyName + L"\" \"" + releaseName + L"\"";
    STARTUPINFOW startup = {sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {};
    const bool started = ready && release && CreateProcessW(executable, command.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process);
    Check(started, "start a second process holding this game root's save transaction lock");
    if (!started) { if (ready) CloseHandle(ready); if (release) CloseHandle(release); return; }
    const bool locked = WaitForSingleObject(ready, 30000) == WAIT_OBJECT_0;
    Check(locked, "a separate process holds a real active save transaction");
    const std::wstring journal = PatchPrivatePath(root, CompiledEngineProfile()) + L"\\transactions\\active.ini";
    const auto pending = ReadBytes(journal);
    std::wstring error;
    char shortFilename[16] = {};
    Check(locked && (groupRead || !pending.empty()) && !RecoverSaveTransactions(root, error) &&
        !BeginSaveGroupRead(root, error) &&
        !BeginSaveTransaction(root, fixture.target, fixture.nativeExtension, fixture.targetExtension,
            shortFilename, error, true) && ReadBytes(journal) == pending,
        "another process cannot recover or overwrite an active transaction's journal");
    Check(RecoverSaveTransactions(root + L"\\independent-game", error),
        "a different game root has an independent transaction lock");
    SetEvent(release);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD exitCode = MAXDWORD;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exitCode);
    else TerminateProcess(process.hProcess, 97);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    CloseHandle(ready); CloseHandle(release);
    Check(exitCode == 0 && RecoverSaveTransactions(root, error) && OriginalPairRestored(fixture),
        "the OS lock releases after the other process aborts and original saves remain intact");
}

DWORD WINAPI ReleaseForeignReadLock(void*) {
    EndSaveGroupRead();
    return 0;
}

void ValidateGroupReadLock(const std::wstring& root) {
    const Fixture fixture = Prepare(root, true);
    std::wstring error;
    Check(BeginSaveGroupRead(root, error), "hold the root transaction lock throughout a native save-group read");
    char shortFilename[16] = {};
    Check(!BeginSaveGroupRead(root, error) && !RecoverSaveTransactions(root, error) &&
        !BeginSaveTransaction(root, fixture.target, fixture.nativeExtension, fixture.targetExtension,
            shortFilename, error, true),
        "a reader excludes nested independent readers, recovery and writers until its own completion");
    HANDLE thread = CreateThread(nullptr, 0, ReleaseForeignReadLock, nullptr, 0, nullptr);
    bool foreignComplete = false;
    if (thread) { foreignComplete = WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0; CloseHandle(thread); }
    Check(foreignComplete && !RecoverSaveTransactions(root, error),
        "a different thread cannot release a native reader's root lock");
    EndSaveGroupRead();
    Check(Begin(fixture, shortFilename), "the owning reader releases its root lock for the next save");
    AbortSaveTransaction();
    EndSaveGroupRead();
    Check(RecoverSaveTransactions(root, error) && OriginalPairRestored(fixture),
        "repeated reader cleanup is harmless after its successful release");
}

void ValidateAllocationFailure(const std::wstring& root, int fault) {
    const Fixture fixture = Prepare(root, true);
    std::wstring error;
    char shortFilename[16] = {};
    if (fault == 5) {
        Check(Begin(fixture, shortFilename) && SimulateNativeWriter(fixture, shortFilename),
            "prepare allocation failure after replacing the native save");
    }
    SetSaveTransactionFaultForTest(fault);
    bool rejected = false;
    if (fault == 4) rejected = !Begin(fixture, shortFilename);
    else if (fault == 5) rejected = !FinishSaveTransaction(error);
    else if (fault == 6) rejected = !RecoverSaveTransactions(root, error);
    else rejected = !BeginSaveGroupRead(root, error);
    SetSaveTransactionFaultForTest(0);
    Check(rejected && !SaveTransactionActive(),
        "allocation failure is contained at the public transaction boundary");
    Check(RecoverSaveTransactions(root, error) && OriginalPairRestored(fixture),
        "allocation failure releases process/root locks and leaves sufficient recovery data");
    Check(BeginSaveGroupRead(root, error), "read locking remains usable after an allocation exception");
    EndSaveGroupRead();
}

bool WriteSized(const std::wstring& path, LONGLONG bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length = {}; length.QuadPart = bytes;
    const bool ok = SetFilePointerEx(file, length, nullptr, FILE_BEGIN) && SetEndOfFile(file) && FlushFileBuffers(file);
    return CloseHandle(file) && ok;
}

int ValidateRealExtensionFixture(const std::wstring& dllPath) {
    // Load only the PE image for instruction/data inspection. Never run
    // DllMain, SaveVar or any dependency initialization in this fixture.
    HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    Check(module != nullptr, "map the actual 6.6 DLL without executing any of its code");
    if (!module) return 1;
    const auto writer = reinterpret_cast<uintptr_t>(GetProcAddress(module, "SaveVar"));
    wchar_t temporary[32768] = {};
    GetTempPathW(static_cast<DWORD>(_countof(temporary)), temporary);
    const std::wstring root = std::wstring(temporary) + L"CczEXSFormatFixture-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    std::filesystem::create_directories(root);
    const auto samples = std::filesystem::path(dllPath).parent_path() / L"SV";
    unsigned count = 0;
    std::wstring error;
    for (const auto& entry : std::filesystem::directory_iterator(samples)) {
        if (!entry.is_regular_file() || _wcsicmp(entry.path().extension().c_str(), L".EXS")) continue;
        const std::wstring copied = root + L"\\" + entry.path().filename().wstring();
        Check(CopyFileW(entry.path().c_str(), copied.c_str(), FALSE) &&
            ValidateNativeExtensionForTest(writer, copied, error),
            "accept a copied real EXS using verified writer code and current runtime dimensions");
        ++count;
    }
    Check(count >= 7, "inspect at least seven original EXS samples without modifying them");
    const std::wstring generated = root + L"\\generated.EXS";
    Check(WriteSized(generated, 1384447) && !ValidateNativeExtensionForTest(writer, generated, error),
        "reject a newly created EXS truncated by one byte");
    Check(WriteSized(generated, 0) && !ValidateNativeExtensionForTest(writer, generated, error),
        "reject a newly created empty EXS even though the file exists");
    Check(WriteSized(generated, 1384448) && !ValidateNativeExtensionForTest(writer + 1, generated, error),
        "reject a function address whose writer code cannot be verified");
    auto dimension = reinterpret_cast<DWORD*>(reinterpret_cast<uintptr_t>(module) + 0x1D3A34);
    const DWORD original = *dimension;
    *dimension = 49;
    Check(!ValidateNativeExtensionForTest(writer, generated, error) &&
        WriteSized(generated, 1384448 - 4096) && ValidateNativeExtensionForTest(writer, generated, error),
        "derive size from current dimensions instead of using a previous EXS size");
    *dimension = 51;
    Check(!ValidateNativeExtensionForTest(writer, generated, error),
        "reject runtime dimensions that exceed the verified writer's array capacities");
    *dimension = original;
    FreeLibrary(module);
    printf("NativeExtensionFixtureTests: %d failures\n", failures);
    wprintf(L"Temporary EXS evidence: %s\n", root.c_str());
    return failures ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && !wcscmp(argv[1], L"--native-extension-fixture"))
        return ValidateRealExtensionFixture(argv[2]);
    if (argc == 3 && !wcscmp(argv[1], L"--crash-private")) return CrashChild(argv[2]);
    if (argc == 5 && !wcscmp(argv[1], L"--hold-root-lock"))
        return HoldRootLockChild(argv[2], argv[3], argv[4], false);
    if (argc == 5 && !wcscmp(argv[1], L"--hold-read-lock"))
        return HoldRootLockChild(argv[2], argv[3], argv[4], true);
    wchar_t temporary[32768] = {};
    if (!GetTempPathW(static_cast<DWORD>(_countof(temporary)), temporary)) return 2;
    const std::wstring root = std::wstring(temporary) + L"CczSaveTransactionsTests-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    ShutdownBattleStats();
    ValidateSuccess(root + L"\\manual-success", false);
    ValidateSuccess(root + L"\\private-success", true);
    ValidateFailure(root + L"\\short-write", 0, true, false, false);
    ValidateFailure(root + L"\\close-failure", 0, false, true, false);
    ValidateFailure(root + L"\\missing-write", 0, false, false, false, 3);
    ValidateFailure(root + L"\\missing-close", 0, false, false, false, 4, false);
    ValidateFailure(root + L"\\before-replace-failure", 1, false, false, false);
    ValidateFailure(root + L"\\after-replace-failure", 2, false, false, false);
    ValidateFailure(root + L"\\private-after-replace-failure", 2, false, false, true);
    ValidateCrashRecovery(root + L"\\cross-process-crash");
    ValidateExtensionFailure(root + L"\\missing-extension-file", false, true, true);
    ValidateExtensionFailure(root + L"\\missing-extension-callback", true, false, true);
    ValidateExtensionFailure(root + L"\\failed-extension-callback", true, true, false);
    ValidateStrictJournal(root + L"\\missing-existed", L"Existed0", nullptr, L"1");
    ValidateStrictJournal(root + L"\\invalid-existed", L"Existed0", L"2", L"1");
    ValidateStrictJournal(root + L"\\missing-commit", L"Committed", nullptr, L"0");
    ValidateStrictJournal(root + L"\\invalid-count", L"Count", L"3garbage", L"3");
    ValidateLockedRecovery(root + L"\\locked-recovery");
    ValidateCrossProcessLock(root + L"\\cross-process-lock");
    ValidateCrossProcessLock(root + L"\\cross-process-read-lock", true);
    ValidateGroupReadLock(root + L"\\save-group-read-lock");
    ValidateAllocationFailure(root + L"\\begin-allocation-failure", 4);
    ValidateAllocationFailure(root + L"\\finish-allocation-failure", 5);
    ValidateAllocationFailure(root + L"\\recover-allocation-failure", 6);
    ValidateAllocationFailure(root + L"\\read-allocation-failure", 7);
    ShutdownBattleStats();
    printf("SaveTransactionsTests: %d failures\n", failures);
    wprintf(L"Temporary test evidence: %s\n", root.c_str());
    return failures ? 1 : 0;
}
