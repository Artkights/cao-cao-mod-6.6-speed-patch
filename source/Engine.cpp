#include "Patch.h"
#include "EngineLayout.h"
#include "PeImage.h"
#include "Features.h"
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "winmm.lib")

namespace {
const uintptr_t kPreferredBase = 0x400000;
struct Site {
    uintptr_t address;
    size_t length;
    BYTE original[8];
    BYTE current[8];
    unsigned capability;
    bool available;
};
struct RuntimeJournalEntry {
    bool active = false;
    bool expectedValid = false;
    uintptr_t address = 0;
    size_t length = 0;
    BYTE original[8] = {};
    BYTE expected[8] = {};
};
Site gSites[] = {
    {0x42D5B0, 4, {0x55, 0x8B, 0xEC, 0xE8}, {}, CapabilitySuperDialog, true},
    {0x4332A6, 4, {0x55, 0x8B, 0xEC, 0x6A}, {}, CapabilitySuperMapText, true},
    {0x422DBA, 3, {0x55, 0x8B, 0xEC}, {}, CapabilitySuperDelay, true},
    {0x429964, 5, {0xE8, 0x7C, 0x2B, 0x00, 0x00}, {}, CapabilitySuperR, true},
    {0x42C4E8, 5, {0xE8, 0xF3, 0x8B, 0x04, 0x00}, {}, CapabilitySuperS | CapabilityPhase, true},
    {0x413324, 1, {0x10}, {}, CapabilityInput, true},
    {0x42DB2C, 4, {0x55, 0x8B, 0xEC, 0x6A}, {}, CapabilityDialog3, true},
    {0x415B40, 2, {0xD1, 0xE0}, {}, CapabilityExtraDelay, true},
    {0x42C4EF, 2, {0x75, 0x07}, {}, CapabilitySuperS, true},
    {0x42C812, 2, {0xE8, 0x29}, {}, CapabilityDialogDisplay, true},
    {0x455CE5, 2, {0xE8, 0x56}, {}, CapabilityExtendedDisplay, true},
    {0x4250E6, 5, {}, {}, CapabilityNativeSuper, true}
};
const size_t kSiteCount = sizeof(gSites) / sizeof(gSites[0]);
const size_t kNativeRouteSite = EngineSiteCount;
EngineLayout DefaultLayout() {
    EngineLayout layout;
    layout.profile = EngineProfileId::Ccz66;
    layout.waitWrapper = 0x42C4E5;
    layout.nativeSiteMask = (1u << SiteDialog3)|(1u << SiteExtraDelay)|(1u << SiteWaitBranch)|
        (1u << SiteDialogDisplay)|(1u << SiteDialog)|(1u << SiteMapText)|(1u << SiteExtendedDisplay);
    const uintptr_t sites[EngineSiteCount] = {0x42D5B0,0x4332A6,0x422DBA,0x429964,0x42C4E8,
        0x413324,0x42DB2C,0x415B40,0x42C4EF,0x42C812,0x455CE5};
    memcpy(layout.sites, sites, sizeof(sites));
    layout.nativeStage = 0x4750E0;
    layout.nativeSuper = 0x4250E6;
    layout.nativeSuperCall = 0x4132E3;
    layout.nativeSuperState = 0x500ADB;
    layout.nativeSuperBusy = 0x500E0C;
    layout.deadline = 0x40C19D;
    layout.sleep = 0x411195;
    layout.pump = 0x47EAC4;
    layout.phase = 0x4B3D18;
    layout.speed = 0x500AD8;
    layout.phaseTable = 0x4750DC;
    layout.phaseCounter = 0x4750DF;
    for (size_t i = 0; i < EngineSiteCount; ++i)
        memcpy(layout.siteOriginal[i], gSites[i].original, sizeof(layout.siteOriginal[i]));
    const BYTE enabled[EngineSiteCount][8] = {
        {0xC2,0x0C,0,0xE8},{0xC2,0x10,0,0x6A},{0xC2,0x04,0},
        {0x90,0x90,0x90,0x90,0x90},{},{0x10},{0xC2,0x04,0,0x6A},
        {0xEB,0x06},{0xEB,0x07},{0xEB,0x1D},{0xEB,0xF5}
    };
    memcpy(layout.siteEnabled, enabled, sizeof(enabled));
    layout.capabilities = CapabilitySuperDialog|CapabilitySuperMapText|CapabilitySuperDelay|
        CapabilitySuperR|CapabilitySuperS|CapabilityPhase|CapabilityInput|CapabilityDialog3|
        CapabilityExtraDelay|CapabilityDialogDisplay|CapabilityExtendedDisplay|CapabilityNativeSuper;
    return layout;
}
EngineLayout gLayout = DefaultLayout();
RuntimeJournalEntry gSiteJournal[kSiteCount] = {};
RuntimeJournalEntry gPhaseJournal = {};
RuntimeJournalEntry gNativeStateJournal = {};
RuntimeJournalEntry gNativeBusyJournal = {};
bool gInitialized = false;
size_t gRuntimeImageSize = 0;
wchar_t gError[256] = {};
SRWLOCK gPhaseLock = SRWLOCK_INIT;
struct PhaseState {
    bool enabled;
    bool flags[3];
    int values[3];
    int counters[3];
};
PhaseState gPhase = {};
BYTE gPhaseOriginal[4] = {};
BYTE gPhaseCurrent[3] = {};
bool gPhaseDataAvailable = false;
BOOL (WINAPI* gRealCounter)(LARGE_INTEGER*) = nullptr;
ULONGLONG (WINAPI* gRealTick64)() = nullptr;
LONGLONG gRealFrequency = 0;
void (WINAPI* gRealSleep)(DWORD) = nullptr;
bool gUseQpc = false;
void* gSuperThunk = nullptr;
void* gSuperTarget = nullptr;
void* gPhaseTarget = nullptr;
void* gNativeTarget = nullptr;
void (__cdecl* gNativeToggleHandler)() = nullptr;
bool gNativeDataAvailable = false;
bool gSuperR = false;
bool gSuperS = false;
bool gPhaseInstalled = false;
thread_local bool gSuperBusy = false;
thread_local bool gSuperScheduled = false;
thread_local ULONGLONG gSuperLast = 0;
#ifdef XA66_TEST
int gFailWrite = -1;
int gWriteCount = 0;
ULONGLONG (WINAPI* gTestSuperClock)() = nullptr;
void (WINAPI* gTestSuperSleep)(DWORD) = nullptr;
bool (__cdecl* gTestSuperPump)() = nullptr;
// Existing deterministic harnesses keep mapped images in std::vector storage.
// Production and the strict runtime harness require executable live pages.
bool gTestSyntheticDataImage = false;
bool gTestDirectPointers = false;
struct SyntheticImageCopy {
    BYTE* bytes = nullptr;
    ~SyntheticImageCopy() { if (bytes) VirtualFree(bytes, 0, MEM_RELEASE); }
    bool Capture(const BYTE* source, std::wstring& error) {
        IMAGE_DOS_HEADER dos = {};
        IMAGE_NT_HEADERS32 nt = {};
        const uintptr_t origin = reinterpret_cast<uintptr_t>(source);
        if (!ReadRuntimeMemory(source, &dos, sizeof(dos), error) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) ||
            dos.e_lfanew > 0x100000 || static_cast<uintptr_t>(dos.e_lfanew) > UINTPTR_MAX - origin ||
            !ReadRuntimeMemory(reinterpret_cast<const void*>(origin + dos.e_lfanew), &nt, sizeof(nt), error) ||
            nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage > kMaximumPeAnalysisImage) return false;
        bytes = static_cast<BYTE*>(VirtualAlloc(nullptr, nt.OptionalHeader.SizeOfImage,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        return bytes && ReadRuntimeMemory(source, bytes, nt.OptionalHeader.SizeOfImage, error);
    }
};
#endif

extern "C" int __cdecl SuperYieldBridge();
extern "C" int __cdecl LegacyPhaseBridge();
extern "C" void __cdecl NativeToggleBridge();

void SetError(const wchar_t* text) {
    wcsncpy_s(gError, text, _TRUNCATE);
}

void SetErrorIfEmpty(const wchar_t* text) {
    if (!gError[0]) SetError(text);
}

bool WriteMemory(uintptr_t address, const BYTE* bytes, size_t length);

BYTE* At(uintptr_t address) {
    return reinterpret_cast<BYTE*>(gGameBase + address - kPreferredBase);
}

bool ReadBytes(uintptr_t address, void* destination, size_t length) {
#ifdef XA66_TEST
    if (gTestDirectPointers) {
        std::wstring error;
        return ReadRuntimeMemory(reinterpret_cast<const void*>(address), destination, length, error);
    }
#endif
    if (address < kPreferredBase || !gGameBase) return false;
    const uintptr_t rva = address - kPreferredBase;
    if ((gRuntimeImageSize && (rva > gRuntimeImageSize || length > gRuntimeImageSize - rva)) ||
        rva > UINTPTR_MAX - gGameBase) return false;
    std::wstring error;
    return ReadRuntimeMemory(reinterpret_cast<const void*>(gGameBase + rva), destination, length, error);
}

bool LiveRange(uintptr_t address, size_t length, bool executable) {
    if (address < kPreferredBase || !gGameBase) return false;
    const uintptr_t rva = address - kPreferredBase;
    if ((gRuntimeImageSize && (rva > gRuntimeImageSize || length > gRuntimeImageSize - rva)) ||
        rva > UINTPTR_MAX - gGameBase) return false;
#ifdef XA66_TEST
    executable = executable && !gTestSyntheticDataImage;
#endif
    std::wstring error;
    return ValidateRuntimeMemory(reinterpret_cast<const void*>(gGameBase + rva), length, executable, error);
}

bool MatchBytes(uintptr_t address, const BYTE* expected, size_t length) {
    BYTE actual[16] = {};
    return length <= sizeof(actual) && ReadBytes(address, actual, length) &&
        memcmp(actual, expected, length) == 0;
}

void StartJournal(RuntimeJournalEntry& entry, uintptr_t address, size_t length,
    const BYTE* original) {
    entry = RuntimeJournalEntry();
    if (!address || !length || length > sizeof(entry.original) || !original) return;
    entry.active = true;
    entry.expectedValid = true;
    entry.address = address;
    entry.length = length;
    memcpy(entry.original, original, length);
    memcpy(entry.expected, original, length);
}

void JournalExpected(RuntimeJournalEntry& entry, const BYTE* expected) {
    if (!entry.active || !expected) return;
    memcpy(entry.expected, expected, entry.length);
    entry.expectedValid = true;
}

bool RestoreMemoryJournal(RuntimeJournalEntry& entry, const wchar_t* label,
    size_t ownedLength = SIZE_MAX) {
    if (!entry.active || !entry.expectedValid || !entry.address) return true;
    BYTE actual[8] = {};
    if (!ReadBytes(entry.address, actual, entry.length)) {
        SetError(label);
        return false;
    }
    if (ownedLength == SIZE_MAX) ownedLength = entry.length;
    if (ownedLength > entry.length ||
        (memcmp(actual, entry.expected, ownedLength) != 0 &&
         memcmp(actual, entry.original, ownedLength) != 0)) {
        SetError(label);
        return false;
    }
    if (memcmp(actual, entry.original, entry.length) == 0) {
        JournalExpected(entry, entry.original);
        return true;
    }
    if (!WriteMemory(entry.address, entry.original, entry.length)) {
        SetError(label);
        return false;
    }
    JournalExpected(entry, entry.original);
    return true;
}

void BytesToText(const BYTE* bytes, size_t length, wchar_t* text, size_t capacity) {
    if (!text || !capacity) return;
    text[0] = 0;
    size_t used = 0;
    for (size_t i = 0; i < length && used + 3 < capacity; ++i) {
        const int written = swprintf_s(text + used, capacity - used,
            i ? L" %02X" : L"%02X", static_cast<unsigned>(bytes[i]));
        if (written <= 0) break;
        used += static_cast<size_t>(written);
    }
}

void SetSiteMismatch(const wchar_t* prefix, const Site& site, const BYTE* expected) {
    BYTE actual[8] = {};
    wchar_t expectedText[32] = {};
    wchar_t actualText[32] = {};
    BytesToText(expected, site.length, expectedText, sizeof(expectedText) / sizeof(expectedText[0]));
    if (ReadBytes(site.address, actual, site.length))
        BytesToText(actual, site.length, actualText, sizeof(actualText) / sizeof(actualText[0]));
    else
        wcscpy_s(actualText, L"<unreadable>");
    swprintf_s(gError, L"%ls（地址 0x%08X，期望 [%ls]，实际 [%ls]）",
        prefix, static_cast<unsigned>(site.address), expectedText, actualText);
}

bool WriteProtectedBytes(uintptr_t address, const BYTE* bytes, size_t length, bool executable) {
    if (!bytes || !length || length > 8 || !LiveRange(address, length, executable)) return false;
    BYTE* location = At(address);
    struct Segment { BYTE* address; size_t length; DWORD protection; } segments[2] = {};
    size_t count = 0, offset = 0;
    bool prepared = true;
    while (offset < length && count < ARRAYSIZE(segments)) {
        MEMORY_BASIC_INFORMATION region = {};
        const uintptr_t at = reinterpret_cast<uintptr_t>(location + offset);
        if (!VirtualQuery(location + offset, &region, sizeof(region))) { prepared = false; break; }
        const uintptr_t origin = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - origin || origin + region.RegionSize <= at) {
            prepared = false;
            break;
        }
        const size_t available = static_cast<size_t>(origin + region.RegionSize - at);
        const size_t part = length - offset < available ? length - offset : available;
        Segment& segment = segments[count];
        segment.address = location + offset;
        segment.length = part;
        const DWORD executeBits = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        const DWORD protection = executable || (region.Protect & executeBits) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        if (!VirtualProtect(segment.address, part, protection, &segment.protection)) { prepared = false; break; }
        ++count;
        offset += part;
    }
    prepared = prepared && offset == length;
    bool written = false;
    if (prepared) {
        __try {
            memcpy(location, bytes, length);
            written = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            written = false;
        }
    }
    const bool flushed = !written || FlushInstructionCache(GetCurrentProcess(), location, length) != 0;
    bool restored = true;
    while (count) {
        const Segment& segment = segments[--count];
        DWORD ignored = 0;
        restored = VirtualProtect(segment.address, segment.length, segment.protection, &ignored) != 0 && restored;
    }
    return prepared && written && flushed && restored;
}

bool WriteSite(const Site& site, const BYTE* bytes, bool rollback = false) {
#ifdef XA66_TEST
    if (!rollback && gFailWrite >= 0 && gWriteCount++ == gFailWrite) return false;
#else
    (void)rollback;
#endif
    return WriteProtectedBytes(site.address, bytes, site.length, true);
}

bool WriteMemory(uintptr_t address, const BYTE* bytes, size_t length) {
    return WriteProtectedBytes(address, bytes, length, false);
}

bool RestoreSiteJournals() {
    // Check every site before writing any of them.  A third-party patch that
    // changed one site therefore cannot leave earlier sites half-restored.
    BYTE previousActual[kSiteCount][8] = {};
    for (size_t i = 0; i < kSiteCount; ++i) {
        RuntimeJournalEntry& entry = gSiteJournal[i];
        if (!entry.active || !entry.expectedValid || !entry.address) continue;
        if (!ReadBytes(entry.address, previousActual[i], entry.length) ||
            (memcmp(previousActual[i], entry.expected, entry.length) != 0 &&
             memcmp(previousActual[i], entry.original, entry.length) != 0)) {
            SetError(L"运行时恢复冲突：目标指令已被其它补丁修改，未覆盖现有字节。");
            return false;
        }
    }
    size_t restored[ARRAYSIZE(gSiteJournal)] = {};
    size_t count = 0;
    for (size_t i = 0; i < kSiteCount; ++i) {
        RuntimeJournalEntry& entry = gSiteJournal[i];
        if (!entry.active || !entry.expectedValid ||
            memcmp(previousActual[i], entry.original, entry.length) == 0) continue;
        if (!WriteSite(gSites[i], entry.original)) {
            // A failed write can have copied bytes before protection/cache
            // restoration failed. Include that site in the rollback as well.
            bool rolledBack = WriteSite(gSites[i], previousActual[i], true);
            for (size_t j = 0; j < count; ++j) {
                const bool okay = WriteSite(gSites[restored[j]], previousActual[restored[j]], true);
                rolledBack = okay && rolledBack;
            }
            SetError(rolledBack ? L"运行时恢复写入失败，未完成恢复。" :
                L"运行时恢复和回滚均失败，请重启游戏后重试。");
            return false;
        }
        restored[count++] = i;
    }
    // Commit journal state only after the entire instruction transaction has
    // succeeded. A retry after rollback must still expect the installed bytes.
    for (size_t i = 0; i < kSiteCount; ++i) {
        RuntimeJournalEntry& entry = gSiteJournal[i];
        if (!entry.active || !entry.expectedValid) continue;
        JournalExpected(entry, entry.original);
        memcpy(gSites[i].current, entry.original, entry.length);
    }
    return true;
}

bool Rel32Reachable(uintptr_t instruction, uintptr_t target) {
    const int64_t displacement = static_cast<int64_t>(target) -
        static_cast<int64_t>(instruction + 5);
    return displacement >= INT32_MIN && displacement <= INT32_MAX;
}

void* AllocateNearThunk(uintptr_t instruction, uintptr_t target) {
    if (Rel32Reachable(instruction, target)) return reinterpret_cast<void*>(target);
    const uintptr_t granularity = 0x10000;
    const uintptr_t center = instruction & ~(granularity - 1);
    for (uintptr_t distance = 0; distance <= 0x7F000000; distance += granularity) {
        const uintptr_t hints[] = {
            center + distance,
            center >= distance ? center - distance : 0
        };
        for (uintptr_t hint : hints) {
            if (!hint) continue;
            BYTE* thunk = static_cast<BYTE*>(VirtualAlloc(reinterpret_cast<void*>(hint), 16,
                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (!thunk || !Rel32Reachable(instruction, reinterpret_cast<uintptr_t>(thunk))) {
                if (thunk) VirtualFree(thunk, 0, MEM_RELEASE);
                continue;
            }
            thunk[0] = 0xB8;
            const uint32_t absolute = static_cast<uint32_t>(target);
            memcpy(thunk + 1, &absolute, sizeof(absolute));
            thunk[5] = 0xFF;
            thunk[6] = 0xE0;
            FlushInstructionCache(GetCurrentProcess(), thunk, 7);
            return thunk;
        }
    }
    return nullptr;
}

bool SuperCallBytes(BYTE* bytes, uintptr_t instruction) {
    const uintptr_t bridge = reinterpret_cast<uintptr_t>(&SuperYieldBridge);
    if (!gSuperTarget) {
        gSuperTarget = AllocateNearThunk(instruction, bridge);
        if (!gSuperTarget) return false;
    }
    if (!Rel32Reachable(instruction, reinterpret_cast<uintptr_t>(gSuperTarget))) return false;
    bytes[0] = 0xE8;
    const int32_t displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(gSuperTarget) -
        (instruction + 5));
    memcpy(bytes + 1, &displacement, sizeof(displacement));
    return true;
}

bool PhaseCallBytes(BYTE* bytes, uintptr_t instruction) {
    const uintptr_t bridge = reinterpret_cast<uintptr_t>(&LegacyPhaseBridge);
    if (!gPhaseTarget) {
        gPhaseTarget = AllocateNearThunk(instruction, bridge);
        if (!gPhaseTarget) return false;
    }
    if (!Rel32Reachable(instruction, reinterpret_cast<uintptr_t>(gPhaseTarget))) return false;
    bytes[0] = 0xE8;
    const int32_t displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(gPhaseTarget) -
        (instruction + 5));
    memcpy(bytes + 1, &displacement, sizeof(displacement));
    return true;
}

bool NativeJumpBytes(BYTE* bytes, uintptr_t instruction) {
    const uintptr_t bridge = reinterpret_cast<uintptr_t>(&NativeToggleBridge);
    if (!gNativeTarget) {
        gNativeTarget = AllocateNearThunk(instruction, bridge);
        if (!gNativeTarget) return false;
    }
    if (!Rel32Reachable(instruction, reinterpret_cast<uintptr_t>(gNativeTarget))) return false;
    bytes[0] = 0xE9;
    const int32_t displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(gNativeTarget) -
        (instruction + 5));
    memcpy(bytes + 1, &displacement, sizeof(displacement));
    return true;
}

