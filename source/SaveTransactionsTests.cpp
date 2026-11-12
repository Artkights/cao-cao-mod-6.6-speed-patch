#include "SaveTransactions.h"
#include "BattleStatsPersistence.h"
#include "EngineProfile.h"
#include "StatsDiagnostics.h"

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
    state.currentBattleParticipants.push_back(state.units[0].personId);
    for (const BattleUnitTotals& total : state.campaignUnits) {
        BattleRosterEntry entry;
        entry.personId = total.personId;
        entry.label = total.label;
        entry.joined = entry.tracking = true;
        entry.lastCamp = 0;
        entry.source = BattleRosterSource::ExistingCampaignLedger;
        state.campaignRoster.push_back(entry);
    }
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

std::wstring JournalPath(const std::wstring& root) {
    return PatchPrivatePath(root, CompiledEngineProfile()) + L"\\transactions\\active.ini";
}

std::wstring JournalValue(const std::wstring& root, const wchar_t* key) {
    wchar_t value[32768] = {};
    GetPrivateProfileStringW(L"Transaction", key, L"", value, _countof(value), JournalPath(root).c_str());
    return value;
}

bool BeginMode(const Fixture& fixture, char (&filename)[16], bool extension) {
    std::wstring error;
    return RestoreBattleStatsState(SampleState(true), GetTickCount64()) &&
        BeginSaveTransaction(fixture.root, fixture.target,
            extension ? fixture.nativeExtension : L"", extension ? fixture.targetExtension : L"",
            filename, error, extension);
}

bool WriteMode(const Fixture& fixture, const char* filename, bool extension) {
    return SimulateNativeWriter(fixture, filename, false, false, 4, true, extension, extension);
}

DWORD RunStateChild(const std::wstring& root, int fault, bool extension, const wchar_t* operation) {
    wchar_t executable[32768] = {};
    GetModuleFileNameW(nullptr, executable, _countof(executable));
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --state-crash \"" + root +
        L"\" " + std::to_wstring(fault) + L" " + (extension ? L"1" : L"0") + L" " + operation;
    STARTUPINFOW startup = {sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, root.c_str(), &startup, &process)) return MAXDWORD;
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD code = MAXDWORD;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    else TerminateProcess(process.hProcess, 92);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return code;
}

int StateCrashChild(const std::wstring& root, int fault, bool extension, const wchar_t* operation) {
    std::wstring error;
    if (!wcscmp(operation, L"recover")) {
        SetSaveTransactionFaultForTest(fault);
        RecoverSaveTransactions(root, error);
        return 94;
    }
    const Fixture fixture = Paths(root, true);
    char filename[16] = {};
    if (!BeginMode(fixture, filename, extension) || !WriteMode(fixture, filename, extension)) return 93;
    SetSaveTransactionFaultForTest(fault);
    if (!wcscmp(operation, L"abort")) AbortSaveTransaction();
    else {
        if (!wcscmp(operation, L"rollback")) ObserveSaveWrite(1, 0);
        FinishSaveTransaction(error);
    }
    return 94;
}

void ValidateE5sOnly(const std::wstring& root) {
    const Fixture fixture = Prepare(root, true);
    HANDLE nativeLock = CreateFileW(fixture.nativeExtension.c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, 0, nullptr);
    HANDLE targetLock = CreateFileW(fixture.targetExtension.c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, 0, nullptr);
    Check(nativeLock != INVALID_HANDLE_VALUE && targetLock != INVALID_HANDLE_VALUE,
        "exclusively lock unrelated EXS files throughout E5S-only transactions");
    char filename[16] = {};
    std::wstring error;
    Check(BeginMode(fixture, filename, false) && JournalValue(root, L"Count") == L"1" &&
        WriteMode(fixture, filename, false) && FinishSaveTransaction(error),
        "E5S-only save accepts empty extension paths and commits while unrelated EXS are inaccessible");
    const auto committed = ReadBytes(fixture.target);
    BattleStatsPersistedState state;
    Check(ReadBattleStatsEmbedded(fixture.target, state, error) == BattleStatsReadResult::Loaded &&
        state.units[0].damageDealt == 99, "E5S-only save contains the matching statistics snapshot");
    Check(BeginMode(fixture, filename, false) && WriteMode(fixture, filename, false),
        "begin E5S-only replacement failure with inaccessible unrelated EXS");
    SetSaveTransactionFaultForTest(2);
    Check(!FinishSaveTransaction(error), "reject injected E5S-only replacement failure");
    SetSaveTransactionFaultForTest(0);
    Check(ReadBytes(fixture.target) == committed && RecoverSaveTransactions(root, error),
        "E5S-only rollback restores its last committed E5S without opening EXS");
    Check(RunStateChild(root, 3, false, L"commit") == 91 && JournalValue(root, L"Count") == L"1" &&
        RecoverSaveTransactions(root, error) && ReadBytes(fixture.target) == committed,
        "recover a real E5S-only child-process crash while both EXS remain locked");
    if (nativeLock != INVALID_HANDLE_VALUE) CloseHandle(nativeLock);
    if (targetLock != INVALID_HANDLE_VALUE) CloseHandle(targetLock);
    Check(ReadBytes(fixture.nativeExtension) == fixture.originalNativeExtension &&
        ReadBytes(fixture.targetExtension) == fixture.originalTargetExtension,
        "E5S-only success, rollback and crash recovery preserve unrelated EXS byte for byte");
}

