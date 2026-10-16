#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

namespace {

typedef DWORD (WINAPI *TickFn)();
typedef BOOL (WINAPI *CounterFn)(LARGE_INTEGER*);
typedef LONG (WINAPI *MessageTimeFn)();
typedef UINT_PTR (WINAPI *TimerFn)(HWND, UINT_PTR, UINT, TIMERPROC);
typedef MMRESULT (WINAPI *EventFn)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);

TickFn realTick = NULL;
TickFn realTime = NULL;
CounterFn realCounter = NULL;
MessageTimeFn realMessageTime = NULL;
TimerFn realTimer = NULL;
EventFn realEvent = NULL;
INIT_ONCE initializeOnce = INIT_ONCE_STATIC_INIT;
SRWLOCK clockLock = SRWLOCK_INIT;
SRWLOCK hookLock = SRWLOCK_INIT;
int rawSpeed = 0;
double multiplier = 1.0;
bool clockReady = false;
bool intercepting = false;

struct MillisecondClock {
    DWORD previousReal;
    double virtualValue;
};

MillisecondClock tickClock = {};
MillisecondClock multimediaClock = {};
LONGLONG previousCounter = 0;
double virtualCounter = 0;
double counterMilliseconds = 0;

int ClampSpeed(int value) {
    return value < -128 ? -128 : (value > 128 ? 128 : value);
}

double SpeedMultiplier(int value) {
    return pow(2.0, ClampSpeed(value) / 10.0);
}

DWORD WrapMilliseconds(double value) {
    if (value >= 0 && value < 4294967296.0) return static_cast<DWORD>(value);
    double wrapped = fmod(value, 4294967296.0);
    if (wrapped < 0) wrapped += 4294967296.0;
    return static_cast<DWORD>(wrapped);
}

void AddMilliseconds(MillisecondClock& clock, double elapsed) {
    clock.virtualValue += elapsed;
    if (clock.virtualValue >= 4294967296.0)
        clock.virtualValue = fmod(clock.virtualValue, 4294967296.0);
}

void AdvanceMilliseconds(MillisecondClock& clock, DWORD realNow) {
    AddMilliseconds(clock, static_cast<DWORD>(realNow - clock.previousReal) * multiplier);
    clock.previousReal = realNow;
}

void AdvanceCounter(LONGLONG realNow) {
    if (realNow >= previousCounter)
        virtualCounter += static_cast<double>(realNow - previousCounter) * multiplier;
    previousCounter = realNow;
}

void AdvancePreciseClocks(LONGLONG realNow) {
    // Magnifying a native 10-16 ms tick magnifies animation jumps too. Use one
    // QPC delta for both accelerated millisecond clocks; keep native 1x ticks.
    if (rawSpeed != 0 && realNow >= previousCounter) {
        double elapsed = static_cast<double>(realNow - previousCounter)
            * multiplier * counterMilliseconds;
        AddMilliseconds(tickClock, elapsed);
        AddMilliseconds(multimediaClock, elapsed);
    }
    AdvanceCounter(realNow);
}

LONGLONG CounterValue() {
    // Keep pathological uptime/rate combinations within the signed QPC ABI.
    return virtualCounter >= 9223372036854774784.0
        ? 9223372036854774784LL : static_cast<LONGLONG>(virtualCounter);
}