extern "C" void __cdecl NativeToggleBridge() {
    if (gNativeToggleHandler) gNativeToggleHandler();
}

ULONGLONG SuperClock() {
#ifdef XA66_TEST
    if (gTestSuperClock) return gTestSuperClock();
#endif
    if (gUseQpc && gRealCounter && gRealFrequency > 0) {
        LARGE_INTEGER value = {};
        if (gRealCounter(&value)) {
            const ULONGLONG seconds = static_cast<ULONGLONG>(value.QuadPart / gRealFrequency);
            const LONGLONG remainder = value.QuadPart % gRealFrequency;
            // Keep the conversion integer-only.  This function runs from the
            // presentation wait bridge; converting the remainder through a
            // double needlessly enters the floating-point pipeline and can
            // round the four-millisecond throttle boundary differently on
            // different CPUs.
            return seconds * 1000000ULL +
                static_cast<ULONGLONG>(remainder) * 1000000ULL /
                static_cast<ULONGLONG>(gRealFrequency);
        }
    }
    return gRealTick64 ? gRealTick64() * 1000ULL : static_cast<ULONGLONG>(GetTickCount()) * 1000ULL;
}

void SuperSleep(DWORD delay) {
#ifdef XA66_TEST
    if (gTestSuperSleep) { gTestSuperSleep(delay); return; }
#endif
    gRealSleep(delay);
}