void ValidateFirstPrivateSave(const std::wstring& root) {
    const Fixture fixture = Paths(root, true);
    std::filesystem::create_directories(std::filesystem::path(fixture.target).parent_path());
    Check(GetFileAttributesW((root + L"\\SV").c_str()) == INVALID_FILE_ATTRIBUTES,
        "prepare a first private autosave before any numbered save directory exists");
    char filename[16] = {};
    std::wstring error;
    Check(BeginMode(fixture, filename, false) && WriteMode(fixture, filename, false) && FinishSaveTransaction(error),
        "first private autosave creates the native staging directory without requiring an earlier manual save");
    BattleStatsPersistedState state;
    Check(ReadBattleStatsEmbedded(fixture.target, state, error) == BattleStatsReadResult::Loaded &&
        GetFileAttributesW(fixture.nativeExtension.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(fixture.targetExtension.c_str()) == INVALID_FILE_ATTRIBUTES,
        "first E5S-only autosave contains statistics and creates no EXS files");
}

void ValidateStateCrash(const std::wstring& root, int fault, bool extension,
    const wchar_t* operation, bool expectNew) {
    const Fixture fixture = Prepare(root, true);
    Check(RunStateChild(root, fault, extension, operation) == 91,
        "terminate a real child at the requested durable-state or cleanup instruction boundary");
    const auto beforeRecovery = ReadBytes(fixture.target);
    std::wstring error;
    Check(RecoverSaveTransactions(root, error), "recover after an interrupted state transition or cleanup");
    if (expectNew) {
        BattleStatsPersistedState state;
        Check(ReadBytes(fixture.target) == beforeRecovery &&
            ReadBattleStatsEmbedded(fixture.target, state, error) == BattleStatsReadResult::Loaded &&
            state.units[0].damageDealt == 99,
            "a durable Committed marker preserves the new E5S even when backups were partly deleted");
        Check(ReadBytes(fixture.nativeExtension) == fixture.originalNativeExtension &&
            ReadBytes(fixture.targetExtension) == (extension ? Bytes("new-native-extension") : fixture.originalTargetExtension),
            "committed cleanup preserves the correct native and private EXS ownership");
    } else Check(OriginalPairRestored(fixture),
        "Prepared recovery and Restored cleanup both preserve the previous complete save group");
    Check(GetFileAttributesW(JournalPath(root).c_str()) == INVALID_FILE_ATTRIBUTES &&
        RecoverSaveTransactions(root, error), "completed state recovery removes its journal and remains idempotent");
}

void ValidateRecoveryCrash(const std::wstring& root, int fault, bool extension) {
    const Fixture fixture = Prepare(root, true);
    Check(RunStateChild(root, 3, extension, L"commit") == 91,
        "prepare a real post-replacement crash before interrupting recovery again");
    Check(RunStateChild(root, fault, extension, L"recover") == 91,
        "terminate a separate recovery process during restoration or cleanup");
    std::wstring error;
    Check(RecoverSaveTransactions(root, error) && OriginalPairRestored(fixture) &&
        RecoverSaveTransactions(root, error),
        "a third process state safely finishes recovery interrupted after restoring or deleting any member");
}

void ValidateStateCompatibility(const std::wstring& root, bool committed) {
    const Fixture fixture = Prepare(root, true);
    Check(RunStateChild(root, committed ? 25 : 3, true, L"commit") == 91,
        "prepare a durable journal for legacy reader-format compatibility");
    const auto saved = ReadBytes(fixture.target);
    const auto journal = JournalPath(root);
    Check(WritePrivateProfileStringW(L"Transaction", L"SchemaVersion", nullptr, journal.c_str()) &&
        WritePrivateProfileStringW(L"Transaction", L"State", nullptr, journal.c_str()),
        "retain only the legacy Committed bit and member fields");
    std::wstring error;
    Check(RecoverSaveTransactions(root, error) &&
        (committed ? ReadBytes(fixture.target) == saved : OriginalPairRestored(fixture)),
        "read legacy Prepared and cleanup-only journals without guessing their outcome");
}

void ValidateMarkerAndCleanupFailure(const std::wstring& root, bool committed) {
    const Fixture fixture = Prepare(root, true);
    char filename[16] = {};
    std::wstring error;
    Check(BeginMode(fixture, filename, true) && WriteMode(fixture, filename, true),
        "prepare marker and cleanup retry fixture");
    if (!committed) ObserveSaveWrite(1, 0);
    SetSaveTransactionFaultForTest(committed ? 11 : 10);
    Check(FinishSaveTransaction(error) == committed && !SaveTransactionActive(),
        "cleanup failure cannot turn a committed save into failure; failed restore seal remains recoverable");
    Check(ReadSaveRecoveryStatus() == (committed ? SaveRecoveryStatus::CleanupPending : SaveRecoveryStatus::RecoveryPending),
        "the nonblocking public recovery status distinguishes cleanup from unconfirmed rollback");
    Check(JournalValue(root, L"State") == (committed ? L"Committed" : L"Prepared") &&
        GetFileAttributesW((fixture.target + L".xa-old").c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW((fixture.nativeExtension + L".xa-old").c_str()) != INVALID_FILE_ATTRIBUTES,
        "failed cleanup or Restored marker retains its journal and complete backups");
    const auto saved = ReadBytes(fixture.target);
    SetSaveTransactionFaultForTest(0);
    Check(RecoverSaveTransactions(root, error) &&
        (committed ? ReadBytes(fixture.target) == saved : OriginalPairRestored(fixture)) &&
        ReadSaveRecoveryStatus() == SaveRecoveryStatus::Ready,
        "retry cleanup-only or Prepared recovery after the transient failure ends");
}

void ValidatePreparationFailure(const std::wstring& root, int fault) {
    const Fixture fixture = Prepare(root, true);
    char filename[16] = {};
    SetSaveTransactionFaultForTest(fault);
    Check(!BeginMode(fixture, filename, true) && !SaveTransactionActive(),
        "contain allocation failure during snapshot capture or backup preparation");
    SetSaveTransactionFaultForTest(0);
    bool clean = true;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"X", 0) == 0 || name.find(L".xa-old") != std::wstring::npos ||
            name.find(L".tmp") != std::wstring::npos) clean = false;
    }
    std::wstring error;
    Check(clean && OriginalPairRestored(fixture) && RecoverSaveTransactions(root, error) &&
        BeginMode(fixture, filename, true),
        "pre-journal failure removes owned staging files and backups and releases locks for retry");
    AbortSaveTransaction();
}