UINT ScaleInterval(UINT interval, double speed) {
    double scaled = interval / speed;
    if (scaled < 1.0) return 1;
    if (scaled >= 2147483647.0) return 0x7fffffffU;
    return static_cast<UINT>(scaled);
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    HMODULE user = GetModuleHandleW(L"user32.dll");
    HMODULE mm = GetModuleHandleW(L"winmm.dll");
    if (!user) user = LoadLibraryW(L"user32.dll");
    if (!mm) mm = LoadLibraryW(L"winmm.dll");
    if (!kernel || !user || !mm) return FALSE;
    realTick = reinterpret_cast<TickFn>(GetProcAddress(kernel, "GetTickCount"));
    realTime = reinterpret_cast<TickFn>(GetProcAddress(mm, "timeGetTime"));
    realCounter = reinterpret_cast<CounterFn>(GetProcAddress(kernel, "QueryPerformanceCounter"));
    realMessageTime = reinterpret_cast<MessageTimeFn>(GetProcAddress(user, "GetMessageTime"));
    realTimer = reinterpret_cast<TimerFn>(GetProcAddress(user, "SetTimer"));
    realEvent = reinterpret_cast<EventFn>(GetProcAddress(mm, "timeSetEvent"));
    if (!realTick || !realTime || !realCounter || !realMessageTime || !realTimer || !realEvent)
        return FALSE;
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    if (!realCounter(&counter) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return FALSE;
    counterMilliseconds = 1000.0 / static_cast<double>(frequency.QuadPart);
    tickClock.previousReal = realTick();
    tickClock.virtualValue = tickClock.previousReal;
    multimediaClock.previousReal = realTime();
    multimediaClock.virtualValue = multimediaClock.previousReal;
    previousCounter = counter.QuadPart;
    virtualCounter = static_cast<double>(counter.QuadPart);
    clockReady = true;
    return TRUE;
}

bool EnsureInitialized() {
    return InitOnceExecuteOnce(&initializeOnce, Initialize, NULL, NULL) != FALSE;
}

DWORD WINAPI AcceleratedTick() {
    AcquireSRWLockExclusive(&clockLock);
    if (rawSpeed == 0) AdvanceMilliseconds(tickClock, realTick());
    else {
        LARGE_INTEGER counter;
        if (realCounter(&counter)) AdvancePreciseClocks(counter.QuadPart);
    }
    DWORD value = WrapMilliseconds(tickClock.virtualValue);
    ReleaseSRWLockExclusive(&clockLock);
    return value;
}

DWORD WINAPI AcceleratedTime() {
    AcquireSRWLockExclusive(&clockLock);
    if (rawSpeed == 0) AdvanceMilliseconds(multimediaClock, realTime());
    else {
        LARGE_INTEGER counter;
        if (realCounter(&counter)) AdvancePreciseClocks(counter.QuadPart);
    }
    DWORD value = WrapMilliseconds(multimediaClock.virtualValue);
    ReleaseSRWLockExclusive(&clockLock);
    return value;
}

BOOL WINAPI AcceleratedCounter(LARGE_INTEGER* result) {
    if (!result) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    AcquireSRWLockExclusive(&clockLock);
    LARGE_INTEGER current;
    BOOL succeeded = realCounter(&current);
    if (succeeded) {
        AdvancePreciseClocks(current.QuadPart);
        result->QuadPart = CounterValue();
    }
    ReleaseSRWLockExclusive(&clockLock);
    return succeeded;
}

LONG WINAPI AcceleratedMessageTime() {
    AcquireSRWLockExclusive(&clockLock);
    DWORD now = realTick();
    if (rawSpeed == 0) AdvanceMilliseconds(tickClock, now);
    else {
        LARGE_INTEGER counter;
        if (realCounter(&counter)) AdvancePreciseClocks(counter.QuadPart);
    }
    LONG displacement = static_cast<LONG>(static_cast<DWORD>(realMessageTime()) - now);
    LONG value = static_cast<LONG>(WrapMilliseconds(tickClock.virtualValue + displacement * multiplier));
    ReleaseSRWLockExclusive(&clockLock);
    return value;
}

double CurrentMultiplier() {
    AcquireSRWLockShared(&clockLock);
    double value = multiplier;
    ReleaseSRWLockShared(&clockLock);
    return value;
}

UINT_PTR WINAPI AcceleratedTimer(HWND window, UINT_PTR id, UINT interval, TIMERPROC callback) {
    return realTimer(window, id, ScaleInterval(interval, CurrentMultiplier()), callback);
}

MMRESULT WINAPI AcceleratedEvent(UINT delay, UINT resolution, LPTIMECALLBACK callback,
                                DWORD_PTR user, UINT flags) {
    return realEvent(ScaleInterval(delay, CurrentMultiplier()), resolution, callback, user, flags);
}

struct HookDefinition {
    const char* name;
    void* replacement;
    void* realFunction;
};

struct HookRecord {
    HMODULE image;
    void** slot;
    void* original;
    void* replacement;
};

const size_t maxHooks = 128;
HookRecord hooks[maxHooks] = {};
size_t hookCount = 0;

bool Readable(const void* pointer, size_t bytes) {
    uintptr_t current = reinterpret_cast<uintptr_t>(pointer);
    if (bytes > UINTPTR_MAX - current) return false;
    uintptr_t end = current + bytes;
    while (current < end) {
        MEMORY_BASIC_INFORMATION page;
        if (!VirtualQuery(reinterpret_cast<void*>(current), &page, sizeof(page))) return false;
        if (page.State != MEM_COMMIT || (page.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        uintptr_t next = reinterpret_cast<uintptr_t>(page.BaseAddress) + page.RegionSize;
        if (next <= current) return false;
        current = next;
    }
    return true;
}

bool InImage(const BYTE* image, DWORD size, DWORD offset, size_t bytes) {
    uintptr_t base = reinterpret_cast<uintptr_t>(image);
    return offset <= size && bytes <= size - offset && offset <= UINTPTR_MAX - base
        && Readable(reinterpret_cast<const void*>(base + offset), bytes);
}

bool ImageString(const BYTE* image, DWORD size, DWORD offset) {
    if (offset >= size) return false;
    // Import identifiers are short; reject unterminated or malformed strings.
    size_t limit = size - offset;
    if (limit > 512) limit = 512;
    for (size_t i = 0; i < limit; ++i) {
        if (!Readable(image + offset + i, 1)) return false;
        if (!image[offset + i]) return true;
    }
    return false;
}

bool RelevantProvider(const char* name) {
    return _stricmp(name, "kernel32.dll") == 0 || _stricmp(name, "kernelbase.dll") == 0
        || _stricmp(name, "winmm.dll") == 0 || _stricmp(name, "user32.dll") == 0
        || _strnicmp(name, "api-ms-win-core-", 16) == 0;
}

bool ChangeSlot(void** slot, void* expected, void* value) {
    DWORD previous;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) return false;
    void* observed = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), value, expected);
    DWORD ignored;
    BOOL restored = VirtualProtect(slot, sizeof(void*), previous, &ignored);
    if (!restored && observed == expected) {
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), expected, value);
        VirtualProtect(slot, sizeof(void*), previous, &ignored);
        return false;
    }
    return observed == expected;
}

