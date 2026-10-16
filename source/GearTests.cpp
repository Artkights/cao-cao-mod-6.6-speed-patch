#define GEAR_UNIT_TEST
#include "GearNtKe.cpp"
#include <stdio.h>
#include <stdlib.h>

namespace {
int failures = 0;
DWORD fakeTickValue = 0;
DWORD fakeTimeValue = 0;
LONGLONG fakeCounterValue = 0;
LONG fakeMessageValue = 0;
UINT capturedInterval = 0;
UINT capturedResolution = 0;
UINT capturedFlags = 0;
volatile LONG concurrentFailures = 0;
DWORD WINAPI FakeTick() { return fakeTickValue; }
DWORD WINAPI FakeTime() { return fakeTimeValue; }
BOOL WINAPI FakeCounter(LARGE_INTEGER* value) { value->QuadPart = fakeCounterValue; return TRUE; }
BOOL WINAPI FailedCounter(LARGE_INTEGER*) { return FALSE; }
LONG WINAPI FakeMessage() { return fakeMessageValue; }
UINT_PTR WINAPI FakeTimer(HWND, UINT_PTR id, UINT interval, TIMERPROC) {
    capturedInterval = interval; return id ? id : 123;
}
MMRESULT WINAPI FakeEvent(UINT delay, UINT resolution, LPTIMECALLBACK, DWORD_PTR, UINT flags) {
    capturedInterval = delay; capturedResolution = resolution; capturedFlags = flags; return 456;
}
void Check(bool condition, const char* label) {
    if (!condition) { ++failures; printf("FAIL: %s\n", label); }
}
void ResetClocks(DWORD tick = 1000, DWORD time = 2000, LONGLONG counter = 100000) {
    EnsureInitialized();
    realTick = FakeTick; realTime = FakeTime; realCounter = FakeCounter;
    realMessageTime = FakeMessage; realTimer = FakeTimer; realEvent = FakeEvent;
    fakeTickValue = tick; fakeTimeValue = time; fakeCounterValue = counter;
    tickClock = {tick, static_cast<double>(tick)};
    multimediaClock = {time, static_cast<double>(time)};
    previousCounter = counter; virtualCounter = static_cast<double>(counter);
    counterMilliseconds = 0.1;
    multiplier = 1.0; rawSpeed = 0; clockReady = true; intercepting = false;
}

struct MockImage {
    BYTE* image;
    IMAGE_NT_HEADERS32* nt;
    IMAGE_IMPORT_DESCRIPTOR* imports;
    IMAGE_THUNK_DATA32* kernelIat;
    IMAGE_THUNK_DATA32* mmIat;
    IMAGE_THUNK_DATA32* userIat;
    MockImage() {
        image = static_cast<BYTE*>(VirtualAlloc(NULL, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!image) abort();
        IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
        dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
        nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image + 0x80);
        nt->Signature = IMAGE_NT_SIGNATURE; nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        nt->OptionalHeader.SizeOfImage = 0x2000;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {0x200, 20};
        imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image + 0x200);
        imports[0].Name = 0x400; imports[0].FirstThunk = 0x500; imports[0].OriginalFirstThunk = 0x600;
        imports[1].Name = 0x420; imports[1].FirstThunk = 0x540; imports[1].OriginalFirstThunk = 0x640;
        imports[2].Name = 0x440; imports[2].FirstThunk = 0x580; imports[2].OriginalFirstThunk = 0x680;
        strcpy_s(reinterpret_cast<char*>(image + 0x400), 32, "KERNEL32.dll");
        strcpy_s(reinterpret_cast<char*>(image + 0x420), 32, "WINMM.dll");
        strcpy_s(reinterpret_cast<char*>(image + 0x440), 32, "USER32.dll");
        kernelIat = reinterpret_cast<IMAGE_THUNK_DATA32*>(image + 0x500);
        mmIat = reinterpret_cast<IMAGE_THUNK_DATA32*>(image + 0x540);
        userIat = reinterpret_cast<IMAGE_THUNK_DATA32*>(image + 0x580);
        NameThunk(0x600, 0, 0x700, "GetTickCount", reinterpret_cast<void*>(FakeTick), kernelIat);
        NameThunk(0x600, 1, 0x740, "QueryPerformanceCounter", reinterpret_cast<void*>(FakeCounter), kernelIat);
        NameThunk(0x600, 2, 0x780, "Sleep", reinterpret_cast<void*>(Sleep), kernelIat);
        NameThunk(0x640, 0, 0x7c0, "timeGetTime", reinterpret_cast<void*>(FakeTime), mmIat);
        NameThunk(0x640, 1, 0x800, "timeSetEvent", reinterpret_cast<void*>(FakeEvent), mmIat);
        NameThunk(0x680, 0, 0x840, "SetTimer", reinterpret_cast<void*>(FakeTimer), userIat);
        NameThunk(0x680, 1, 0x880, "GetMessageTime", reinterpret_cast<void*>(FakeMessage), userIat);
    }
    void NameThunk(DWORD table, int index, DWORD name, const char* text, void* function, IMAGE_THUNK_DATA32* iat) {
        reinterpret_cast<IMAGE_THUNK_DATA32*>(image + table)[index].u1.AddressOfData = name;
        strcpy_s(reinterpret_cast<char*>(image + name + sizeof(WORD)), 60, text);
        iat[index].u1.Function = reinterpret_cast<DWORD>(function);
    }
    HMODULE Module() { return reinterpret_cast<HMODULE>(image); }
    ~MockImage() { RestoreHooks(); VirtualFree(image, 0, MEM_RELEASE); }
};