void ValidateEnumerationFailure(const std::wstring& root, int fault) {
    const Fixture fixture = Prepare(root, true);
    Check(RunStateChild(root, 3, true, L"commit") == 91, "prepare interrupted save for enumeration failure");
    SetSaveTransactionFaultForTest(fault);
    std::wstring error;
    Check(!RecoverSaveTransactions(root, error) && !error.empty(),
        "enumeration allocation and I/O failures are not mistaken for successful recovery");
    SetSaveTransactionFaultForTest(0);
    Check(RecoverSaveTransactions(root, error) && OriginalPairRestored(fixture),
        "enumeration failure releases search and root handles and preserves recoverable saves");
}

void ValidateLegacyRestoredAndMissingBackup(const std::wstring& root, bool restored) {
    const Fixture fixture = Prepare(root, true);
    Check(RunStateChild(root, restored ? 22 : 3, true, restored ? L"rollback" : L"commit") == 91,
        "prepare Restored or Prepared legacy-format recovery evidence");
    const auto journal = JournalPath(root);
    Check(WritePrivateProfileStringW(L"Transaction", L"SchemaVersion", nullptr, journal.c_str()) &&
        WritePrivateProfileStringW(L"Transaction", L"State", nullptr, journal.c_str()) &&
        DeleteFileW((fixture.target + L".xa-old").c_str()),
        "simulate a partially removed backup while retaining the legacy cleanup bit");
    const auto nativeBefore = ReadBytes(fixture.nativeExtension);
    const auto targetBefore = ReadBytes(fixture.target);
    std::wstring error;
    const bool recovered = RecoverSaveTransactions(root, error);
    Check(recovered == restored && ReadBytes(fixture.target) == targetBefore &&
        (restored ? OriginalPairRestored(fixture) : !error.empty()),
        "legacy cleanup-only does not require backups; legacy Prepared with a missing backup cannot be guessed safe");
    if (!restored) Check(ReadBytes(fixture.nativeExtension) == nativeBefore &&
        GetFileAttributesW(journal.c_str()) != INVALID_FILE_ATTRIBUTES,
        "invalid legacy recovery preserves remaining metadata and does not delete its evidence");
}