bool SuperPump() {
#ifdef XA66_TEST
    if (gTestSuperPump) return gTestSuperPump();
#endif
    MSG message = {};
    // Leave WM_QUIT queued for the application's outer message loop.
    if (!PeekMessageA(&message, nullptr, 0, 0, PM_NOREMOVE) || message.message == WM_QUIT) return false;
    reinterpret_cast<void (__cdecl*)()>(At(gLayout.pump))();
    return true;
}

int KnownWaitCallerKind(uintptr_t caller) {
    for (size_t i = 0; i < gLayout.waitCallerCount; ++i)
        if (gLayout.waitCallers[i] == caller) return gLayout.waitKinds[i];
    return -1;
}

int CallTimerFunction(uintptr_t address, uintptr_t context) {
    if (gLayout.stageUsesThis)
        return reinterpret_cast<int (__thiscall*)(void*)>(At(address))(reinterpret_cast<void*>(context));
    return reinterpret_cast<int (__cdecl*)()>(At(address))();
}

int NativeStageCall(uintptr_t context) {
    return CallTimerFunction(gLayout.nativeStage, context);
}

static int LegacyStageDelay(int value, int& counter) {
    if (value > 50) return value - 50;
    ++counter;
    const int delay = counter < value ? 1 : 0;
    if (counter >= 50) counter -= 50;
    return delay;
}