DWORD WINAPI ConcurrentReader(void*) {
    LARGE_INTEGER previous;
    AcceleratedCounter(&previous);
    DWORD previousTick = AcceleratedTick();
    for (int i = 0; i < 3000; ++i) {
        LARGE_INTEGER next;
        AcceleratedCounter(&next);
        DWORD nextTick = AcceleratedTick();
        AcceleratedTime();
        if (next.QuadPart < previous.QuadPart || static_cast<LONG>(nextTick - previousTick) < 0)
            InterlockedIncrement(&concurrentFailures);
        previous = next;
        previousTick = nextTick;
        if (!(i % 50)) Sleep(1);
    }
    return 0;
}

void NativeClockTests() {
    Check(EnsureInitialized(), "real Windows APIs initialize outside loader lock");
    SetSpeed(20);
    DWORD realStart = realTick();
    DWORD virtualStart = AcceleratedTick();
    LARGE_INTEGER counterStart;
    AcceleratedCounter(&counterStart);
    LARGE_INTEGER realCounterStart;
    realCounter(&realCounterStart);
    Sleep(80);
    DWORD elapsedReal = realTick() - realStart;
    DWORD elapsedVirtual = AcceleratedTick() - virtualStart;
    LARGE_INTEGER counterEnd;
    AcceleratedCounter(&counterEnd);
    LARGE_INTEGER realCounterEnd;
    realCounter(&realCounterEnd);
    double ratio = static_cast<double>(counterEnd.QuadPart - counterStart.QuadPart)
        / (realCounterEnd.QuadPart - realCounterStart.QuadPart);
    double preciseElapsed = static_cast<double>(realCounterEnd.QuadPart - realCounterStart.QuadPart)
        * counterMilliseconds;
    Check(elapsedReal >= 70 && elapsedReal < 2000, "Sleep remains real-time at 4x");
    Check(fabs(elapsedVirtual - preciseElapsed * 4) <= 8,
        "accelerated GetTickCount runs at QPC-derived 4x");
    Check(ratio > 3.8 && ratio < 4.2, "native QPC runs at 4x");
    SetSpeed(0);
    HANDLE readers[2] = {CreateThread(NULL, 0, ConcurrentReader, NULL, 0, NULL),
                         CreateThread(NULL, 0, ConcurrentReader, NULL, 0, NULL)};
    Check(readers[0] && readers[1], "concurrent reader threads created");
    for (int i = 0; i < 100; ++i) { SetSpeed(i % 2 ? 20 : -10); Sleep(1); }
    if (readers[0] && readers[1]) WaitForMultipleObjects(2, readers, TRUE, INFINITE);
    for (int i = 0; i < 2; ++i) if (readers[i]) CloseHandle(readers[i]);
    Check(concurrentFailures == 0, "concurrent rate changes preserve monotonic clocks");
    SetSpeed(0);
}