void ValidateDiagnostics(const std::wstring& root) {
    const Fixture fixture = Prepare(root, true);
    InitializeStatsDiagnostics(root.c_str());
    char filename[16] = {};
    std::wstring error;
    Check(BeginMode(fixture, filename, false) && WriteMode(fixture, filename, false) && FinishSaveTransaction(error),
        "persist a real E5S-only transaction with stage diagnostics enabled");
    BattleStatsPersistedState before, after;
    Check(CaptureBattleStatsState(before, GetTickCount64()) && BeginMode(fixture, filename, false) &&
        WriteMode(fixture, filename, false), "capture statistics history before a diagnosed save failure");
    ObserveSaveWrite(1, 0);
    Check(!FinishSaveTransaction(error) && CaptureBattleStatsState(after, GetTickCount64()) &&
        before.historyIncomplete == after.historyIncomplete,
        "save failure diagnostics do not mark combat history incomplete");
    const auto path = PatchPrivatePath(root, CompiledEngineProfile()) + L"\\diagnostics\\stats.log";
    const auto bytes = ReadBytes(path);
    const std::string log(bytes.begin(), bytes.end());
    Check(log.find("captured") != std::string::npos && log.find("embedded_verified") != std::string::npos &&
        log.find("target_committed") != std::string::npos && log.find("save_commit_failed") != std::string::npos &&
        log.find("restored") != std::string::npos && log.find("rollback") != std::string::npos &&
        log.find("extensionRequired=0") != std::string::npos && log.find("writes=5") != std::string::npos,
        "diagnostic evidence records success, rollback, actual extension policy and native I/O observations");
    HANDLE lockedLog = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(lockedLog != INVALID_HANDLE_VALUE && BeginMode(fixture, filename, false) &&
        WriteMode(fixture, filename, false) && FinishSaveTransaction(error),
        "an inaccessible diagnostic log cannot prevent committing a complete save");
    if (lockedLog != INVALID_HANDLE_VALUE) CloseHandle(lockedLog);
    InitializeStatsDiagnostics(nullptr);
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
    if (argc == 6 && !wcscmp(argv[1], L"--state-crash"))
        return StateCrashChild(argv[2], _wtoi(argv[3]), !wcscmp(argv[4], L"1"), argv[5]);
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
    ValidateE5sOnly(root + L"\\e5s-only");
    ValidateFirstPrivateSave(root + L"\\first-private-save");
    ValidatePreparationFailure(root + L"\\snapshot-allocation", 8);
    ValidatePreparationFailure(root + L"\\backup-allocation", 9);
    ValidateEnumerationFailure(root + L"\\enumeration-allocation", 12);
    ValidateEnumerationFailure(root + L"\\enumeration-io", 13);
    ValidateMarkerAndCleanupFailure(root + L"\\restored-marker-failure", false);
    ValidateMarkerAndCleanupFailure(root + L"\\committed-cleanup-failure", true);
    ValidateFailure(root + L"\\commit-marker-failure", 14, false, false, true);
    ValidateStateCompatibility(root + L"\\legacy-prepared", false);
    ValidateStateCompatibility(root + L"\\legacy-committed", true);
    ValidateLegacyRestoredAndMissingBackup(root + L"\\legacy-restored-cleanup", true);
    ValidateLegacyRestoredAndMissingBackup(root + L"\\legacy-missing-backup", false);
    ValidateStrictJournal(root + L"\\invalid-state", L"State", L"Unknown", L"Prepared");
    ValidateStrictJournal(root + L"\\state-bit-mismatch", L"Committed", L"1", L"0");
    ValidateStrictJournal(root + L"\\missing-state", L"State", nullptr, L"Prepared");
    ValidateStrictJournal(root + L"\\invalid-schema", L"SchemaVersion", L"999", L"2");
    for (const bool extension : {false, true}) {
        const std::wstring group = root + (extension ? L"\\extended" : L"\\native-only");
        std::vector<int> rollbackPoints = {21,22,24};
        std::vector<int> commitPoints = {25,24};
        for (int member = 0; member < (extension ? 3 : 1); ++member) {
            rollbackPoints.push_back(200 + member);
            rollbackPoints.push_back(230 + member);
            commitPoints.push_back(230 + member);
        }
        for (const int point : rollbackPoints) {
            ValidateStateCrash(group + L"-rollback-" + std::to_wstring(point), point, extension, L"rollback", false);
            ValidateStateCrash(group + L"-abort-" + std::to_wstring(point), point, extension, L"abort", false);
            ValidateRecoveryCrash(group + L"-recovery-" + std::to_wstring(point), point, extension);
        }
        for (const int point : commitPoints)
            ValidateStateCrash(group + L"-commit-" + std::to_wstring(point), point, extension, L"commit", true);
    }
    ValidateDiagnostics(root + L"\\diagnostics");
    ShutdownBattleStats();
    printf("SaveTransactionsTests: %d failures\n", failures);
    wprintf(L"Temporary test evidence: %s\n", root.c_str());
    return failures ? 1 : 0;
}
