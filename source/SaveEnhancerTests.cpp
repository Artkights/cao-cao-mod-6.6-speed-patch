#include "SaveEnhancer.h"
#include "EngineProfile.h"
#include "Features.h"
#include "BattleStats.h"
#include "BattleStatsPersistence.h"
#include "ScenarioLifecycle.h"
#include "SaveTransactions.h"
#include <filesystem>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

Settings gSettings;
HMODULE gInstance = nullptr;
uintptr_t gGameBase = 0;
HWND gMainWindow = nullptr;
HWND gDialog = nullptr;

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    fflush(stdout);
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
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        if (section.PointerToRawData + static_cast<size_t>(section.SizeOfRawData) > raw.size() ||
            section.VirtualAddress + static_cast<size_t>(section.SizeOfRawData) > image.size())
            return false;
        memcpy(image.data() + section.VirtualAddress, raw.data() + section.PointerToRawData,
            section.SizeOfRawData);
    }
    base = nt->OptionalHeader.ImageBase;
    return true;
}
} // namespace

namespace {
std::wstring gFakeRoot;
int gFakeSaveMode = 0;
WORD gFakeBranches[2] = {0xD06B, 0x016A};
bool gOtherThreadUsedNativeName = false;
int gFakeSaveCalls = 0;
uintptr_t gFakePrivateGame = 0;

bool WriteTestFile(const std::wstring& path, const char* content) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(strlen(content));
    const bool success = WriteFile(file, content, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return success;
}

std::string ReadTestFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    char bytes[256] = {};
    DWORD read = 0;
    const bool success = ReadFile(file, bytes, sizeof(bytes), &read, nullptr) != FALSE;
    CloseHandle(file);
    return success ? std::string(bytes, read) : std::string();
}

void __stdcall FakeFilename(char* output, int rawSlot) {
    const int slot = (rawSlot & 0xFF) == 20 ? 0 : rawSlot + 1;
    sprintf_s(output, 16, "SV\\SV%03d.E5S", slot);
}

DWORD WINAPI OtherSaveThread(void*) {
    char filename[16] = {};
    RouteSaveFilenameForTest(filename, 256);
    gOtherThreadUsedNativeName = strcmp(filename, "SV\\SV257.E5S") == 0;
    return 0;
}

extern "C" void __cdecl FakeSaveBody(void* game, int rawSlot) {
    gFakePrivateGame = reinterpret_cast<uintptr_t>(game);
    ++gFakeSaveCalls;
    if (gFakeSaveMode == 3) {
        gFakeBranches[0] = 0x44EB;
        gFakeBranches[1] = 0x12EB;
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        return;
    }
    char filename[16] = {};
    RouteSaveFilenameForTest(filename, rawSlot);
    if (gFakeSaveMode == 1 || gFakeSaveMode == 2) {
        const std::wstring wide(filename, filename + strlen(filename));
        WriteTestFile(gFakeRoot + L"\\" + wide, "new-native-save");
    }
    HANDLE thread = CreateThread(nullptr, 0, OtherSaveThread, nullptr, 0, nullptr);
    if (thread) { WaitForSingleObject(thread, 10000); CloseHandle(thread); }
    if (gFakeSaveMode == 2 || gFakeSaveMode == 4) CompleteSaveForTest(rawSlot);
}

__declspec(naked) void FakeNativeSave() {
    __asm {
        push dword ptr [esp + 4]
        push ecx
        call FakeSaveBody
        add esp, 8
        ret 4
    }
}

uintptr_t gBoundaryOwner = 0;
int gBoundarySlot = -1;
int gBoundaryCalls = 0;

__declspec(naked) void FakeBoundarySave() {
    __asm {
        mov dword ptr [gBoundaryOwner], ecx
        mov eax, dword ptr [esp + 4]
        mov dword ptr [gBoundarySlot], eax
        inc dword ptr [gBoundaryCalls]
        mov eax, 12345678h
        mov ecx, 23456789h
        mov edx, 3456789Ah
        stc
        ret 4
    }
}

__declspec(naked) void FakeBoundaryAuto() {
    __asm {
        mov dword ptr [gBoundaryOwner], ecx
        inc dword ptr [gBoundaryCalls]
        mov eax, 12345678h
        mov ecx, 23456789h
        mov edx, 3456789Ah
        stc
        ret
    }
}

void ValidateBoundaryBridges() {
    gFeatureSettings.autoSaveTurnStart = false;
    gFeatureSettings.autoSaveTurnEnd = false;
    BYTE owner[4] = {};
    const uintptr_t expectedOwner = reinterpret_cast<uintptr_t>(owner);
    for (int kind = 0; kind < 3; ++kind) {
        const bool saveCall = kind == 2;
        const uintptr_t native = saveCall ? reinterpret_cast<uintptr_t>(&FakeBoundarySave) :
            reinterpret_cast<uintptr_t>(&FakeBoundaryAuto);
        const uintptr_t bridge = ConfigureSaveBoundaryBridgeForTest(kind == 0, saveCall, native);
        DWORD resultEax = 0, resultEcx = 0, resultEdx = 0, resultFlags = 0;
        uintptr_t before = 0, after = 0;
        __asm {
            mov before, esp
            cmp saveCall, 0
            je noSlot
            push 14h
        noSlot:
            mov ecx, expectedOwner
            call dword ptr [bridge]
            pushfd
            pop resultFlags
            mov resultEax, eax
            mov resultEcx, ecx
            mov resultEdx, edx
            mov after, esp
        }
        Check(before == after && gBoundaryOwner == expectedOwner &&
            resultEax == 0x12345678 && resultEcx == 0x23456789 && resultEdx == 0x3456789A &&
            (resultFlags & 1) && (!saveCall || gBoundarySlot == 20),
            "preserve native boundary owner, slot, stack, output registers and flags");
    }
    Check(gBoundaryCalls == 3, "call each original native boundary exactly once when private saves are off");
}