int __cdecl LegacyPhaseHelper(uintptr_t context) {
    if (!gPhaseInstalled || !gPhaseDataAvailable || !gLayout.speed || !gLayout.phase ||
        (!gLayout.phaseOwnedByPatch && (!gLayout.phaseTable || !gLayout.phaseCounter)) ||
        !gLayout.sleep) return NativeStageCall(context);

    BYTE phase = 0;
    BYTE value = 0;
    BYTE counter = 0;
    if (!ReadBytes(gLayout.phase, &phase, sizeof(phase)) || phase >= 3)
        return NativeStageCall(context);

    if (gLayout.phaseOwnedByPatch) {
        AcquireSRWLockExclusive(&gPhaseLock);
        if (!gPhase.enabled) {
            ReleaseSRWLockExclusive(&gPhaseLock);
            return NativeStageCall(context);
        }
        const int selected = gPhase.flags[phase] ? gPhase.values[phase] : 50;
        const int delay = LegacyStageDelay(selected, gPhase.counters[phase]);
        ReleaseSRWLockExclusive(&gPhaseLock);
        reinterpret_cast<void (__stdcall*)(DWORD)>(At(gLayout.sleep))(static_cast<DWORD>(delay));
        // Proxy-owned 6.4 pacing replaces the native deadline path. Calling
        // it here would mix two timers and duplicate a frame wait.
        return 1;
    }
    WORD speed = 0;
    if (!ReadBytes(gLayout.phaseTable + phase, &value, sizeof(value)) ||
        !ReadBytes(gLayout.speed, &speed, sizeof(speed)) ||
        !ReadBytes(gLayout.phaseCounter, &counter, sizeof(counter))) return NativeStageCall(context);

    int delay = 0;
    if (value > 50) {
        delay = static_cast<int>(value) - 50;
    } else {
        ++counter;
        delay = counter < value ? 1 : 0;
        if (counter >= 50) counter = static_cast<BYTE>(counter - 50);
        bool wroteCounter = false;
        __try {
            *At(gLayout.phaseCounter) = counter;
            wroteCounter = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            wroteCounter = false;
        }
        if (!wroteCounter) return NativeStageCall(context);
    }
    reinterpret_cast<void (__stdcall*)(DWORD)>(At(gLayout.sleep))(static_cast<DWORD>(delay));
    if ((speed & 0xFF) == 0 && gLayout.deadline)
        CallTimerFunction(gLayout.deadline, context);
    return 1;
}

__declspec(naked) int __cdecl LegacyPhaseBridge() {
    __asm {
        push ecx
        call LegacyPhaseHelper
        add esp, 4
        ret
    }
}

int __cdecl SuperYieldWithContext(uintptr_t caller, uintptr_t context) {
    // The wrapper repeats this call while it returns zero. Merely returning
    // zero for an unknown wait would remove the native completion test and
    // keep the wrapper pumping forever. Preserve that test and its timer ABI.
    const int kind = KnownWaitCallerKind(caller);
    if (!gSuperS || kind < 0) return NativeStageCall(context);
#ifndef XA66_TEST
    if (kind >= static_cast<int>(AnimationKind::Count) || !gFeatureSettings.animationKinds[kind])
        return NativeStageCall(context);
    BYTE camp = 0xFF;
    if (!gLayout.phase || !ReadBytes(gLayout.phase, &camp, sizeof(camp))) return NativeStageCall(context);
    if (camp >= 3 || !gFeatureSettings.animationCamps[camp]) return NativeStageCall(context);
#endif
    if (gSuperBusy) return 1;
    // One clock sample is enough for the throttle.  The previous code took a
    // second QPC sample after pumping; that doubled the cost of this hot
    // presentation path and could make a long pump appear to have no
    // throttle interval at all.
    const ULONGLONG now = SuperClock();
    if (gSuperScheduled && now >= gSuperLast && now - gSuperLast < 4000) return 1;
    gSuperBusy = true;
    // Keep the native wrapper's completion check and give the window one small
    // scheduling opportunity. This skips intermediate attack frames without
    // starving resource, BUFF and repaint callbacks.
    SuperSleep(0);
    SuperPump();
    gSuperLast = now;
    gSuperScheduled = true;
    gSuperBusy = false;
    return 1;
}

__declspec(naked) int __cdecl SuperYieldBridge() {
    __asm {
        mov eax, [esp + 8]
        push ecx
        push eax
        call SuperYieldWithContext
        add esp, 8
        ret
    }
}

uintptr_t CallTarget(uintptr_t instruction) {
    BYTE bytes[5] = {};
    int32_t displacement = 0;
    if (!ReadBytes(instruction, bytes, sizeof(bytes)) || bytes[0] != 0xE8) return 0;
    memcpy(&displacement, bytes + 1, sizeof(displacement));
    return static_cast<uint32_t>(instruction + 5 + displacement);
}

bool NormalizeNativeToggleLayout() {
    size_t indices[EngineSiteCount] = {};
    size_t count = 0;
    for (size_t i = 0; i < EngineSiteCount; ++i)
        if (gLayout.nativeSiteMask & (1u << i)) indices[count++] = i;
    bool allOriginal = true;
    bool allEnabled = true;
    for (size_t i = 0; i < count; ++i) {
        const Site& site = gSites[indices[i]];
        allOriginal = allOriginal && MatchBytes(site.address, site.original, site.length);
        allEnabled = allEnabled && MatchBytes(site.address, gLayout.siteEnabled[indices[i]], site.length);
    }
    if (!allOriginal && !allEnabled) {
        SetError(L"原生超级加速站点处于未知或不完整状态；已停用相关子项，阶段、齿轮和其它已验证功能仍可使用。");
        for (size_t i = 0; i < count; ++i) gSites[indices[i]].available = false;
        gSites[kNativeRouteSite].available = false;
        return true;
    }
    if (allEnabled) {
        bool changed[ARRAYSIZE(indices)] = {};
        for (size_t i = 0; i < count; ++i) {
            Site& site = gSites[indices[i]];
            if (!WriteSite(site, site.original)) {
                for (size_t j = 0; j < i; ++j)
                    if (changed[j]) WriteSite(gSites[indices[j]], gLayout.siteEnabled[indices[j]], true);
                SetSiteMismatch(L"无法把 原生超级加速恢复为关闭状态", site, gLayout.siteEnabled[indices[i]]);
                return false;
            }
            changed[i] = true;
        }
    }
    if (gLayout.nativeSuperState && gLayout.nativeSuperBusy) {
        const BYTE zero = 0;
        if (!WriteMemory(gLayout.nativeSuperState, &zero, 1) ||
            !WriteMemory(gLayout.nativeSuperBusy, &zero, 1)) {
            SetError(L"无法初始化 原生超级加速状态字节，已停止接管 Z 键。");
            gSites[kNativeRouteSite].available = false;
            return true;
        }
        gNativeDataAvailable = true;
    }
    return true;
}