void CurveAndClockTests() {
    Check(SpeedMultiplier(0) == 1.0, "0 is 1x");
    Check(SpeedMultiplier(10) == 2.0, "10 is 2x");
    Check(SpeedMultiplier(20) == 4.0, "20 is 4x");
    Check(SpeedMultiplier(-10) == 0.5, "-10 is half speed");
    Check(fabs(SpeedMultiplier(1) - 1.0717734625362931) < 1e-12, "non-dial input uses original curve");
    ResetClocks();
    SetSpeed(10);
    fakeTickValue += 100; fakeTimeValue += 100; fakeCounterValue += 1000;
    Check(AcceleratedTick() == 1200, "tick at 2x");
    Check(AcceleratedTime() == 2200, "separate multimedia epoch");
    LARGE_INTEGER counter;
    Check(AcceleratedCounter(&counter) && counter.QuadPart == 102000, "QPC at 2x");
    SetSpeed(-10);
    Check(AcceleratedTick() == 1200 && AcceleratedTime() == 2200, "no clock jump at speed switch");
    Check(AcceleratedCounter(&counter) && counter.QuadPart == 102000, "QPC continuous at speed switch");
    fakeTickValue += 100; fakeTimeValue += 100; fakeCounterValue += 1000;
    Check(AcceleratedTick() == 1250 && AcceleratedTime() == 2250, "negative speed slows both clocks");
    Check(AcceleratedCounter(&counter) && counter.QuadPart == 102500, "negative speed slows QPC");
    fakeMessageValue = static_cast<LONG>(fakeTickValue - 20);
    Check(AcceleratedMessageTime() == 1240, "message timestamp shares tick epoch");
    SetSpeed(128);
    DWORD before = AcceleratedTick();
    fakeTickValue += 10; fakeCounterValue += 100;
    DWORD delta = AcceleratedTick() - before;
    Check(delta >= 71315 && delta <= 71316, "high speed avoids fixedpoint overflow");
    SetSpeed(100000);
    Check(GetSpeed() == 128, "upper speed clamp");
    SetSpeed(-100000);
    Check(GetSpeed() == -128 && isfinite(CurrentMultiplier()), "lower speed clamp");
    ResetClocks(0xfffffff0U, 0xffffffe0U);
    SetSpeed(10);
    fakeTickValue = 0x10; fakeTimeValue = 0; fakeCounterValue += 320;
    Check(AcceleratedTick() == 0x30 && AcceleratedTime() == 0x20, "independent DWORD clocks wrap correctly");
    ResetClocks();
    SetSpeed(-10);
    fakeTickValue += 1; fakeCounterValue += 10;
    Check(AcceleratedTick() == 1000, "fractional millisecond retained");
    fakeTickValue += 1; fakeCounterValue += 10;
    Check(AcceleratedTick() == 1001, "fractional millisecond accumulates");
}

void GranularityAndTransitionTests() {
    ResetClocks();
    fakeCounterValue += 50;
    Check(AcceleratedTick() == 1000 && AcceleratedTime() == 2000,
        "initial 1x retains native millisecond granularity");
    SetSpeed(20);
    for (int i = 1; i <= 15; ++i) {
        fakeCounterValue += 10;
        if (i == 15) { fakeTickValue += 15; fakeTimeValue += 15; }
        Check(AcceleratedTick() == 1000 + static_cast<DWORD>(i * 4),
            "4x progresses every millisecond despite a 15 ms native tick");
        Check(AcceleratedTime() == 2000 + static_cast<DWORD>(i * 4),
            "multimedia clock shares the QPC delta and retains its origin");
    }
    SetSpeed(-10);
    fakeCounterValue += 40;
    Check(AcceleratedTime() == 2062 && AcceleratedTick() == 1062,
        "API read order cannot duplicate the shared high-resolution delta");
    LARGE_INTEGER counter;
    Check(AcceleratedCounter(&counter) && counter.QuadPart == 100670,
        "QPC and both millisecond clocks retain the same elapsed time");
    SetSpeed(0);
    fakeCounterValue += 20;
    Check(AcceleratedTick() == 1062 && AcceleratedTime() == 2062,
        "returning to 1x retains the continuous value and native tick plateau");
    fakeTickValue += 15; fakeTimeValue += 15;
    Check(AcceleratedTick() == 1077 && AcceleratedTime() == 2077,
        "disabled gear uses unscaled native tick increments");
    SetSpeed(20);
    DWORD before = AcceleratedTick();
    realCounter = FailedCounter;
    fakeCounterValue += 100;
    Check(AcceleratedTick() == before && !AcceleratedCounter(&counter),
        "failed QPC read neither invents elapsed time nor jumps backwards");
    realCounter = FakeCounter;
    Check(AcceleratedTick() == before + 40,
        "recovered QPC accounts for elapsed time exactly once");
}