__declspec(naked) void InvokeEndWithNativeFrame(uintptr_t, void*) {
    __asm {
        push ebp
        mov ebp, esp
        push 0
        push dword ptr [ebp + 0Ch]
        push 14h
        mov ecx, 0DEADBEEFh
        call dword ptr [ebp + 8]
        mov esp, ebp
        pop ebp
        ret
    }
}

void ValidateNativeDisabledAndEndOwner() {
    BYTE owner[4] = {0, 0, 7, 0};
    BYTE campaign = 0;
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    gFakeRoot = std::wstring(temp) + L"CczBoundaryOwner-" + std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(gFakeRoot + L"\\SV");
    ConfigureSaveEnhancerForTest(gFakeRoot.c_str(), &campaign,
        reinterpret_cast<uintptr_t>(&FakeNativeSave), reinterpret_cast<uintptr_t>(&FakeFilename));
    ConfigureSaveEnhancerBypassForTest(reinterpret_cast<BYTE*>(&gFakeBranches[0]),
        reinterpret_cast<BYTE*>(&gFakeBranches[1]), 0x44EB, 0x12EB);
    campaign = 1;
    Check(EffectiveSaveSlotForTest(256) == 277 && EffectiveSaveSlotForTest(276) == 0,
        "preserve both native versions' DWORD slot addition and low-byte autosave sentinel");
    campaign = 0;
    gFakeSaveMode = 0;
    gFeatureSettings.autoSaveTurnEnd = true;
    const uintptr_t end = ConfigureSaveBoundaryBridgeForTest(false, true,
        reinterpret_cast<uintptr_t>(&FakeBoundarySave));
    InvokeEndWithNativeFrame(end, owner);
    Check(gBoundaryOwner == 0xDEADBEEF && gBoundarySlot == 20 &&
        gFakePrivateGame == reinterpret_cast<uintptr_t>(owner),
        "recover the private end-save owner from the native caller frame with unrelated ECX");
    gFeatureSettings.autoSaveTurnStart = false;
    gFeatureSettings.autoSaveTurnEnd = false;
    const int calls = gBoundaryCalls;
    for (bool start : {true, false}) {
        BYTE enabled = 0;
        const uintptr_t bridge = ConfigureSaveBoundaryBridgeForTest(start, false,
            reinterpret_cast<uintptr_t>(&FakeBoundaryAuto));
        ConfigureSaveBoundaryEnableForTest(start, &enabled);
        uintptr_t before = 0, after = 0;
        __asm {
            mov before, esp
            lea ecx, owner
            call dword ptr [bridge]
            mov after, esp
        }
        Check(before == after && gBoundaryCalls == calls,
            "skip only the proven broken disabled native-auto branch and keep stack balanced");
    }
    gFakeSaveMode = 3;
    SavePrivateForTest(owner, true);
    Check(gFakeBranches[0] == 0xD06B && gFakeBranches[1] == 0x016A,
        "restore both versions' native writer branches after an exceptional private save");
    const int savesBeforeRestore = gFakeSaveCalls;
    RestoreSaveEnhancer();
    SavePrivateForTest(owner, true);
    Check(gFakeSaveCalls == savesBeforeRestore,
        "disable private writers before restoring any dependent hook");
    ConfigureSaveEnhancerForTest(nullptr, nullptr, 0, 0);
    gFakeSaveCalls = 0;
    std::filesystem::remove_all(gFakeRoot);
}

void ValidatePrivateSaveTransactions65() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    gFakeRoot = std::wstring(temp) + L"CczSaveTransaction-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    std::filesystem::create_directories(gFakeRoot + L"\\SV");
    const std::wstring privateRoot = PatchPrivatePath(gFakeRoot, CompiledEngineProfile());
    std::filesystem::create_directories(privateRoot + L"\\autosave");
    const std::wstring target = privateRoot + L"\\autosave\\start-007";
    WriteTestFile(target + L".E5S", "old-save");
    WriteTestFile(target + L".EXS", "old-extension");
    WriteTestFile(gFakeRoot + L"\\SV\\SV001.E5S", "manual-save");
    WritePrivateProfileStringW(L"Save", L"Turn", L"old", (target + L".ini").c_str());
    BYTE campaign = 0;
    BYTE game[4] = {0, 0, 7, 0};
    ConfigureSaveEnhancerForTest(gFakeRoot.c_str(), &campaign,
        reinterpret_cast<uintptr_t>(&FakeNativeSave), reinterpret_cast<uintptr_t>(&FakeFilename));
    ConfigureSaveEnhancerBypassForTest(reinterpret_cast<BYTE*>(&gFakeBranches[0]),
        reinterpret_cast<BYTE*>(&gFakeBranches[1]), 0x44EB, 0x12EB);
    gFeatureSettings.saveScreenshots = false;
    gFeatureSettings.autoSaveSlots = 10;
    char ordinary[16] = {};
    RouteSaveFilenameForTest(ordinary, 256);
    Check(strcmp(ordinary, "SV\\SV257.E5S") == 0,
        "route an ordinary same-slot request through the native filename ABI");
    Check(EffectiveSaveSlotForTest(256) == 257 && EffectiveSaveSlotForTest(276) == 0,
        "preserve the native DWORD slot addition and low-byte autosave sentinel");
    for (int mode : {0, 1, 4}) {
        gFakeSaveMode = mode;
        SavePrivateForTest(game, true);
        wchar_t turn[32] = {};
        GetPrivateProfileStringW(L"Save", L"Turn", L"", turn, 32, (target + L".ini").c_str());
        Check(ReadTestFile(target + L".E5S") == "old-save" && wcscmp(turn, L"old") == 0,
            "preserve the old rotation entry when native success or a new file is missing");
    }
    gFakeSaveMode = 3;
    SavePrivateForTest(game, true);
    Check(gFakeBranches[0] == 0xD06B && gFakeBranches[1] == 0x016A &&
        ReadTestFile(target + L".E5S") == "old-save",
        "restore both native metadata branches after an exceptional save");
    gFakeSaveMode = 2;
    HANDLE lockedTarget = CreateFileW((target + L".E5S").c_str(), GENERIC_READ, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SavePrivateForTest(game, true);
    if (lockedTarget != INVALID_HANDLE_VALUE) CloseHandle(lockedTarget);
    wchar_t turn[32] = {};
    GetPrivateProfileStringW(L"Save", L"Turn", L"", turn, 32, (target + L".ini").c_str());
    Check(ReadTestFile(target + L".E5S") == "old-save" && wcscmp(turn, L"old") == 0,
        "preserve the old rotation metadata when the new file cannot be moved");
    SavePrivateForTest(game, true);
    GetPrivateProfileStringW(L"Save", L"Turn", L"", turn, 32, (target + L".ini").c_str());
    Check(ReadTestFile(target + L".E5S") == "new-native-save" && wcscmp(turn, L"7") == 0,
        "commit only a newly written and successful native save");
    Check(gOtherThreadUsedNativeName && gFakeSaveCalls == 6,
        "keep concurrent filename requests outside the private thread transaction");
    Check(ReadTestFile(gFakeRoot + L"\\SV\\SV001.E5S") == "manual-save" &&
        GetFileAttributesW((target + L".EXS").c_str()) == INVALID_FILE_ATTRIBUTES,
        "preserve numbered player saves and discard stale optional extension data");
    ConfigureSaveEnhancerForTest(nullptr, nullptr, 0, 0);
    std::filesystem::remove_all(gFakeRoot);
}