bool HookImage(HMODULE module, HookRecord* additions = NULL, size_t* additionCount = NULL) {
    BYTE* image = reinterpret_cast<BYTE*>(module);
    if (!Readable(image, sizeof(IMAGE_DOS_HEADER))) return false;
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER)
        || dos->e_lfanew > 1048576) return false;
    IMAGE_NT_HEADERS32* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    if (!Readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE
        || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386
        || nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32)
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
    DWORD size = nt->OptionalHeader.SizeOfImage;
    if (size < static_cast<DWORD>(dos->e_lfanew) + sizeof(*nt)
        || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return false;
    DWORD imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imports) return true;

    HookDefinition definitions[] = {
        {"GetTickCount", reinterpret_cast<void*>(AcceleratedTick), reinterpret_cast<void*>(realTick)},
        {"timeGetTime", reinterpret_cast<void*>(AcceleratedTime), reinterpret_cast<void*>(realTime)},
        {"QueryPerformanceCounter", reinterpret_cast<void*>(AcceleratedCounter), reinterpret_cast<void*>(realCounter)},
        {"GetMessageTime", reinterpret_cast<void*>(AcceleratedMessageTime), reinterpret_cast<void*>(realMessageTime)},
        {"SetTimer", reinterpret_cast<void*>(AcceleratedTimer), reinterpret_cast<void*>(realTimer)},
        {"timeSetEvent", reinterpret_cast<void*>(AcceleratedEvent), reinterpret_cast<void*>(realEvent)}
    };
    HookRecord pending[maxHooks];
    size_t pendingCount = 0;
    bool terminated = false;
    // The target's directory size is shorter than its actual descriptor array.
    // Validate the complete zero-terminated array before changing any IAT slot.
    for (DWORD offset = imports; InImage(image, size, offset, sizeof(IMAGE_IMPORT_DESCRIPTOR));
         offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image + offset);
        if (!descriptor->OriginalFirstThunk && !descriptor->TimeDateStamp && !descriptor->ForwarderChain
            && !descriptor->Name && !descriptor->FirstThunk) {
            terminated = true;
            break;
        }
        if (!ImageString(image, size, descriptor->Name) || !descriptor->FirstThunk) return false;
        if (!RelevantProvider(reinterpret_cast<char*>(image + descriptor->Name))) continue;
        // Added 6.6 imports use one table for lookup and IAT; the loader has
        // replaced those identifiers with resolved API addresses.
        DWORD lookup = descriptor->OriginalFirstThunk == descriptor->FirstThunk
            ? 0 : descriptor->OriginalFirstThunk;
        DWORD table = descriptor->FirstThunk;
        bool thunkTerminated = false;
        for (DWORD index = 0; index < size / sizeof(IMAGE_THUNK_DATA32); ++index) {
            size_t displacement = static_cast<size_t>(index) * sizeof(IMAGE_THUNK_DATA32);
            if (displacement > size || table > size - displacement
                || !InImage(image, size, table + static_cast<DWORD>(displacement), sizeof(IMAGE_THUNK_DATA32)))
                return false;
            IMAGE_THUNK_DATA32* thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(image + table + displacement);
            if (!thunk->u1.Function) { thunkTerminated = true; break; }
            const char* name = NULL;
            if (lookup) {
                if (lookup > size - displacement
                    || !InImage(image, size, lookup + static_cast<DWORD>(displacement), sizeof(IMAGE_THUNK_DATA32)))
                    return false;
                DWORD identifier = reinterpret_cast<IMAGE_THUNK_DATA32*>(image + lookup + displacement)->u1.AddressOfData;
                if (!identifier) return false;
                if (IMAGE_SNAP_BY_ORDINAL32(identifier)) continue;
                if (!InImage(image, size, identifier, sizeof(WORD)) || identifier > size - sizeof(WORD)
                    || !ImageString(image, size, identifier + sizeof(WORD))) return false;
                name = reinterpret_cast<char*>(image + identifier + sizeof(WORD));
            }
            for (size_t which = 0; which < sizeof(definitions) / sizeof(definitions[0]); ++which) {
                HookDefinition& definition = definitions[which];
                void* current = reinterpret_cast<void*>(thunk->u1.Function);
                bool matches = name ? strcmp(name, definition.name) == 0 : current == definition.realFunction;
                if (!matches || current == definition.replacement) continue;
                bool recorded = false;
                for (size_t previous = 0; previous < hookCount; ++previous)
                    if (hooks[previous].slot == reinterpret_cast<void**>(&thunk->u1.Function)) recorded = true;
                for (size_t previous = 0; previous < pendingCount; ++previous)
                    if (pending[previous].slot == reinterpret_cast<void**>(&thunk->u1.Function)) recorded = true;
                if (recorded) continue;
                if (pendingCount >= maxHooks || hookCount + pendingCount >= maxHooks) return false;
                pending[pendingCount++] = {module, reinterpret_cast<void**>(&thunk->u1.Function), current, definition.replacement};
                break;
            }
        }
        if (!thunkTerminated) return false;
    }
    if (!terminated) return false;
    for (size_t i = 0; i < pendingCount; ++i) {
        HookRecord& record = pending[i];
        if (!ChangeSlot(record.slot, record.original, record.replacement)) {
            // Roll back this image if protection changes or a competing hook prevented installation.
            for (size_t j = 0; j < i; ++j)
                ChangeSlot(pending[j].slot, pending[j].replacement, pending[j].original);
            return false;
        }
    }
    for (size_t i = 0; i < pendingCount; ++i) {
        hooks[hookCount++] = pending[i];
        if (additions && additionCount) additions[(*additionCount)++] = pending[i];
    }
    return true;
}