void TimerTests() {
    ResetClocks();
    SetSpeed(20);
    Check(AcceleratedTimer(NULL, 42, 100, NULL) == 42 && capturedInterval == 25, "SetTimer divides delay by speed");
    Check(OldSetTimer(NULL, 42, 100, NULL) == 42 && capturedInterval == 100, "OldSetTimer bypasses acceleration");
    Check(AcceleratedEvent(100, 7, NULL, 0, TIME_PERIODIC) == 456 && capturedInterval == 25
        && capturedResolution == 7 && capturedFlags == TIME_PERIODIC, "timeSetEvent preserves arguments");
    Check(ScaleInterval(0, SpeedMultiplier(128)) == 1, "zero or tiny interval bounded at 1");
    Check(ScaleInterval(0xffffffffU, SpeedMultiplier(-128)) == 0x7fffffffU, "large slow interval bounded");
}

void ImportTests() {
    ResetClocks();
    {
        MockImage fixture;
        Check(HookImage(fixture.Module()), "short import-directory size supports all descriptors");
        Check(hookCount == 6, "all six timing API slots intercepted");
        Check(fixture.kernelIat[2].u1.Function == reinterpret_cast<DWORD>(Sleep), "Sleep left unscaled");
        SetSpeed(10); fakeTickValue += 100; fakeCounterValue += 1000;
        Check(reinterpret_cast<TickFn>(fixture.kernelIat[0].u1.Function)() == 1200, "actual mocked IAT calls accelerated tick");
        Check(HookImage(fixture.Module()) && hookCount == 6, "repeated interception idempotent");
        fixture.userIat[0].u1.Function = reinterpret_cast<DWORD>(FakeTick);
        Check(HookImage(fixture.Module()) && hookCount == 6,
            "refresh preserves competing replacement without duplicate records");
        RestoreHooks();
        Check(fixture.kernelIat[0].u1.Function == reinterpret_cast<DWORD>(FakeTick), "original slot restored");
        Check(fixture.userIat[0].u1.Function == reinterpret_cast<DWORD>(FakeTick), "unhook preserves competing replacement");
    }
    {
        MockImage fixture;
        fixture.imports[1].Name = 0x2000;
        Check(!HookImage(fixture.Module()) && hookCount == 0, "malformed descriptor rejects entire hook batch");
        Check(fixture.kernelIat[0].u1.Function == reinterpret_cast<DWORD>(FakeTick), "malformed PE makes no partial change");
    }
    {
        MockImage fixture;
        fixture.imports[1].OriginalFirstThunk = 0x1ffc;
        reinterpret_cast<DWORD*>(fixture.image + 0x1ffc)[0] = 0x800;
        Check(!HookImage(fixture.Module()) && hookCount == 0, "unterminated lookup thunk rejected");
    }
    {
        MockImage fixture;
        fixture.imports[0].OriginalFirstThunk = 0;
        Check(HookImage(fixture.Module()) && hookCount == 6, "missing lookup table matches resolved API addresses");
    }
    {
        MockImage fixture;
        fixture.imports[0].OriginalFirstThunk = fixture.imports[0].FirstThunk;
        fixture.imports[1].OriginalFirstThunk = fixture.imports[1].FirstThunk;
        fixture.imports[2].OriginalFirstThunk = fixture.imports[2].FirstThunk;
        Check(HookImage(fixture.Module()) && hookCount == 6,
            "6.6 shared lookup/IAT tables match resolved addresses");
    }
    {
        MockImage fixture;
        fixture.nt->OptionalHeader.SizeOfImage = 0x230;
        Check(!HookImage(fixture.Module()) && hookCount == 0, "bounded unterminated import array rejected");
    }
    {
        MockImage fixture;
        reinterpret_cast<IMAGE_THUNK_DATA32*>(fixture.image + 0x600)[0].u1.Ordinal = IMAGE_ORDINAL_FLAG32 | 1;
        Check(HookImage(fixture.Module()) && hookCount == 5, "ordinal imports skipped safely");
    }
    Check(AddProcess(GetCurrentProcess()) != FALSE && hookCount > 0,
        "renamed GearTests.exe host installs timing hooks without a basename restriction");
    SetSpeed(10);
    LONGLONG counterBefore = CounterValue();
    fakeCounterValue += 100;
    LARGE_INTEGER hostCounter;
    QueryPerformanceCounter(&hostCounter);
    Check(hostCounter.QuadPart == counterBefore + 200,
        "renamed host's actual QPC import uses the accelerated clock");
    UnIntercept();
    HANDLE foreign = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 4);
    Check(!AddProcess(foreign), "exports refuse invalid or foreign process handle");
    if (foreign) CloseHandle(foreign);
}

