#include "Patch.h"
#include <stdio.h>

int EngineTestSuperHelper();
int EngineTestSuperCaller(uintptr_t caller);
void EngineTestSuperWhitelist(uintptr_t caller);
void EngineTestSuperCallbacks(ULONGLONG (WINAPI* clock)(), void (WINAPI* sleep)(DWORD),
    bool (__cdecl* pump)());

HMODULE gInstance = nullptr;
uintptr_t gGameBase = 0;

namespace {
int failures = 0;
ULONGLONG testClock = 10000;
int sleepCalls = 0;
int pumpCalls = 0;
bool recursiveResult = false;
ULONGLONG WINAPI TestClock() { return testClock; }
void WINAPI TestSleep(DWORD value) { if (value == 0) ++sleepCalls; }
bool __cdecl TestPump() {
    ++pumpCalls;
    recursiveResult = EngineTestSuperCaller(0x12345678) == 1;
    return true;
}
void Check(bool condition, const char* name) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

int NativeStageReference(WORD speed, BYTE phase, const BYTE table[3], BYTE& counter,
    int& sleepValue, bool& deadlineCalled) {
    const BYTE mode = static_cast<BYTE>(speed & 0xFF);
    int delay = 0;
    int selected = mode ? ((speed >> 8) ? (speed >> 8) + 1 : 1) : 0;
    if (selected == 1) {
        const BYTE value = table[phase < 3 ? phase : 0];
        if (value > 50) delay = value - 50;
        else {
            ++counter;
            delay = counter < value ? 1 : 0;
            if (counter >= 50) counter = static_cast<BYTE>(counter - 50);
        }
    } else if (selected >= 2 && selected <= 4) {
        delay = selected * 4 + 1;
    }
    sleepValue = delay;
    deadlineCalled = selected < 2;
    return 1;
}

int LegacyStageReference(WORD speed, BYTE phase, const BYTE table[3], BYTE& counter,
    int& sleepValue, bool& deadlineCalled) {
    const BYTE value = table[phase < 3 ? phase : 0];
    if (value > 50) sleepValue = value - 50;
    else {
        ++counter;
        sleepValue = counter < value ? 1 : 0;
        if (counter >= 50) counter = static_cast<BYTE>(counter - 50);
    }
    deadlineCalled = (speed & 0xFF) == 0;
    return 1;
}
}

int main() {
    bool pacing = true;
    const int values[] = {0, 1, 2, 25, 49, 50};
    for (int value : values) {
        int counter = 0;
        int total = 0;
        for (int i = 0; i < 50; ++i) total += StageDelay(value, counter);
        const int wanted = value ? value - 1 : 0;
        pacing = pacing && total == wanted && counter == 0;
    }
    Check(pacing, "native shared-counter pacing matches 6.6 for values 0..50");

    int counter = 49;
    Check(StageDelay(50, counter) == 0 && counter == 0 &&
        StageDelay(50, counter) == 1 && counter == 1,
        "value 50 produces one Sleep(0) at the 50-call rollover");

    const BYTE table[3] = {60, 60, 60};
    BYTE nativeCounter = 0;
    int sleepValue = -1;
    bool deadline = false;
    NativeStageReference(0x0001, 0, table, nativeCounter, sleepValue, deadline);
    Check(sleepValue == 10 && deadline, "0x0001 reads the phase table and keeps deadline polling");
    NativeStageReference(0x0000, 0, table, nativeCounter, sleepValue, deadline);
    Check(sleepValue == 0 && deadline, "0x0000 keeps native deadline polling with Sleep(0)");
    NativeStageReference(0x0101, 0, table, nativeCounter, sleepValue, deadline);
    Check(sleepValue == 9 && !deadline, "0x0101 uses the native fixed-rate branch");
    NativeStageReference(0x0201, 0, table, nativeCounter, sleepValue, deadline);
    Check(sleepValue == 13 && !deadline, "0x0201 uses the native fixed-rate branch");
    NativeStageReference(0x0301, 0, table, nativeCounter, sleepValue, deadline);
    Check(sleepValue == 17 && !deadline, "0x0301 uses the native fixed-rate branch");

    LegacyStageReference(0x0000, 0, table, nativeCounter, sleepValue, deadline);
    Check(sleepValue == 10 && deadline, "legacy mode 0 reads the phase table and polls the deadline");
    LegacyStageReference(0x0001, 0, table, nativeCounter, sleepValue, deadline);
    Check(sleepValue == 10 && !deadline, "legacy mode 1 reads the phase table");
    LegacyStageReference(0x0101, 0, table, nativeCounter, sleepValue, deadline);
    Check(sleepValue == 10 && !deadline, "legacy numeric speed 2 keeps the phase table");
    LegacyStageReference(0x0201, 0, table, nativeCounter, sleepValue, deadline);
    Check(sleepValue == 10 && !deadline, "legacy numeric speed 3 keeps the phase table");
    LegacyStageReference(0x0301, 0, table, nativeCounter, sleepValue, deadline);
    Check(sleepValue == 10 && !deadline, "legacy numeric speed 4 keeps the phase table");
    BYTE legacyCounter = 0;
    const BYTE legacyFast[3] = {70, 60, 60};
    LegacyStageReference(0x0101, 0, legacyFast, legacyCounter, sleepValue, deadline);
    Check(sleepValue == 20, "legacy phase value 70 produces a 20 ms wait");

    BYTE independent[3] = {60, 61, 62};
    nativeCounter = 0;
    NativeStageReference(0x0001, 2, independent, nativeCounter, sleepValue, deadline);
    Check(sleepValue == 12, "phase 2 uses its independent table byte");
    int counterForZero = 0;
    bool zeroAlways = true;
    for (int i = 0; i < 200; ++i) zeroAlways = zeroAlways && StageDelay(0, counterForZero) == 0;
    Check(zeroAlways, "phase value zero never sleeps for one millisecond");
    Check(EngineTestSuperHelper() == 0,
        "unknown super-wait context stays on the native message-pump path");
    EngineTestSuperWhitelist(0x12345678);
    EngineTestSuperCallbacks(TestClock, TestSleep, TestPump);
    Check(EngineTestSuperCaller(0x12345678) == 1 && sleepCalls == 1 && pumpCalls == 1 && recursiveResult,
        "known S presentation wait yields once and recursion completes without looping");
    Check(EngineTestSuperCaller(0x12345678) == 1 && sleepCalls == 1 && pumpCalls == 1,
        "four-millisecond throttle suppresses duplicate scheduling work");
    testClock += 4000;
    Check(EngineTestSuperCaller(0x12345678) == 1 && sleepCalls == 2 && pumpCalls == 2,
        "known long-running presentation receives another bounded yield after four milliseconds");
    Check(EngineTestSuperCaller(0x87654321) == 0,
        "unknown caller remains on the native wait path even while super S is enabled");

    printf("Engine verification: %d failures\n", failures);
    return failures ? 1 : 0;
}