struct ModuleIdentity {
    DWORD timestamp;
    DWORD imageSize;
    DWORD checksum;
    DWORD entryPoint;
    DWORD importRva;
    LONG headerOffset;
};

struct ModuleCache {
    HMODULE image;
    ModuleIdentity identity;
    void** slots[maxHooks];
    void* values[maxHooks];
    size_t slotCount;
    bool valid;
};

ModuleCache mainCache = {};
ModuleCache extensionCache = {};

bool ReadIdentity(HMODULE module, ModuleIdentity& identity) {
    BYTE* image = reinterpret_cast<BYTE*>(module);
    if (!module || !Readable(image, sizeof(IMAGE_DOS_HEADER))) return false;
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER)
        || dos->e_lfanew > 1048576) return false;
    const IMAGE_NT_HEADERS32* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    if (!Readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE
        || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC
        || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return false;
    identity = {nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
        nt->OptionalHeader.CheckSum, nt->OptionalHeader.AddressOfEntryPoint,
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress, dos->e_lfanew};
    return true;
}

bool SameIdentity(const ModuleIdentity& left, const ModuleIdentity& right) {
    return memcmp(&left, &right, sizeof(left)) == 0;
}

bool CacheMatches(const ModuleCache& cache, HMODULE module) {
    ModuleIdentity identity;
    if (!cache.valid || cache.image != module || !ReadIdentity(module, identity)
        || !SameIdentity(cache.identity, identity)) return false;
    uintptr_t base = reinterpret_cast<uintptr_t>(module);
    for (size_t i = 0; i < cache.slotCount; ++i) {
        uintptr_t slot = reinterpret_cast<uintptr_t>(cache.slots[i]);
        if (slot < base || slot - base > identity.imageSize
            || !InImage(reinterpret_cast<BYTE*>(module), identity.imageSize,
                static_cast<DWORD>(slot - base), sizeof(void*))
            || *cache.slots[i] != cache.values[i]) return false;
    }
    return true;
}