void CacheTests() {
    ResetClocks();
    {
        MockImage fixture;
        fixture.imports[0].Name = 0x1000;
        fixture.imports[1].Name = 0x1020;
        fixture.imports[2].Name = 0x1040;
        strcpy_s(reinterpret_cast<char*>(fixture.image + 0x1000), 32, "KERNEL32.dll");
        strcpy_s(reinterpret_cast<char*>(fixture.image + 0x1020), 32, "WINMM.dll");
        strcpy_s(reinterpret_cast<char*>(fixture.image + 0x1040), 32, "USER32.dll");
        Check(HookKnownImagesFor(fixture.Module(), NULL), "initial cache installation");
        DWORD oldProtection;
        Check(VirtualProtect(fixture.image + 0x1000, 0x1000, PAGE_NOACCESS, &oldProtection) != FALSE,
            "import metadata page hidden after installation");
        Check(HookKnownImagesFor(fixture.Module(), NULL),
            "cached modules work without rescanning import metadata");
        fakeTickValue += 10;
        Check(reinterpret_cast<TickFn>(fixture.kernelIat[0].u1.Function)() == 1010,
            "cached timing hook remains callable");
        DWORD ignored;
        VirtualProtect(fixture.image + 0x1000, 0x1000, oldProtection, &ignored);
        UnIntercept();
        Check(fixture.kernelIat[0].u1.Function == reinterpret_cast<DWORD>(FakeTick), "unintercept restores cached hooks");
        fixture.imports[0].Name = 0x2000;
        Check(!HookKnownImagesFor(fixture.Module(), NULL), "unintercept clears cache and revalidates later calls");
    }
    {
        MockImage main;
        MockImage extension;
        extension.imports[1].Name = 0x2000;
        Check(!HookKnownImagesFor(main.Module(), extension.Module()) && hookCount == 0,
            "two-module installation failure rolls back main hooks");
        Check(main.kernelIat[0].u1.Function == reinterpret_cast<DWORD>(FakeTick),
            "partial installation leaves no changed main IAT");
        extension.imports[1].Name = 0x420;
        Check(HookKnownImagesFor(main.Module(), extension.Module()) && hookCount == 12,
            "failed install is retried after correction");
        for (size_t i = 0; i < hookCount; ++i) {
            if (hooks[i].image == extension.Module()) *hooks[i].slot = hooks[i].original;
        }
        Check(HookKnownImagesFor(main.Module(), extension.Module()) && hookCount == 12,
            "same-address reload with identical headers reinstalls missing hooks");
        Check(extension.kernelIat[0].u1.Function == reinterpret_cast<DWORD>(AcceleratedTick),
            "same-address reload has live timing interception");
        ++extension.nt->FileHeader.TimeDateStamp;
        Check(HookKnownImagesFor(main.Module(), extension.Module()) && hookCount == 12,
            "changed PE identity forces complete validation and hook refresh");
        extension.userIat[0].u1.Function = reinterpret_cast<DWORD>(FakeTick);
        Check(HookKnownImagesFor(main.Module(), extension.Module()) && hookCount == 12,
            "cached refresh preserves competing hook and avoids duplicate records");
        Check(extension.userIat[0].u1.Function == reinterpret_cast<DWORD>(FakeTick),
            "competing hook remains installed");
        Check(HookKnownImagesFor(main.Module(), NULL) && hookCount == 6,
            "extension disappearance removes obsolete hooks");
        Check(extension.kernelIat[0].u1.Function == reinterpret_cast<DWORD>(FakeTick),
            "remaining mapped obsolete extension is restored");
    }
    {
        MockImage main;
        MockImage oldExtension;
        MockImage newExtension;
        Check(HookKnownImagesFor(main.Module(), oldExtension.Module()), "initial extension cached");
        Check(HookKnownImagesFor(main.Module(), newExtension.Module()) && hookCount == 12,
            "new extension address installed without growing stale records");
        Check(oldExtension.kernelIat[0].u1.Function == reinterpret_cast<DWORD>(FakeTick)
            && newExtension.kernelIat[0].u1.Function == reinterpret_cast<DWORD>(AcceleratedTick),
            "module replacement restores old and intercepts new");
    }
}
} // namespace

int main() {
    NativeClockTests(); CurveAndClockTests(); GranularityAndTransitionTests();
    TimerTests(); ImportTests(); CacheTests();
    printf("GearTests: %s (%d failures)\n", failures ? "FAILED" : "PASS", failures);
    return failures ? 1 : 0;
}