bool InitializeSites(bool resolveLayout, uintptr_t virtualBase = 0,
    bool allowVersionFallback = false, EngineProfileId fallbackProfile = EngineProfileId::Unknown) {
    gInitialized = false;
    gRuntimeImageSize = 0;
    gPhaseDataAvailable = false;
    gNativeDataAvailable = false;
    for (size_t i = 0; i < kSiteCount; ++i) gSiteJournal[i] = RuntimeJournalEntry();
    gPhaseJournal = RuntimeJournalEntry();
    gNativeStateJournal = RuntimeJournalEntry();
    gNativeBusyJournal = RuntimeJournalEntry();
    gError[0] = 0;
    if (!gGameBase) {
        SetError(L"\u65e0\u6cd5\u5b9a\u4f4d\u6e38\u620f\u4e3b\u7a0b\u5e8f\u3002");
        return false;
    }
    if (resolveLayout) {
        EngineLayout layout;
        RuntimeImageView snapshot;
        std::wstring readError;
        const BYTE* captureBase = reinterpret_cast<const BYTE*>(gGameBase);
#ifdef XA66_TEST
        // The legacy deterministic fixtures use std::vector rather than an
        // independent module allocation. Copy those bounded test images into
        // a dedicated allocation before using the production snapshot reader;
        // the live source and byte journal still refer to the fixture itself.
        SyntheticImageCopy syntheticImage;
        if (gTestSyntheticDataImage) {
            if (!syntheticImage.Capture(captureBase, readError)) {
                SetError(L"无法捕获测试映像，未修改引擎。" );
                return false;
            }
            captureBase = syntheticImage.bytes;
        }
#endif
        if (!snapshot.Capture(captureBase, 0, readError)) {
            SetError(L"无法安全捕获游戏运行时映像，未修改引擎。" );
            return false;
        }
        const BYTE* image = snapshot.Data();
        gRuntimeImageSize = snapshot.Size();
        const uintptr_t operandBase = virtualBase ? virtualBase : gGameBase;
        EngineAnalysisSource analysisSource = EngineAnalysisSource::Runtime;
#ifdef XA66_TEST
        if (gTestSyntheticDataImage) analysisSource = EngineAnalysisSource::Disk;
#endif
        bool resolved = ResolveEngineLayout(image, layout, operandBase, analysisSource, gGameBase) &&
            layout.plan.status == EnginePlanStatus::AutoVerified;
        if (!resolved && allowVersionFallback) {
            resolved = ResolveVersionEngineLayout(image, snapshot.Size(),
                fallbackProfile == EngineProfileId::Unknown ? CompiledEngineProfile() : fallbackProfile,
                layout, operandBase);
        }
        if (!resolved) {
            SetError(L"未能定位并验证此基底的加速函数。" );
            return false;
        }
        const auto validateSite = [&](const EngineSitePlan& site, bool executable) {
            if (!site.present) return !site.required;
            return LiveRange(site.legacyAddress, site.length, executable) &&
                MatchBytes(site.legacyAddress, site.original, site.length);
        };
        bool liveVerified = true;
        for (const auto& site : layout.plan.sites) liveVerified = validateSite(site, true) && liveVerified;
        const EngineSitePlan* dataSites[] = {&layout.plan.nativeState, &layout.plan.nativeBusy,
            &layout.plan.phaseTable, &layout.plan.phaseValue, &layout.plan.speedValue, &layout.plan.timerContext};
        for (const auto* site : dataSites) liveVerified = validateSite(*site, false) && liveVerified;
        liveVerified = validateSite(layout.plan.nativeRoute, true) && liveVerified;
        const uintptr_t dependencies[] = {layout.waitWrapper, layout.nativeStage, layout.deadline, layout.sleep, layout.pump};
        for (const uintptr_t address : dependencies)
            if (address && !LiveRange(address, 1, true)) liveVerified = false;
        if (!liveVerified) {
            SetError(L"运行时站点页面不可读/不可执行或原始字节已变化，未提交加速钩子。" );
            return false;
        }
        layout.plan.loadedBase = gGameBase;
        layout.plan.runtimeValidated = true;
#ifndef XA66_TEST
        if (layout.profile != CompiledEngineProfile()) {
            SetError(L"游戏引擎与补丁版本不匹配，请重新运行加速补丁安装器。");
            return false;
        }
#endif
        gLayout = layout;
        for (size_t i = 0; i < EngineSiteCount; ++i) {
            gSites[i].address = layout.sites[i];
            memcpy(gSites[i].original, layout.siteOriginal[i], sizeof(gSites[i].original));
            gSites[i].available = layout.sites[i] != 0 && (layout.capabilities & gSites[i].capability) != 0;
        }
        gSites[kNativeRouteSite].address = layout.nativeSuper;
        gSites[kNativeRouteSite].available = layout.nativeSuper != 0 &&
            (layout.capabilities & CapabilityNativeSuper) != 0;
    }
    const bool versionSelected = gLayout.plan.status == EnginePlanStatus::VersionSelected;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    gRealCounter = reinterpret_cast<decltype(gRealCounter)>(GetProcAddress(kernel, "QueryPerformanceCounter"));
    const auto frequency = reinterpret_cast<BOOL (WINAPI*)(LARGE_INTEGER*)>(
        GetProcAddress(kernel, "QueryPerformanceFrequency"));
    LARGE_INTEGER ticks = {};
    if (frequency && frequency(&ticks)) gRealFrequency = ticks.QuadPart;
    gRealSleep = reinterpret_cast<decltype(gRealSleep)>(GetProcAddress(kernel, "Sleep"));
    gRealTick64 = reinterpret_cast<decltype(gRealTick64)>(GetProcAddress(kernel, "GetTickCount64"));
    gUseQpc = gRealCounter && gRealFrequency > 0;
    if (!gRealSleep) { SetError(L"\u65e0\u6cd5\u83b7\u53d6\u7cfb\u7edf Sleep \u51fd\u6570\u3002"); return false; }
    const BYTE shiftContext[] = {0x83, 0xF8, 0x10, 0x75, 0x0F};
    const BYTE branchTail[] = {0x07};
    if (!resolveLayout && (!MatchBytes(gSites[SiteInputModifier].address - 2, shiftContext, sizeof(shiftContext)) ||
        !MatchBytes(gSites[SiteWaitBranch].address + 1, branchTail, sizeof(branchTail)))) {
        SetError(L"\u8f93\u5165\u6216\u7b49\u5f85\u6307\u4ee4\u5df2\u88ab\u5176\u4ed6\u8865\u4e01\u6539\u53d8\uff0c\u5df2\u505c\u6b62\u52a0\u901f\u4fee\u6539\u3002");
        return false;
    }
    if (gSites[SiteWaitCall].available) {
        if (!versionSelected && CallTarget(gSites[SiteWaitCall].address) != gLayout.nativeStage) {
            SetSiteMismatch(L"公共等待调用目标无效，已停用阶段和超级 S", gSites[SiteWaitCall],
                gSites[SiteWaitCall].original);
            gSites[SiteWaitCall].available = false;
        } else ReadBytes(gSites[SiteWaitCall].address, gSites[SiteWaitCall].original,
            gSites[SiteWaitCall].length);
    }
    if (gSites[SiteRWait].available) {
        if (!versionSelected && CallTarget(gSites[SiteRWait].address) != gLayout.waitWrapper) {
            gSites[SiteRWait].available = false;
        } else ReadBytes(gSites[SiteRWait].address, gSites[SiteRWait].original,
            gSites[SiteRWait].length);
    }
    if (gSites[kNativeRouteSite].available &&
        !ReadBytes(gSites[kNativeRouteSite].address, gSites[kNativeRouteSite].original,
            gSites[kNativeRouteSite].length))
        gSites[kNativeRouteSite].available = false;
    if (versionSelected) {
        // A confirmed version template captures the current bytes verbatim.
        // Do not normalize native toggle bytes or zero the live state before
        // the restoration journal has a chance to preserve them.
        BYTE state = 0, busy = 0;
        gNativeDataAvailable = gLayout.nativeSuperState && gLayout.nativeSuperBusy &&
            ReadBytes(gLayout.nativeSuperState, &state, 1) &&
            ReadBytes(gLayout.nativeSuperBusy, &busy, 1);
    } else if (!NormalizeNativeToggleLayout()) return false;
    for (size_t i = 0; i < kSiteCount; ++i) {
        if (!gSites[i].available) continue;
        if (i == SiteWaitCall || i == SiteRWait || i == kNativeRouteSite) {
            memcpy(gSites[i].current, gSites[i].original, gSites[i].length);
            continue;
        }
        if (resolveLayout && !versionSelected && i == SiteInputModifier) {
            BYTE modifier = 0;
            if (!ReadBytes(gSites[i].address, &modifier, 1) ||
                (modifier != VK_SHIFT && modifier != VK_CONTROL)) {
                gSites[i].available = false;
                continue;
            }
            gSites[i].original[0] = modifier;
        }
        if (!versionSelected && !MatchBytes(gSites[i].address, gSites[i].original, gSites[i].length)) {
            SetSiteMismatch(L"\u6307\u4ee4\u6821\u9a8c\u5931\u8d25\uff0c\u5df2\u505c\u7528\u8be5\u529f\u80fd", gSites[i], gSites[i].original);
            gSites[i].available = false;
            continue;
        }
        if (!ReadBytes(gSites[i].address, gSites[i].original, gSites[i].length)) {
            swprintf_s(gError, L"\u65e0\u6cd5\u8bfb\u53d6\u52a0\u901f\u51fd\u6570\u539f\u59cb\u6307\u4ee4\uff080x%08X\uff09\u3002",
                static_cast<unsigned>(gSites[i].address));
            return false;
        }
        memcpy(gSites[i].current, gSites[i].original, gSites[i].length);
    }
    for (size_t i = 0; i < kSiteCount; ++i) {
        if (gSites[i].available)
            StartJournal(gSiteJournal[i], gSites[i].address, gSites[i].length, gSites[i].original);
    }
    gPhaseDataAvailable = false;
    if ((gLayout.capabilities & CapabilityPhase) && gLayout.phaseOwnedByPatch &&
        gLayout.phase && gLayout.speed && gLayout.sleep && gLayout.nativeStage) {
        // 6.4 has no native phase table. Its parameters and counters belong
        // to the proxy, so only the wait CALL participates in the byte journal.
        gPhaseDataAvailable = true;
    } else if ((gLayout.capabilities & CapabilityPhase) && gLayout.phaseTable && gLayout.phaseCounter &&
        ReadBytes(gLayout.phaseTable, gPhaseOriginal, 4)) {
        memcpy(gPhaseCurrent, gPhaseOriginal, 3);
        gPhaseDataAvailable = true;
        StartJournal(gPhaseJournal, gLayout.phaseTable, sizeof(gPhaseOriginal), gPhaseOriginal);
    }
    if (gNativeDataAvailable) {
        BYTE state = 0, busy = 0;
        if (ReadBytes(gLayout.nativeSuperState, &state, 1) &&
            ReadBytes(gLayout.nativeSuperBusy, &busy, 1)) {
            StartJournal(gNativeStateJournal, gLayout.nativeSuperState, 1, &state);
            StartJournal(gNativeBusyJournal, gLayout.nativeSuperBusy, 1, &busy);
        } else {
            gNativeDataAvailable = false;
        }
    }
    AcquireSRWLockExclusive(&gPhaseLock);
    gPhase = {};
    ReleaseSRWLockExclusive(&gPhaseLock);
    gSuperBusy = gSuperScheduled = false;
    gSuperLast = 0;
    gPhaseInstalled = false;
    gInitialized = true;
    return true;
}