uintptr_t gLoadResultBridge = 0;
DWORD gLoadSucceeded = 1;
DWORD gLoadEntryEax = 0, gLoadEntryEcx = 0, gLoadEntryEdx = 0, gLoadEntryFlags = 0;
DWORD gLoadEntryLocal1 = 1, gLoadEntryLocal2 = 1, gLoadEntrySlot = 0;
char gLoadFilenameOverride[16] = {};
bool gLoadMidpointFrozen = false;

void __stdcall FakeLoadFilename(char* output, int rawSlot) {
    if (*gLoadFilenameOverride) strcpy_s(output, 16, gLoadFilenameOverride);
    else FakeFilename(output, rawSlot);
}

bool SameLedger(const std::vector<BattleUnitTotals>& left,
    const std::vector<BattleUnitTotals>& right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.personId != b.personId || a.slot != b.slot || a.label != b.label ||
            a.damageDealt != b.damageDealt || a.damageReceived != b.damageReceived ||
            a.recoveryDone != b.recoveryDone || a.criticalCount != b.criticalCount ||
            a.comboExtraHitCount != b.comboExtraHitCount ||
            a.attackBlockCount != b.attackBlockCount || a.spellBlockCount != b.spellBlockCount ||
            a.defeatedEnemies != b.defeatedEnemies) return false;
    }
    return true;
}

bool HasSavedLedger(const BattleStatsSnapshot& actual, const BattleStatsPersistedState& expected) {
    return actual.scenarioOrdinal == expected.scenarioOrdinal &&
        actual.campaignId == expected.campaignId && actual.battleInstance == expected.battleInstance &&
        actual.scenario == expected.scenario && actual.active == expected.active &&
        SameLedger(actual.units, expected.units) &&
        SameLedger(actual.campaignUnits, expected.campaignUnits);
}

extern "C" void __cdecl InspectNativeLoadMidpoint() {
    BattleStatsSnapshot snapshot;
    BattleStatsPersistedState before, after;
    ReadBattleStatsSnapshot(snapshot);
    CaptureBattleStatsState(before, GetTickCount64());
    // Native loading can transiently write HP or install units. These must
    // not become battle events, elapsed time, or a new scenario instance.
    RecordBattleStatsAttackBlockForTest(100, 2);
    RecordBattleStatsSpellBlockForTest(100, 2);
    PollBattleStats(GetTickCount64() + 60000);
    CompleteScenarioInstallForTest(true, 0x2222, 0, GetTickCount64() + 60000);
    CaptureBattleStatsState(after, GetTickCount64() + 60000);
    gLoadMidpointFrozen = snapshot.loading && ScenarioLoadInProgress() &&
        before.elapsedMilliseconds == after.elapsedMilliseconds &&
        before.battleInstance == after.battleInstance &&
        SameLedger(before.units, after.units) && SameLedger(before.campaignUnits, after.campaignUnits);
    char path[16] = {};
    RouteLoadFilenameForTest(path, static_cast<int>(gLoadEntrySlot));
}

__declspec(naked) void NativeLoadContinuation() {
    __asm {
        mov dword ptr [gLoadEntryEax], eax
        mov dword ptr [gLoadEntryEcx], ecx
        mov dword ptr [gLoadEntryEdx], edx
        pushfd
        pop dword ptr [gLoadEntryFlags]
        mov eax, dword ptr [ebp - 4]
        mov dword ptr [gLoadEntryLocal1], eax
        mov eax, dword ptr [ebp - 8]
        mov dword ptr [gLoadEntryLocal2], eax
        mov eax, dword ptr [ebp + 8]
        mov dword ptr [gLoadEntrySlot], eax
        call InspectNativeLoadMidpoint
        mov eax, 12345600h
        or al, byte ptr [gLoadSucceeded]
        mov ecx, 23456789h
        mov edx, 3456789Ah
        mov ebx, 456789ABh
        mov esi, 56789ABCh
        mov edi, 6789ABCDh
        stc
        jmp dword ptr [gLoadResultBridge]
    }
}