void CaptureCache(ModuleCache& cache, HMODULE module) {
    cache = {};
    if (!ReadIdentity(module, cache.identity)) return;
    cache.image = module;
    for (size_t i = 0; i < hookCount; ++i) {
        if (hooks[i].image != module) continue;
        cache.slots[cache.slotCount] = hooks[i].slot;
        cache.values[cache.slotCount++] = *hooks[i].slot;
    }
    cache.valid = true;
}

bool LiveRecord(const HookRecord& record) {
    MEMORY_BASIC_INFORMATION page;
    return VirtualQuery(record.slot, &page, sizeof(page)) && page.AllocationBase == record.image
        && Readable(record.slot, sizeof(void*));
}

void PrepareCache(ModuleCache& cache, HMODULE module) {
    ModuleIdentity identity;
    bool identityValid = ReadIdentity(module, identity);
    bool moved = cache.image && cache.image != module;
    bool changed = cache.image == module && cache.valid
        && (!identityValid || !SameIdentity(cache.identity, identity));
    size_t keep = 0;
    for (size_t i = 0; i < hookCount; ++i) {
        HookRecord& record = hooks[i];
        if (moved && record.image == cache.image) {
            if (LiveRecord(record)) ChangeSlot(record.slot, record.replacement, record.original);
            continue;
        }
        if (record.image == module) {
            // An unloaded/reloaded image can retain the same address and PE identity.
            // Its restored original IAT pointers must be eligible for installation again.
            if (changed) {
                if (LiveRecord(record)) ChangeSlot(record.slot, record.replacement, record.original);
                continue;
            }
            if (!LiveRecord(record) || *record.slot == record.original) continue;
        }
        hooks[keep++] = record;
    }
    hookCount = keep;
    if (moved || changed || !module) cache = {};
}