bool ApplyBytes(const BYTE desired[kSiteCount][8]) {
    for (size_t i = 0; i < kSiteCount; ++i) {
        if (!gSites[i].available) continue;
        if (!MatchBytes(gSites[i].address, gSites[i].current, gSites[i].length)) {
            SetSiteMismatch(L"\u52a0\u901f\u6307\u4ee4\u53d1\u751f\u51b2\u7a81\uff0c\u672c\u6b21\u8bbe\u7f6e\u672a\u751f\u6548", gSites[i], gSites[i].current);
            return false;
        }
    }
    bool changed[kSiteCount] = {};
    for (size_t i = 0; i < kSiteCount; ++i) {
        if (!gSites[i].available) continue;
        if (memcmp(desired[i], gSites[i].current, gSites[i].length) == 0) continue;
        changed[i] = true;
        if (!WriteSite(gSites[i], desired[i])) {
            bool rolledBack = true;
            for (size_t j = 0; j <= i; ++j) {
                if (changed[j]) {
                    const bool okay = WriteSite(gSites[j], gSites[j].current, true);
                    if (okay) JournalExpected(gSiteJournal[j], gSites[j].current);
                    rolledBack = okay && rolledBack;
                }
            }
            SetError(rolledBack ? L"\u65e0\u6cd5\u5199\u5165\u52a0\u901f\u6307\u4ee4\uff0c\u672c\u6b21\u4fee\u6539\u5df2\u56de\u9000\u3002" :
                L"\u52a0\u901f\u6307\u4ee4\u5199\u5165\u548c\u56de\u9000\u5931\u8d25\uff0c\u8bf7\u91cd\u65b0\u542f\u52a8\u6e38\u620f\u3002");
            return false;
        }
        JournalExpected(gSiteJournal[i], desired[i]);
    }
    for (size_t i = 0; i < kSiteCount; ++i) memcpy(gSites[i].current, desired[i], gSites[i].length);
    return true;
}
}

int StageDelay(int value, int& counter) {
    if (value > 50) return value - 50;
    ++counter;
    const int delay = counter < value ? 1 : 0;
    if (counter >= 50) counter -= 50;
    return delay;
}

bool InitializeEngine(bool allowVersionFallback) {
    if (gInitialized) return true;
    return InitializeSites(true, 0, allowVersionFallback);
}

int CurrentGamePhase() {
    BYTE phase = 0;
    return gInitialized && gLayout.phase && ReadBytes(gLayout.phase, &phase, sizeof(phase)) && phase < 3 ? phase : -1;
}