void InvokeNativeLoad(uintptr_t start, int rawSlot, bool succeeded) {
    gLoadSucceeded = succeeded ? 1 : 0;
    gLoadMidpointFrozen = false;
    DWORD eaxResult = 0, ecxResult = 0, edxResult = 0, ebxResult = 0;
    DWORD esiResult = 0, ediResult = 0, flagsResult = 0;
    uintptr_t before = 0, after = 0;
    __asm {
        push ebx
        push esi
        push edi
        mov before, esp
        push rawSlot
        mov eax, 10203040h
        mov ecx, 20304050h
        mov edx, 30405060h
        stc
        call dword ptr [start]
        mov eaxResult, eax
        mov ecxResult, ecx
        mov edxResult, edx
        mov ebxResult, ebx
        mov esiResult, esi
        mov ediResult, edi
        pushfd
        pop flagsResult
        mov after, esp
        pop edi
        pop esi
        pop ebx
    }
    Check(before == after && eaxResult == (succeeded ? 0x12345601u : 0x12345600u) &&
        ecxResult == 0x23456789 && edxResult == 0x3456789A && ebxResult == 0x456789AB &&
        esiResult == 0x56789ABC && ediResult == 0x6789ABCD && (flagsResult & 1),
        "execute real load result bridge with native AL result, registers, flags and RET 4 ABI");
    Check(gLoadEntryEax == 0x10203040 && gLoadEntryEcx == 0x20304050 &&
        gLoadEntryEdx == 0x30405060 && (gLoadEntryFlags & 1) &&
        gLoadEntrySlot == static_cast<DWORD>(rawSlot) && !gLoadEntryLocal1 && !gLoadEntryLocal2,
        "execute real load entry bridge preserving inputs and replaying both native zero locals");
    Check(gLoadMidpointFrozen, "freeze timer, event ledgers and scenario lifecycle throughout native loading");
}

BattleStatsPersistedState MakeLoadState(uint64_t campaign, long long current, long long total) {
    BattleStatsPersistedState state;
    state.scenarioOrdinal = 1;
    state.campaignId = campaign;
    state.battleInstance = campaign + 10;
    state.knownMetrics = 0xFF;
    state.active = true;
    state.elapsedMilliseconds = 987654321;
    state.scenario = L"S_00.eex";
    BattleUnitTotals unit;
    unit.slot = 2; unit.personId = 1002; unit.label = L"曹操";
    unit.damageDealt = current; unit.damageReceived = current + 1;
    unit.recoveryDone = current + 2; unit.criticalCount = current + 3;
    unit.comboExtraHitCount = current + 4; unit.attackBlockCount = current + 5;
    unit.spellBlockCount = current + 6; unit.defeatedEnemies = 7;
    state.units.push_back(unit);
    unit.damageDealt = total; unit.damageReceived = total + 1;
    unit.recoveryDone = total + 2; unit.criticalCount = total + 3;
    unit.comboExtraHitCount = total + 4; unit.attackBlockCount = total + 5;
    unit.spellBlockCount = total + 6; unit.defeatedEnemies = 70;
    state.campaignUnits.push_back(unit);
    return state;
}