bool HookKnownImagesFor(HMODULE main, HMODULE extension) {
    bool mainCached = CacheMatches(mainCache, main);
    bool extensionCached = extension && CacheMatches(extensionCache, extension);
    if (mainCached && (extensionCached || (!extension && !extensionCache.image))) return true;
    if (!mainCached) PrepareCache(mainCache, main);
    if (!extensionCached) PrepareCache(extensionCache, extension);
    HookRecord before[maxHooks];
    memcpy(before, hooks, sizeof(before));
    size_t beforeCount = hookCount;
    HookRecord additions[maxHooks];
    size_t additionCount = 0;
    bool succeeded = mainCached || HookImage(main, additions, &additionCount);
    if (succeeded && extension && !extensionCached)
        succeeded = HookImage(extension, additions, &additionCount);
    if (!succeeded) {
        for (size_t i = additionCount; i > 0; --i)
            ChangeSlot(additions[i - 1].slot, additions[i - 1].replacement, additions[i - 1].original);
        memcpy(hooks, before, sizeof(before));
        hookCount = beforeCount;
        if (!mainCached) mainCache.valid = false;
        if (!extensionCached) extensionCache.valid = false;
        return false;
    }
    if (!mainCached) CaptureCache(mainCache, main);
    if (extension && !extensionCached) CaptureCache(extensionCache, extension);
    return true;
}

bool HookKnownImages() {
    return HookKnownImagesFor(GetModuleHandleW(NULL), GetModuleHandleW(L"ExtendData.dll"));
}

void RestoreHooks() {
    for (size_t i = 0; i < hookCount; ++i) {
        HookRecord& record = hooks[i];
        MEMORY_BASIC_INFORMATION page;
        if (VirtualQuery(record.slot, &page, sizeof(page)) && page.AllocationBase == record.image
            && Readable(record.slot, sizeof(void*)))
            ChangeSlot(record.slot, record.replacement, record.original);
    }
    hookCount = 0;
    mainCache = {};
    extensionCache = {};
}

} // namespace

extern "C" BOOL __cdecl AddProcess(HANDLE process) {
    if (GetProcessId(process) != GetCurrentProcessId()) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!EnsureInitialized()) return FALSE;
    AcquireSRWLockExclusive(&hookLock);
    bool succeeded = HookKnownImages();
    if (succeeded) intercepting = true;
    ReleaseSRWLockExclusive(&hookLock);
    return succeeded ? TRUE : FALSE;
}

extern "C" int __cdecl GetSpeed() {
    AcquireSRWLockShared(&clockLock);
    int value = rawSpeed;
    ReleaseSRWLockShared(&clockLock);
    return value;
}

extern "C" UINT_PTR WINAPI OldSetTimer(HWND window, UINT_PTR id, UINT interval, TIMERPROC callback) {
    if (!EnsureInitialized()) return 0;
    return realTimer(window, id, interval, callback);
}

extern "C" void __cdecl SetSpeed(int value) {
    if (!EnsureInitialized()) return;
    value = ClampSpeed(value);
    AcquireSRWLockExclusive(&clockLock);
    if (clockReady) {
        if (rawSpeed == 0) {
            AdvanceMilliseconds(tickClock, realTick());
            AdvanceMilliseconds(multimediaClock, realTime());
        }
        LARGE_INTEGER counter;
        if (realCounter(&counter)) AdvancePreciseClocks(counter.QuadPart);
        tickClock.previousReal = realTick();
        multimediaClock.previousReal = realTime();
    }
    rawSpeed = value;
    multiplier = SpeedMultiplier(value);
    ReleaseSRWLockExclusive(&clockLock);
    AcquireSRWLockExclusive(&hookLock);
    if (intercepting) HookKnownImages();
    ReleaseSRWLockExclusive(&hookLock);
}

extern "C" void __cdecl UnIntercept() {
    SetSpeed(0);
    AcquireSRWLockExclusive(&hookLock);
    RestoreHooks();
    intercepting = false;
    ReleaseSRWLockExclusive(&hookLock);
}

#ifndef GEAR_UNIT_TEST
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
#endif
