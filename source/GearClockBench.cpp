#define GEAR_UNIT_TEST
#include "GearNtKe.cpp"
#include <objidl.h>
#include <gdiplus.h>
#include <stdio.h>
#include <algorithm>

namespace {
struct LegacyClock {
    DWORD previous;
    double value;
    void Reset() { previous = realTick(); value = previous; }
    DWORD Read() {
        AcquireSRWLockExclusive(&clockLock);
        DWORD now = realTick();
        value += static_cast<DWORD>(now - previous) * 4.0;
        previous = now;
        value = fmod(value, 4294967296.0);
        DWORD result = static_cast<DWORD>(fmod(value, 4294967296.0));
        ReleaseSRWLockExclusive(&clockLock);
        return result;
    }
};

LONGLONG Now() { LARGE_INTEGER value; realCounter(&value); return value.QuadPart; }

void CallCost() {
    LegacyClock legacy;
    legacy.Reset();
    const int calls = 200000;
    volatile DWORD sink = 0;
    LONGLONG start = Now();
    for (int i = 0; i < calls; ++i) sink = legacy.Read();
    double legacyMs = (Now() - start) * counterMilliseconds;
    start = Now();
    for (int i = 0; i < calls; ++i) sink = AcceleratedTick();
    double preciseMs = (Now() - start) * counterMilliseconds;
    printf("clock calls=%d legacy_us=%.4f qpc_us=%.4f sink=%lu\n", calls,
        legacyMs * 1000.0 / calls, preciseMs * 1000.0 / calls, sink);
}

void RenderLoad() {
    Gdiplus::Bitmap frame(640, 480, PixelFormat32bppPARGB);
    Gdiplus::Bitmap sprite(48, 64, PixelFormat32bppPARGB);
    Gdiplus::Graphics source(&sprite);
    source.Clear(Gdiplus::Color(255, 22, 70, 48));
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 175, 200, 45));
    source.FillRectangle(&brush, 8, 8, 32, 48);
    Gdiplus::Graphics graphics(&frame);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    LegacyClock legacy;
    legacy.Reset();
    DWORD oldPrevious = legacy.Read();
    DWORD newPrevious = AcceleratedTick();
    unsigned oldChanges = 0, newChanges = 0, frames = 0, shortFrames = 0;
    DWORD oldMax = 0, newMax = 0, oldShortMax = 0, newShortMax = 0;
    LONGLONG start = Now(), previousCounterSample = start;
    while ((Now() - start) * counterMilliseconds < 1000.0) {
        graphics.Clear(Gdiplus::Color(255, 42, 48, 30));
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 8; ++x)
                graphics.DrawImage(&sprite, x * 72 + static_cast<int>(frames % 9), y * 88);
        }
        graphics.Flush(Gdiplus::FlushIntentionSync);
        DWORD oldValue = legacy.Read();
        DWORD newValue = AcceleratedTick();
        LONGLONG counterSample = Now();
        DWORD oldDelta = oldValue - oldPrevious, newDelta = newValue - newPrevious;
        oldChanges += oldDelta != 0;
        newChanges += newDelta != 0;
        oldMax = (std::max)(oldMax, oldDelta);
        newMax = (std::max)(newMax, newDelta);
        if ((counterSample - previousCounterSample) * counterMilliseconds <= 2.0) {
            ++shortFrames;
            oldShortMax = (std::max)(oldShortMax, oldDelta);
            newShortMax = (std::max)(newShortMax, newDelta);
        }
        oldPrevious = oldValue; newPrevious = newValue;
        previousCounterSample = counterSample;
        ++frames;
    }
    printf("GDI+ render loop 1s: frames=%u legacy_updates=%u qpc_updates=%u "
        "legacy_max_jump_ms=%lu qpc_max_jump_ms=%lu short_frames=%u "
        "legacy_short_max_jump_ms=%lu qpc_short_max_jump_ms=%lu\n", frames,
        oldChanges, newChanges, oldMax, newMax, shortFrames, oldShortMax, newShortMax);
}
}

int main() {
    if (!EnsureInitialized()) return 1;
    SetSpeed(20);
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, NULL) != Gdiplus::Ok) return 2;
    CallCost();
    RenderLoad();
    Gdiplus::GdiplusShutdown(token);
    SetSpeed(0);
    return 0;
}