void ValidateSynchronousLoadStatistics() {
    wchar_t temporary[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temporary);
    const std::filesystem::path root = std::filesystem::path(temporary) /
        (L"CczSaveLoad-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root / L"SV");
    const std::wstring rootText = root.wstring();
    BYTE campaign = 0;
    WORD ordinal = 1;
    std::vector<BYTE> units(0x30 * 250, 0);
    for (unsigned slot = 0; slot < 250; ++slot) {
        const WORD person = static_cast<WORD>(1000 + slot);
        memcpy(units.data() + slot * 0x30, &person, sizeof(person));
    }
    units[100 * 0x30 + 5] = 2;
    InitializeBattleStatsModelForTest(reinterpret_cast<uintptr_t>(units.data()));
    ConfigureScenarioLifecycleForTest(&ordinal, 0x1111, 0x2222, 0x3333);
    ConfigureSaveEnhancerForTest(rootText.c_str(), &campaign,
        reinterpret_cast<uintptr_t>(&FakeNativeSave), reinterpret_cast<uintptr_t>(&FakeLoadFilename));
    uintptr_t loadStart = 0;
    ConfigureSaveLoadBridgesForTest(reinterpret_cast<uintptr_t>(&NativeLoadContinuation),
        loadStart, gLoadResultBridge);
    const auto stateA = MakeLoadState(101, 123, 12345);
    const auto stateB = MakeLoadState(202, 456, 45678);
    std::wstring error;
    const auto saveA = (root / L"SV" / L"SV001.E5S").wstring();
    const auto saveB = (root / L"SV" / L"SV002.E5S").wstring();
    Check(WriteTestFile(saveA, "native-A") && WriteBattleStatsEmbedded(saveA, stateA, error) &&
        WriteTestFile(saveB, "native-B") && WriteBattleStatsEmbedded(saveB, stateB, error),
        "prepare two save files with different embedded battle and campaign ledgers");
    Check(RestoreBattleStatsState(stateB, GetTickCount64()), "seed a previous campaign before loading another save");
    InvokeNativeLoad(loadStart, 0, true);
    BattleStatsSnapshot snapshot;
    ReadBattleStatsSnapshot(snapshot);
    Check(!snapshot.loading && !ScenarioLoadInProgress() && HasSavedLedger(snapshot, stateA),
        "restore both saved ledgers synchronously before native load returns without polling");
    InvokeNativeLoad(loadStart, 1, false);
    ReadBattleStatsSnapshot(snapshot);
    Check(!snapshot.loading && !ScenarioLoadInProgress() && HasSavedLedger(snapshot, stateA),
        "keep successful save A after a subsequent failed native load B");

    Check(RestoreBattleStatsState(stateB, GetTickCount64()), "seed a ledger before nested native loads");
    char nestedPath[16] = {};
    ObserveSaveLoadForTest(0);
    RouteLoadFilenameForTest(nestedPath, 0);
    ObserveSaveLoadForTest(1);
    RouteLoadFilenameForTest(nestedPath, 1);
    CompleteSaveLoadForTest(false);
    ReadBattleStatsSnapshot(snapshot);
    Check(snapshot.loading && ScenarioLoadInProgress() && HasSavedLedger(snapshot, stateB),
        "a failed inner load keeps the outer load frozen and preserves the preceding ledger");
    CompleteSaveLoadForTest(true);
    ReadBattleStatsSnapshot(snapshot);
    Check(!snapshot.loading && !ScenarioLoadInProgress() && HasSavedLedger(snapshot, stateA),
        "a successful outer load restores its own filename after a failed inner load");

    ObserveSaveLoadForTest(0);
    RouteLoadFilenameForTest(nestedPath, 0);
    ObserveSaveLoadForTest(1);
    RouteLoadFilenameForTest(nestedPath, 1);
    CompleteSaveLoadForTest(true);
    ReadBattleStatsSnapshot(snapshot);
    Check(snapshot.loading && ScenarioLoadInProgress() && HasSavedLedger(snapshot, stateB),
        "an inner successful load restores immediately while the outer load remains frozen");
    CompleteSaveLoadForTest(false);
    ReadBattleStatsSnapshot(snapshot);
    Check(!snapshot.loading && !ScenarioLoadInProgress() && HasSavedLedger(snapshot, stateB),
        "a failed outer load retains a completed inner save instead of stale earlier data");

    // No sidecar is copied. The actual filename hook must select the trailer
    // in the renamed file, even though the nominal slot resolves elsewhere.
    const auto copied = (root / L"SV" / L"COPIED.E5S").wstring();
    Check(CopyFileW(saveA.c_str(), copied.c_str(), FALSE) != FALSE,
        "copy a save containing embedded statistics to an unrelated filename");
    strcpy_s(gLoadFilenameOverride, "SV\\COPIED.E5S");
    InvokeNativeLoad(loadStart, 1, true);
    gLoadFilenameOverride[0] = 0;
    ReadBattleStatsSnapshot(snapshot);
    Check(HasSavedLedger(snapshot, stateA),
        "restore embedded statistics from the actual copied or renamed native filename");

    const auto oldSave = (root / L"SV" / L"SV003.E5S").wstring();
    Check(WriteTestFile(oldSave, "native-old-save-without-statistics"), "prepare an old save without a statistics extension");
    InvokeNativeLoad(loadStart, 2, true);
    ReadBattleStatsSnapshot(snapshot);
    Check(snapshot.units.empty() && snapshot.campaignUnits.empty() && snapshot.historyIncomplete &&
        !snapshot.status.empty() && !snapshot.loading,
        "start empty ledgers with an explicit incomplete-history message for old saves");

    InvokeNativeLoad(loadStart, 0, true);
    const auto corrupt = (root / L"SV" / L"CORRUPT.E5S").wstring();
    CopyFileW(saveB.c_str(), corrupt.c_str(), FALSE);
    HANDLE file = CreateFileW(corrupt.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    bool corrupted = false;
    if (file != INVALID_HANDLE_VALUE) {
        const BYTE changed = 0x7F;
        DWORD written = 0;
        corrupted = SetFilePointer(file, -1, nullptr, FILE_END) != INVALID_SET_FILE_POINTER &&
            WriteFile(file, &changed, 1, &written, nullptr) && written == 1;
        CloseHandle(file);
    }
    Check(corrupted, "prepare a corrupt embedded statistics footer");
    strcpy_s(gLoadFilenameOverride, "SV\\CORRUPT.E5S");
    InvokeNativeLoad(loadStart, 1, true);
    gLoadFilenameOverride[0] = 0;
    ReadBattleStatsSnapshot(snapshot);
    Check(snapshot.units.empty() && snapshot.campaignUnits.empty() && snapshot.historyIncomplete &&
        !snapshot.status.empty(),
        "prevent successful native loads with corrupt statistics from inheriting another save's data");

    ConfigureSaveEnhancerForTest(nullptr, nullptr, 0, 0);
    ShutdownScenarioLifecycle();
    ShutdownBattleStats();
    std::filesystem::remove_all(root);
}

uintptr_t gTestSaveResultBridge = 0;
DWORD gTestSaveResultSucceeded = 0;
DWORD gCommitPostCalls = 0, gCommitPostArgument = 1, gCommitPostFlags = 0;
DWORD gCommitPostEax = 0, gCommitPostEcx = 0, gCommitPostEdx = 0;
DWORD gCommitOutputEax = 0, gCommitOutputEcx = 0, gCommitOutputEdx = 0;
DWORD gCommitOutputEbx = 0, gCommitOutputEsi = 0, gCommitOutputEdi = 0;
DWORD gCommitOutputFlags = 0, gCommitContinuationKind = 0;

__declspec(naked) void FakeCommitPostWrite() {
    __asm {
        mov dword ptr [gCommitPostEax], eax
        mov dword ptr [gCommitPostEcx], ecx
        mov dword ptr [gCommitPostEdx], edx
        pushfd
        pop dword ptr [gCommitPostFlags]
        mov eax, dword ptr [esp + 4]
        mov dword ptr [gCommitPostArgument], eax
        inc dword ptr [gCommitPostCalls]
        mov eax, 89ABCDEFh
        mov ecx, 9ABCDEF0h
        mov edx, 0ABCDEF01h
        stc
        ret
    }
}

__declspec(naked) void NativeCommitSucceededContinuation() {
    __asm {
        mov dword ptr [gCommitContinuationKind], 1
        mov dword ptr [gCommitOutputEax], eax
        mov dword ptr [gCommitOutputEcx], ecx
        mov dword ptr [gCommitOutputEdx], edx
        mov dword ptr [gCommitOutputEbx], ebx
        mov dword ptr [gCommitOutputEsi], esi
        mov dword ptr [gCommitOutputEdi], edi
        pushfd
        pop dword ptr [gCommitOutputFlags]
        // This is the original continuation's caller cleanup for PUSH 0.
        add esp, 4
        ret
    }
}

__declspec(naked) void NativeCommitFailedContinuation() {
    __asm {
        mov dword ptr [gCommitContinuationKind], 2
        mov dword ptr [gCommitOutputEax], eax
        mov dword ptr [gCommitOutputEcx], ecx
        mov dword ptr [gCommitOutputEdx], edx
        mov dword ptr [gCommitOutputEbx], ebx
        mov dword ptr [gCommitOutputEsi], esi
        mov dword ptr [gCommitOutputEdi], edi
        pushfd
        pop dword ptr [gCommitOutputFlags]
        ret
    }
}

void InvokeProductionSaveCommit(uintptr_t bridge, bool success) {
    const DWORD calls = gCommitPostCalls;
    gCommitContinuationKind = 0;
    uintptr_t before = 0, after = 0;
    __asm {
        push ebx
        push esi
        push edi
        mov before, esp
        mov eax, 11223344h
        mov ecx, 22334455h
        mov edx, 33445566h
        mov ebx, 44556677h
        mov esi, 55667788h
        mov edi, 66778899h
        stc
        call dword ptr [bridge]
        mov after, esp
        pop edi
        pop esi
        pop ebx
    }
    Check(before == after && gCommitOutputEbx == 0x44556677 &&
        gCommitOutputEsi == 0x55667788 && gCommitOutputEdi == 0x66778899,
        "execute the real commit bridge with preserved nonvolatile registers and balanced native cleanup");
    if (success) {
        Check(gCommitContinuationKind == 1 && gCommitPostCalls == calls + 1 && !gCommitPostArgument &&
            gCommitPostEax == 0x11223344 && gCommitPostEcx == 0x22334455 &&
            gCommitPostEdx == 0x33445566 && (gCommitPostFlags & 1) &&
            gCommitOutputEax == 0x89ABCDEF && gCommitOutputEcx == 0x9ABCDEF0 &&
            gCommitOutputEdx == 0xABCDEF01 && (gCommitOutputFlags & 1),
            "successful commit calls the original post-write ABI exactly once and keeps its outputs");
    } else {
        Check(gCommitContinuationKind == 2 && gCommitPostCalls == calls && !gCommitOutputEax &&
            gCommitOutputEcx == 0x22334455 && gCommitOutputEdx == 0x33445566 &&
            !(gCommitOutputFlags & 1) && (gCommitOutputFlags & 0x40),
            "failed commit bypasses native success UI and returns zero with the failure branch ABI");
    }
}

__declspec(naked) void NativeSaveResultFrame() {
    __asm {
        push ebp
        mov ebp, esp
        sub esp, 8
        mov eax, 12345600h
        or al, byte ptr [gTestSaveResultSucceeded]
        mov ecx, 23456789h
        mov edx, 3456789Ah
        mov ebx, 456789ABh
        mov esi, 56789ABCh
        mov edi, 6789ABCDh
        stc
        jmp dword ptr [gTestSaveResultBridge]
    }
}

void InvokeProductionSaveResult(int rawSlot, bool succeeded) {
    gTestSaveResultSucceeded = succeeded ? 1 : 0;
    DWORD eaxResult = 0, ecxResult = 0, edxResult = 0, ebxResult = 0;
    DWORD esiResult = 0, ediResult = 0, flagsResult = 0;
    uintptr_t before = 0, after = 0;
    __asm {
        push ebx
        push esi
        push edi
        mov before, esp
        push rawSlot
        call NativeSaveResultFrame
        mov eaxResult, eax
        mov ecxResult, ecx
        mov edxResult, edx
        mov ebxResult, ebx
        mov esiResult, esi
        mov ediResult, edi
        pushfd
        pop flagsResult
        mov after, esp
        pop edi
        pop esi
        pop ebx
    }
    Check(before == after && eaxResult == (succeeded ? 0x12345601u : 0x12345600u) &&
        ecxResult == 0x23456789 && edxResult == 0x3456789A && ebxResult == 0x456789AB &&
        esiResult == 0x56789ABC && ediResult == 0x6789ABCD && (flagsResult & 1),
        "execute the real save result bridge preserving AL, registers, carry and RET 4 on every native return");
}

bool StageCommitBridgeSave(const std::wstring& root, bool complete) {
    char filename[16] = {};
    RouteSaveFilenameForTest(filename, 0);
    if (!SaveTransactionActive() || strncmp(filename, "SV\\X", 4)) return false;
    const std::wstring temporary = root + L"\\" + std::wstring(filename, filename + strlen(filename));
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool okay = true;
    const char* chunks[] = {"native-header|", "native-main|", "native-people|", "native-end|"};
    for (unsigned index = 0; index < (complete ? 4u : 1u); ++index) {
        const DWORD requested = static_cast<DWORD>(strlen(chunks[index]));
        DWORD written = 0;
        const bool result = WriteFile(file, chunks[index], requested, &written, nullptr) != FALSE;
        ObserveSaveWrite(requested, result ? written : 0);
        okay = result && written == requested && okay;
    }
    const bool flushed = FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    if (complete) ObserveSaveClose(flushed && closed);
    if (CompiledEngineProfile() == EngineProfileId::Ccz66 && complete) {
        const bool extension = WriteTestFile(root + L"\\SV\\SV001.EXS", "new-native-extension");
        ObserveSaveExtension(extension);
        okay = extension && okay;
    }
    return okay && flushed && closed;
}

void ValidateProductionSaveCommitAndResultBridges() {
    wchar_t temporary[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temporary);
    const auto root = std::filesystem::path(temporary) /
        (L"CczCommitBridge-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root / L"SV");
    const auto rootText = root.wstring();
    const auto save = (root / L"SV" / L"SV001.E5S").wstring();
    const auto extension = (root / L"SV" / L"SV001.EXS").wstring();
    BYTE campaign = 0;
    ConfigureSaveEnhancerForTest(rootText.c_str(), &campaign,
        reinterpret_cast<uintptr_t>(&FakeNativeSave), reinterpret_cast<uintptr_t>(&FakeFilename));
    SetSaveTransactionalHooksForTest(true);
    SetSaveFailureDialogForTest(false);
    const uintptr_t commit = ConfigureSaveCommitBridgeForTest(
        reinterpret_cast<uintptr_t>(&FakeCommitPostWrite),
        reinterpret_cast<uintptr_t>(&NativeCommitSucceededContinuation),
        reinterpret_cast<uintptr_t>(&NativeCommitFailedContinuation));
    gTestSaveResultBridge = SaveResultBridgeForTest();
    gCommitPostCalls = 0;
    auto state = MakeLoadState(303, 99, 999);
    state.active = false;
    std::wstring error;
    Check(WriteTestFile(save, "old-native") && WriteTestFile(extension, "old-extension") &&
        RestoreBattleStatsState(state, GetTickCount64()) && StageCommitBridgeSave(rootText, true),
        "stage a real transactional filename and four complete native writes before the production commit bridge");
    InvokeProductionSaveCommit(commit, true);
    BattleStatsPersistedState persisted;
    Check(!SaveTransactionActive() && ReadBattleStatsEmbedded(save, persisted, error) == BattleStatsReadResult::Loaded &&
        SameLedger(persisted.units, state.units) && SameLedger(persisted.campaignUnits, state.campaignUnits),
        "the production commit bridge persists both snapshotted ledgers with the native save");
    InvokeProductionSaveResult(0, true);
    Check(RecoverSaveTransactions(rootText, error), "successful save result leaves no active transaction lock");

    const std::string committedPrefix = ReadTestFile(save);
    const std::string committedExtension = ReadTestFile(extension);
    Check(StageCommitBridgeSave(rootText, true), "stage another complete native save before an injected commit failure");
    SetSaveTransactionFaultForTest(1);
    InvokeProductionSaveCommit(commit, false);
    SetSaveTransactionFaultForTest(0);
    InvokeProductionSaveResult(0, false);
    Check(!SaveTransactionActive() && ReadTestFile(save) == committedPrefix &&
        ReadTestFile(extension) == committedExtension && RecoverSaveTransactions(rootText, error),
        "commit failure restores the previous save group and releases its lock before the native failure return");

    Check(StageCommitBridgeSave(rootText, false) && SaveTransactionActive(),
        "stage a native writer that returns failure before reaching any commit bridge");
    InvokeProductionSaveResult(0, false);
    Check(!SaveTransactionActive() && ReadTestFile(save) == committedPrefix &&
        ReadTestFile(extension) == committedExtension && RecoverSaveTransactions(rootText, error),
        "AL zero native result aborts an uncommitted save and releases the journal and directory lock");
    Check(StageCommitBridgeSave(rootText, true), "a fresh save can start immediately after native AL zero cleanup");
    InvokeProductionSaveCommit(commit, true);
    InvokeProductionSaveResult(0, true);
    Check(!SaveTransactionActive() && RecoverSaveTransactions(rootText, error),
        "a later complete save commits after an earlier native failure without stale transaction state");
    SetSaveFailureDialogForTest(true);
    SetSaveTransactionalHooksForTest(false);
    ConfigureSaveEnhancerForTest(nullptr, nullptr, 0, 0);
    ShutdownBattleStats();
    std::filesystem::remove_all(root);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && (!wcscmp(argv[1], L"--layout-only") || !wcscmp(argv[2], L"--layout-only"))) {
        const wchar_t* path = !wcscmp(argv[1], L"--layout-only") ? argv[2] : argv[1];
        std::vector<BYTE> image;
        DWORD base = 0;
        Check(LoadImage(path, image, base), "load x86 game image for read-only compatibility inspection");
        if (failures) return 1;
        printf("profile=%u imageBase=%08X\n", static_cast<unsigned>(CompiledEngineProfile()), base);
        SaveEnhancerImageLayout save;
        const bool saveResolved = ResolveSaveEnhancerImageForTest(image.data(), base, save);
        Check(saveResolved, "resolve production save/load transaction graph without fixed sample addresses");
        printf("save commit=%08X success=%08X failure=%08X post=%08X result=%08X filename=%08X "
            "loadStart=%08X loadResult=%08X write=%08X close=%08X autoStart=%08X autoEnd=%08X\n",
            static_cast<unsigned>(save.commitSite), static_cast<unsigned>(save.commitContinuation),
            static_cast<unsigned>(save.commitFailure), static_cast<unsigned>(save.nativePostWrite),
            static_cast<unsigned>(save.saveResult), static_cast<unsigned>(save.nativeFilename),
            static_cast<unsigned>(save.loadStartSite), static_cast<unsigned>(save.loadResult),
            static_cast<unsigned>(save.nativeWrite), static_cast<unsigned>(save.nativeClose),
            static_cast<unsigned>(save.startCall), static_cast<unsigned>(save.endCall));
        ScenarioLifecycleImageLayout scenario;
        const bool scenarioResolved = ResolveScenarioLifecycleForTest(image.data(), base, scenario);
        Check(scenarioResolved, "resolve production scenario lifecycle graph without fixed sample addresses");
        printf("scenario install=%08X result=%08X continue=%08X ordinal=%08X accessor=%08X "
            "initial=%08X progress=%08X reload=%08X menu=%08X\n",
            static_cast<unsigned>(scenario.nativeInstall), static_cast<unsigned>(scenario.installedSite),
            static_cast<unsigned>(scenario.installedContinuation), static_cast<unsigned>(scenario.ordinalAddress),
            static_cast<unsigned>(scenario.ordinalAccessor), static_cast<unsigned>(scenario.initialCall),
            static_cast<unsigned>(scenario.progressCall), static_cast<unsigned>(scenario.reloadCall),
            static_cast<unsigned>(scenario.menuFunction));
        printf("SaveEnhancerLayoutOnly: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    if (argc != 2) return 2;
    std::vector<BYTE> image;
    DWORD base = 0;
    Check(LoadImage(argv[1], image, base), "load x86 game image");
    SaveEnhancerImageLayout layout;
    const bool resolved = ResolveSaveEnhancerImageForTest(image.data(), base, layout);
    Check(resolved,
        "resolve native save and load result graph");
    const bool engine65 = CompiledEngineProfile() == EngineProfileId::Ccz65;
    Check(layout.nativeAuto == (engine65 ? 0x40CB83u : 0x412AB7u) && layout.nativeSave == 0x412AE5 &&
        layout.saveResult == 0x405314, "resolve successful native save completion");
    Check(layout.loadObserve == 0x4188A4 && layout.loadResult == 0x4189BA &&
        layout.loadContinuation == 0 &&
        layout.campaignByte == (engine65 ? 0x4CE78Fu : 0x500ACFu),
        "resolve load slot before mutation and successful load completion");
    Check(layout.commitSite == 0x4052E7 && layout.commitContinuation == 0x4052EE &&
        layout.commitFailure == 0x405307 && layout.nativePostWrite == 0x42B81B &&
        layout.loadStartSite == 0x41888D && layout.loadStartContinuation == 0x418894 &&
        layout.nativeWrite == 0x41923C && layout.nativeClose == 0x4192C5,
        "resolve transaction commit before native success and load entry before any mutation");

    if (!engine65) Check(layout.startCall == 0x44E1A2 && layout.endCall == 0x44E1EC,
        "resolve separate native 6.6 start and confirmed end-save calls");
    if (engine65) Check(layout.startCall == 0x44E1A2 && layout.endCall == 0x44E1B9 &&
        layout.nativeEndAuto == 0x40CBB6 && layout.nativeFilename == 0x4288F4 &&
        layout.saveFilenameCall == 0x40532B,
        "resolve distinct native start/end boundaries and the bounded filename ABI");
    if (resolved) {
        const SaveEnhancerImageLayout resolvedLayout = layout;
        std::vector<BYTE> changed = image;
        changed[resolvedLayout.loadResult - base] = 0x90;
        Check(!ResolveSaveEnhancerImageForTest(changed.data(), base, layout),
            "reject a modified load completion instead of installing a partial hook");
        changed = image;
        changed[resolvedLayout.saveResult - base] = 0x90;
        Check(!ResolveSaveEnhancerImageForTest(changed.data(), base, layout),
            "reject a modified save completion instead of installing a partial hook");
        if (engine65) for (uintptr_t native : {resolvedLayout.nativeAuto, resolvedLayout.nativeEndAuto}) {
            changed = image;
            changed[native - base + 46] = 0x90;
            Check(ResolveSaveEnhancerImageForTest(changed.data(), base, layout) &&
                !layout.nativeAuto && !layout.nativeEndAuto && !layout.startCall && !layout.endCall &&
                layout.commitSite == resolvedLayout.commitSite &&
                layout.loadStartSite == resolvedLayout.loadStartSite,
                "keep required save/load persistence available while disabling an unknown autosave graph");
        }
        changed = image;
        changed[resolvedLayout.loadStartSite - base + 3] = 0x90;
        Check(!ResolveSaveEnhancerImageForTest(changed.data(), base, layout),
            "reject a modified load prologue before replaying native entry instructions");
        changed = image;
        changed[resolvedLayout.loadObserve - base] = 0x90;
        Check(!ResolveSaveEnhancerImageForTest(changed.data(), base, layout),
            "reject a modified native load filename call before observing the actual E5S path");
        changed = image;
        changed[resolvedLayout.commitSite - base] = 0x90;
        Check(!ResolveSaveEnhancerImageForTest(changed.data(), base, layout),
            "reject an unverified commit site before reporting native save success");
    }
    ValidateBoundaryBridges();
    ValidateNativeDisabledAndEndOwner();
    if (engine65) ValidatePrivateSaveTransactions65();
    ValidateSynchronousLoadStatistics();
    ValidateProductionSaveCommitAndResultBridges();
    printf("SaveEnhancerTests: %d failures\n", failures);
    return failures ? 1 : 0;
}