bool ApplyEngineState(const Settings& settings) {
    if (!gInitialized) return false;
    gError[0] = 0;
    if (settings.superEnabled && settings.phaseEnabled) {
        SetError(L"\u8d85\u7ea7\u52a0\u901f\u4e0e\u9636\u6bb5\u52a0\u901f\u4e0d\u80fd\u540c\u65f6\u5f00\u542f\u3002");
        return false;
    }
    for (int phase = 0; phase < 3; ++phase) {
        if (settings.phase[phase] < 0 || settings.phase[phase] > 200) {
            SetError(L"\u9636\u6bb5\u52a0\u901f\u53c2\u6570\u5fc5\u987b\u5728 0 \u81f3 200 \u4e4b\u95f4\u3002");
            return false;
        }
    }
    BYTE desired[kSiteCount][8] = {};
    for (size_t i = 0; i < kSiteCount; ++i) memcpy(desired[i], gSites[i].original, gSites[i].length);
    bool nativeRouteInstalled = false;
    if (gSites[kNativeRouteSite].available && NativeJumpBytes(desired[kNativeRouteSite],
        reinterpret_cast<uintptr_t>(At(gSites[kNativeRouteSite].address))))
        nativeRouteInstalled = true;
    bool superSInstalled = false;
    bool phaseUnavailable = settings.phaseEnabled && !gPhaseDataAvailable;
    const bool dialogReady = gSites[SiteDialog].available &&
        (!gLayout.dialog3Applicable || gSites[SiteDialog3].available) &&
        gSites[SiteDialogDisplay].available;
    const bool mapReady = gSites[SiteMapText].available && gSites[SiteDialogDisplay].available;
    const bool delayReady = gSites[SiteDelay].available && gSites[SiteExtraDelay].available;
    const bool rReady = gSites[SiteRWait].available;
    const bool sReady = gSites[SiteWaitCall].available && gSites[SiteExtendedDisplay].available &&
        (gLayout.capabilities & CapabilitySuperS) != 0;
    if (settings.superEnabled) {
        if (settings.superFlags[0] && dialogReady) {
            memcpy(desired[SiteDialog], gLayout.siteEnabled[SiteDialog], gSites[SiteDialog].length);
            if (gLayout.dialog3Applicable)
                memcpy(desired[SiteDialog3], gLayout.siteEnabled[SiteDialog3], gSites[SiteDialog3].length);
        }
        if (settings.superFlags[1] && mapReady) {
            memcpy(desired[SiteMapText], gLayout.siteEnabled[SiteMapText], gSites[SiteMapText].length);
        }
        if ((settings.superFlags[0] && dialogReady) || (settings.superFlags[1] && mapReady)) {
            memcpy(desired[SiteDialogDisplay], gLayout.siteEnabled[SiteDialogDisplay],
                gSites[SiteDialogDisplay].length);
        }
        if (settings.superFlags[2] && delayReady) {
            memcpy(desired[SiteDelay], gLayout.siteEnabled[SiteDelay], gSites[SiteDelay].length);
            memcpy(desired[SiteExtraDelay], gLayout.siteEnabled[SiteExtraDelay], gSites[SiteExtraDelay].length);
        }
        if (settings.superFlags[3] && rReady)
            memset(desired[SiteRWait], 0x90, gSites[SiteRWait].length);
        if (settings.superFlags[4] && sReady) {
            if (
                SuperCallBytes(desired[SiteWaitCall],
                reinterpret_cast<uintptr_t>(At(gSites[SiteWaitCall].address)))) {
                superSInstalled = true;
                desired[SiteExtendedDisplay][0] = 0xEB;
                desired[SiteExtendedDisplay][1] = gLayout.siteEnabled[SiteExtendedDisplay][1];
            }
        }
    }
    bool phaseInstalled = false;
    if (settings.phaseEnabled && gPhaseDataAvailable && gSites[SiteWaitCall].available) {
        if (!PhaseCallBytes(desired[SiteWaitCall],
            reinterpret_cast<uintptr_t>(At(gSites[SiteWaitCall].address)))) {
            phaseUnavailable = true;
            SetErrorIfEmpty(L"阶段加速跳板无法安装，已停用阶段加速。请检查游戏模块与补丁 DLL 的地址距离。");
        } else {
            phaseInstalled = true;
        }
    }
    if (gSites[SiteInputModifier].available)
        desired[SiteInputModifier][0] = settings.shiftKey ? 0x10 : 0x11;

    BYTE oldPhase[4] = {};
    BYTE newPhase[4] = {};
    const bool phaseWrite = settings.phaseEnabled && phaseInstalled;
    if (phaseUnavailable) SetErrorIfEmpty(L"阶段加速原生阶段表无法定位，已停用阶段加速。");
    const bool nativePhaseData = gPhaseDataAvailable && !gLayout.phaseOwnedByPatch;
    if (nativePhaseData) {
        if (!ReadBytes(gLayout.phaseTable, oldPhase, sizeof(oldPhase))) {
            SetError(L"无法读取阶段表当前值，已停用阶段加速。");
            return false;
        }
        if (phaseWrite) {
            for (int phase = 0; phase < 3; ++phase)
                newPhase[phase] = settings.phaseFlags[phase] ? static_cast<BYTE>(settings.phase[phase]) : 50;
            newPhase[3] = 0;
        } else memcpy(newPhase, gPhaseOriginal, sizeof(newPhase));
    }
    const bool phaseChanged = nativePhaseData && memcmp(oldPhase, newPhase, sizeof(newPhase)) != 0;
    BYTE oldNativeState = 0;
    BYTE oldNativeBusy = 0;
    const BYTE busy = 1;
    if (gNativeDataAvailable &&
        (!ReadBytes(gLayout.nativeSuperState, &oldNativeState, 1) ||
         !ReadBytes(gLayout.nativeSuperBusy, &oldNativeBusy, 1) ||
         !WriteMemory(gLayout.nativeSuperBusy, &busy, 1))) {
        SetError(L"无法锁定 原生超级加速状态，本次设置未生效。");
        return false;
    }
    if (phaseChanged && !WriteMemory(gLayout.phaseTable, newPhase, sizeof(newPhase))) {
        if (gNativeDataAvailable) WriteMemory(gLayout.nativeSuperBusy, &oldNativeBusy, 1);
        SetError(L"阶段参数写入失败，已回滚本次设置。");
        return false;
    }
    if (phaseChanged) JournalExpected(gPhaseJournal, newPhase);
    BYTE previous[kSiteCount][8] = {};
    for (size_t i = 0; i < kSiteCount; ++i) memcpy(previous[i], gSites[i].current, gSites[i].length);
    if (!ApplyBytes(desired)) {
        if (phaseChanged && WriteMemory(gLayout.phaseTable, oldPhase, sizeof(oldPhase)))
            JournalExpected(gPhaseJournal, oldPhase);
        if (gNativeDataAvailable && WriteMemory(gLayout.nativeSuperBusy, &oldNativeBusy, 1))
            JournalExpected(gNativeBusyJournal, &oldNativeBusy);
        return false;
    }
    if (gNativeDataAvailable) {
        const BYTE state = settings.superEnabled ? 1 : 0;
        const BYTE unlocked = 0;
        if (!WriteMemory(gLayout.nativeSuperState, &state, 1) ||
            !WriteMemory(gLayout.nativeSuperBusy, &unlocked, 1)) {
            ApplyBytes(previous);
            if (phaseChanged && WriteMemory(gLayout.phaseTable, oldPhase, sizeof(oldPhase)))
                JournalExpected(gPhaseJournal, oldPhase);
            if (WriteMemory(gLayout.nativeSuperState, &oldNativeState, 1))
                JournalExpected(gNativeStateJournal, &oldNativeState);
            if (WriteMemory(gLayout.nativeSuperBusy, &oldNativeBusy, 1))
                JournalExpected(gNativeBusyJournal, &oldNativeBusy);
            SetError(L"原生超级加速状态同步失败，本次设置已回滚。");
            return false;
        }
        JournalExpected(gNativeStateJournal, &state);
        JournalExpected(gNativeBusyJournal, &unlocked);
    }
    if (nativePhaseData) memcpy(gPhaseCurrent, newPhase, 3);
    if (!nativeRouteInstalled && (gLayout.capabilities & CapabilityNativeSuper))
        SetErrorIfEmpty(L"原生 Z 键入口接管失败；已停用原生快捷键兜底，其它可用功能保持不变。");
    if (settings.superEnabled && settings.superFlags[0] && !dialogReady)
        SetErrorIfEmpty(L"超级加速对白功能定位失败，已停用该子项。");
    if (settings.superEnabled && settings.superFlags[1] && !mapReady)
        SetErrorIfEmpty(L"超级加速地图文字功能定位失败，已停用该子项。");
    if (settings.superEnabled && settings.superFlags[2] && !delayReady)
        SetErrorIfEmpty(L"超级加速延时指令功能定位失败，已停用该子项。");
    if (settings.superEnabled && settings.superFlags[3] && !rReady)
        SetErrorIfEmpty(L"超级加速 R 动作功能定位失败，已停用该子项。");
    if (settings.superEnabled && settings.superFlags[4] && !superSInstalled)
        SetErrorIfEmpty(L"超级加速 S 调度跳板无法安装，已停用超级 S；其它超级功能保持可用。");
    gSuperR = settings.superEnabled && settings.superFlags[3] && rReady;
    gSuperS = settings.superEnabled && settings.superFlags[4] && superSInstalled;
    gPhaseInstalled = phaseInstalled;
    gSuperScheduled = false;
    gSuperLast = 0;
    AcquireSRWLockExclusive(&gPhaseLock);
    gPhase.enabled = phaseInstalled && !phaseUnavailable;
    for (int phase = 0; phase < 3; ++phase) {
        if (gPhase.values[phase] != settings.phase[phase] ||
            gPhase.flags[phase] != settings.phaseFlags[phase] || !phaseInstalled)
            gPhase.counters[phase] = 0;
        gPhase.flags[phase] = settings.phaseFlags[phase];
        gPhase.values[phase] = settings.phase[phase];
    }
    ReleaseSRWLockExclusive(&gPhaseLock);
    return true;
}

void RestoreEngine() {
    if (!gInitialized) return;
    SetError(L"");
    bool restored = RestoreSiteJournals();
    if (!restored) return;
    // The first three phase bytes are settings owned by this patch; the fourth
    // is the game's live counter. Native state/busy are also live game fields.
    // Restore their snapshots without treating normal game updates as conflicts.
    restored = RestoreMemoryJournal(gPhaseJournal,
        L"运行时恢复冲突：阶段表已被其它补丁修改，未覆盖现有字节。", 3) && restored;
    restored = RestoreMemoryJournal(gNativeStateJournal,
        L"运行时恢复失败：无法恢复原生加速状态。", 0) && restored;
    restored = RestoreMemoryJournal(gNativeBusyJournal,
        L"运行时恢复失败：无法恢复原生加速忙标志。", 0) && restored;
    if (restored && gPhaseDataAvailable) memcpy(gPhaseCurrent, gPhaseOriginal, 3);
    gPhaseInstalled = false;
    if (restored) {
        AcquireSRWLockExclusive(&gPhaseLock);
        gPhase = {};
        ReleaseSRWLockExclusive(&gPhaseLock);
    }
}

const EnginePatchPlan& RuntimeEnginePatchPlan() {
    return gLayout.plan;
}

const wchar_t* EngineError() {
    return gError;
}

void SetNativeToggleHandler(void (__cdecl* handler)()) {
    gNativeToggleHandler = handler;
}

#ifdef XA66_TEST
bool EngineTestWriteCode(uintptr_t address, const BYTE* bytes, size_t length) {
    return WriteProtectedBytes(address, bytes, length, true);
}
bool EngineTestInitialize() { gTestSyntheticDataImage = true; return InitializeSites(false); }
bool EngineTestInitializeImage(uintptr_t virtualBase) {
    gTestSyntheticDataImage = true;
    return InitializeSites(true, virtualBase);
}
bool EngineTestInitializeRuntimeImage(uintptr_t virtualBase) {
    gTestSyntheticDataImage = false;
    return InitializeSites(true, virtualBase);
}
bool EngineTestInitializeVersionImage(uintptr_t virtualBase, EngineProfileId profile) {
    gTestSyntheticDataImage = true;
    return InitializeSites(true, virtualBase, true, profile);
}
bool EngineTestOwnedPhaseState(int phase, int& value, int& counter) {
    if (!gLayout.phaseOwnedByPatch || !gPhaseInstalled || !gPhase.enabled || phase < 0 || phase >= 3) return false;
    value = gPhase.flags[phase] ? gPhase.values[phase] : 50;
    counter = gPhase.counters[phase];
    return true;
}
void EngineTestFailWrite(int index) { gFailWrite = index; gWriteCount = 0; }
int EngineTestSuperCaller(uintptr_t caller) { return SuperYieldWithContext(caller, 0); }
void EngineTestSuperWhitelist(uintptr_t caller) {
    for (bool& value : gFeatureSettings.animationCamps) value = true;
    for (bool& value : gFeatureSettings.animationKinds) value = true;
    gLayout.waitCallerCount = caller ? 1 : 0;
    gLayout.waitCallers[0] = caller;
    gLayout.waitKinds[0] = static_cast<unsigned char>(AnimationKind::Physical);
    gSuperS = caller != 0;
}
void EngineTestSuperCallbacks(ULONGLONG (WINAPI* clock)(), void (WINAPI* sleep)(DWORD),
    bool (__cdecl* pump)()) {
    gTestSuperClock = clock;
    gTestSuperSleep = sleep;
    gTestSuperPump = pump;
    gSuperBusy = gSuperScheduled = false;
    gSuperLast = 0;
}
// Exercise the actual assembly bridge with functions that require an ECX
// context. This catches an ABI regression that offline byte tests cannot.
void WINAPI EngineTestNoSleep(DWORD) {}
int EngineTestTimerABI(bool usesThis, uintptr_t stage, uintptr_t deadline, uintptr_t context,
    bool nativeFallback) {
    const EngineLayout savedLayout = gLayout;
    const uintptr_t savedBase = gGameBase;
    const bool savedInstalled = gPhaseInstalled;
    const bool savedAvailable = gPhaseDataAvailable;
    const bool savedDirectPointers = gTestDirectPointers;
    WORD speed = 0;
    BYTE phase = 0;
    BYTE table[4] = {60,60,60,0};
    gGameBase = kPreferredBase;
    gTestDirectPointers = true;
    gLayout = EngineLayout();
    gLayout.stageUsesThis = usesThis;
    gLayout.nativeStage = stage;
    gLayout.deadline = deadline;
    gLayout.sleep = reinterpret_cast<uintptr_t>(&EngineTestNoSleep);
    gLayout.speed = reinterpret_cast<uintptr_t>(&speed);
    gLayout.phase = reinterpret_cast<uintptr_t>(&phase);
    gLayout.phaseTable = reinterpret_cast<uintptr_t>(table);
    gLayout.phaseCounter = reinterpret_cast<uintptr_t>(table + 3);
    gPhaseDataAvailable = true;
    gPhaseInstalled = !nativeFallback;
    int result = 0;
    __asm {
        mov ecx, context
        call LegacyPhaseBridge
        mov result, eax
    }
    gLayout = savedLayout;
    gGameBase = savedBase;
    gPhaseInstalled = savedInstalled;
    gPhaseDataAvailable = savedAvailable;
    gTestDirectPointers = savedDirectPointers;
    return result;
}
int EngineTestSuperWaitABI(bool usesThis, uintptr_t stage, uintptr_t context, uintptr_t caller,
    bool enabled, bool known, int& polls) {
    const EngineLayout savedLayout = gLayout;
    const uintptr_t savedBase = gGameBase;
    const bool savedSuperS = gSuperS;
    gGameBase = kPreferredBase;
    gLayout = EngineLayout();
    gLayout.stageUsesThis = usesThis;
    gLayout.nativeStage = stage;
    gLayout.waitCallerCount = known ? 1 : 0;
    gLayout.waitCallers[0] = caller;
    gLayout.waitKinds[0] = static_cast<unsigned char>(AnimationKind::Physical);
    gSuperS = enabled;
    polls = 0;
    int result = 0;
    // Reproduce the wrapper's saved EBP and caller-return positions, then
    // execute its actual zero/retry and nonzero/complete contract.
    while (!result && polls < 8) {
        ++polls;
        __asm {
            mov ecx, context
            push caller
            push ebp
            call SuperYieldBridge
            add esp, 8
            mov result, eax
        }
    }
    gLayout = savedLayout;
    gGameBase = savedBase;
    gSuperS = savedSuperS;
    return result;
}
ULONGLONG EngineTestRealClock() {
    auto saved = gTestSuperClock;
    gTestSuperClock = nullptr;
    const ULONGLONG value = SuperClock();
    gTestSuperClock = saved;
    return value;
}
#endif
