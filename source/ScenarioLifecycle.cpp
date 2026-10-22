#include "ScenarioLifecycle.h"

#include <cstring>
#include <vector>

// BattleStats owns both ledgers. Resource opens are deliberately excluded:
// only a successful native scenario installation can change their lifetime.
void NotifyBattleStatsScenarioInstalled(int ordinal, bool newCampaign,
    bool forceNewInstance, ULONGLONG now);

extern "C" uintptr_t gScenarioInstalledContinuation = 0;

namespace {
struct ImageView {
    BYTE* bytes = nullptr;
    uintptr_t virtualBase = 0;
    uintptr_t preferredBase = 0;
    size_t size = 0;
    BYTE* text = nullptr;
    size_t textSize = 0;

    BYTE* Address(uintptr_t address, size_t length) const {
        for (uintptr_t base : {virtualBase, preferredBase}) {
            if (address >= base && address - base <= size && length <= size - (address - base))
                return bytes + (address - base);
        }
        return nullptr;
    }
    bool InText(const BYTE* p, size_t length) const {
        return p && p >= text && static_cast<size_t>(p - text) <= textSize &&
            length <= textSize - static_cast<size_t>(p - text);
    }
    uintptr_t Logical(const BYTE* p) const { return virtualBase + (p - bytes); }
};

bool OpenImage(BYTE* bytes, uintptr_t virtualBase, ImageView& image) {
    if (!bytes) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
        return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.SizeOfImage < 4096 || nt->OptionalHeader.SizeOfImage > 128 * 1024 * 1024)
        return false;
    image.bytes = bytes;
    image.virtualBase = virtualBase;
    image.preferredBase = nt->OptionalHeader.ImageBase;
    image.size = nt->OptionalHeader.SizeOfImage;
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        if (!(sections[index].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (image.text) continue;
        if (sections[index].VirtualAddress >= image.size ||
            sections[index].Misc.VirtualSize > image.size - sections[index].VirtualAddress) return false;
        image.text = bytes + sections[index].VirtualAddress;
        image.textSize = sections[index].Misc.VirtualSize;
    }
    return image.text && image.textSize >= 0x200;
}

uint32_t Dword(const BYTE* p) { uint32_t value = 0; memcpy(&value, p, 4); return value; }

BYTE* CallTarget(const ImageView& image, const BYTE* call) {
    if (!image.InText(call, 5) || call[0] != 0xE8) return nullptr;
    int32_t displacement = 0;
    memcpy(&displacement, call + 1, 4);
    const int64_t offset = static_cast<int64_t>(call - image.bytes) + 5 + displacement;
    if (offset < 0 || static_cast<uint64_t>(offset) >= image.size) return nullptr;
    BYTE* target = image.bytes + offset;
    return image.InText(target, 1) ? target : nullptr;
}

bool Resolve(BYTE* bytes, uintptr_t virtualBase, ScenarioLifecycleImageLayout& output) {
    output = ScenarioLifecycleImageLayout();
    ImageView image;
    if (!OpenImage(bytes, virtualBase, image)) return false;
    BYTE* install = nullptr;
    BYTE* accessor = nullptr;
    BYTE* ordinal = nullptr;
    BYTE* resultSite = nullptr;
    size_t installMatches = 0;
    const BYTE prefix[] = {0x55,0x8B,0xEC,0x6A,0xFF,0x68};
    const BYTE frame[] = {0x64,0xFF,0x35,0,0,0,0,0x64,0x89,0x25,0,0,0,0,
        0x6A,0,0x6A,0,0x83,0xEC,0x20,0xFF,0x45,0xFC,0xB9};
    const BYTE selection[] = {0xA8,1,0x74,3,0x83,0xC1,0x0C,0x6A,2,0xD1,0xE8,
        0x50,0x51,0x8D,0x4D,0xD8,0x51,0xE8};
    const BYTE accessorBody[] = {0x55,0x8B,0xEC,0x0F,0xB7,0x41,6,0x5D,0xC3};
    const BYTE success[] = {0xFF,0x45,0xEC,0xEB,0x2B};
    const BYTE result[] = {0x8B,0x45,0xEC,0x8B,0x4D,0xF4,0x64,0x89,0x0D,0,0,0,0,
        0x8B,0xE5,0x5D,0xC3};
    for (size_t offset = 0; offset + 0x105 <= image.textSize; ++offset) {
        BYTE* p = image.text + offset;
        if (memcmp(p, prefix, sizeof(prefix)) || memcmp(p + 10, frame, sizeof(frame)) ||
            p[39] != 0xE8 || p[44] != 0xB9 || memcmp(p + 49, selection, sizeof(selection)) ||
            memcmp(p + 0xB9, success, sizeof(success)) || memcmp(p + 0xF4, result, sizeof(result))) continue;
        BYTE* a = CallTarget(image, p + 39);
        BYTE* owner = image.Address(Dword(p + 35), 8);
        if (!image.InText(a, sizeof(accessorBody)) || memcmp(a, accessorBody, sizeof(accessorBody)) || !owner)
            continue;
        install = p;
        accessor = a;
        ordinal = owner + 6;
        resultSite = p + 0xF4;
        ++installMatches;
    }
    if (installMatches != 1) return false;

    BYTE* progress = nullptr;
    BYTE* initial = nullptr;
    BYTE* reload = nullptr;
    BYTE* menu = nullptr;
    size_t calls = 0;
    const BYTE progressTail[] = {0x66,0x83,0x41,6,1,0x8B,0x4D,8};
    const BYTE menuChoice[] = {0x8B,0x45,0xFC,0x85,0xC0,0x74,0x27,0x3C,1,0x75,0x15,0x6A,0,0xE8};
    for (size_t offset = 16; offset + 10 <= image.textSize; ++offset) {
        BYTE* call = image.text + offset;
        if (CallTarget(image, call) != install) continue;
        ++calls;
        if (call[-13] == 0xB9 && !memcmp(call - 8, progressTail, sizeof(progressTail)) &&
            image.Address(Dword(call - 12), 8) == ordinal - 6) {
            if (progress) return false;
            progress = call;
            continue;
        }
        BYTE* candidateMenu = nullptr;
        for (size_t back = 5; back <= 96 && back <= offset; ++back) {
            BYTE* candidateCall = call - back;
            BYTE* candidate = CallTarget(image, candidateCall);
            if (!image.InText(candidate, 0x200) || candidate[0] != 0x55 ||
                candidate[1] != 0x8B || candidate[2] != 0xEC) continue;
            BYTE* after = candidateCall + 5;
            while (after < call && *after == 0x90) ++after;
            if (after + 3 > call || memcmp(after, "\x89\x45\xFC", 3)) continue;
            bool choiceVerified = false;
            for (size_t at = 0; at + sizeof(menuChoice) <= 0x200; ++at)
                if (!memcmp(candidate + at, menuChoice, sizeof(menuChoice))) choiceVerified = true;
            if (!choiceVerified) continue;
            bool checksCancel = false;
            for (BYTE* p = after + 3; p + 5 <= call; ++p)
                if (!memcmp(p, "\x3D\xFF\x00\x00\x00", 5)) checksCancel = true;
            if (!checksCancel || candidateMenu) return false;
            candidateMenu = candidate;
        }
        if (candidateMenu) {
            if (initial || call[5] != 0x85 || call[6] != 0xC0) return false;
            initial = call;
            menu = candidateMenu;
        } else {
            if (reload || call[5] != 0x85 || call[6] != 0xC0) return false;
            reload = call;
        }
    }
    if (calls != 3 || !progress || !initial || !reload || !menu) return false;
    output.nativeInstall = image.Logical(install);
    output.ordinalAccessor = image.Logical(accessor);
    output.ordinalAddress = image.Logical(ordinal);
    output.installedSite = image.Logical(resultSite);
    output.installedContinuation = output.installedSite + 6;
    output.progressCall = image.Logical(progress);
    output.initialCall = image.Logical(initial);
    output.reloadCall = image.Logical(reload);
    output.menuFunction = image.Logical(menu);
    return true;
}

SRWLOCK gLock = SRWLOCK_INIT;
ScenarioLifecycleImageLayout gLayout;
BYTE gOriginal[6] = {};
BYTE gPatched[6] = {};
volatile LONG gReady = FALSE;
unsigned gLoadDepth = 0;
int gSuppressedOrdinal = -1;
ULONGLONG gSuccessfulLoadGeneration = 0;
struct LoadFrame {
    int previousSuppressedOrdinal;
    ULONGLONG successfulGenerationAtBegin;
};
std::vector<LoadFrame> gLoadFrames;

int ReadOrdinalUnlocked() {
    WORD ordinal = 0;
    if (!gLayout.ordinalAddress) return -1;
    __try { memcpy(&ordinal, reinterpret_cast<void*>(gLayout.ordinalAddress), sizeof(ordinal)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return ordinal;
}

void CompleteInstall(bool succeeded, uintptr_t returnAddress, uintptr_t callerFrame, ULONGLONG now) {
    if (!succeeded || !InterlockedCompareExchange(&gReady, FALSE, FALSE)) return;
    bool notify = false, newCampaign = false, forceNew = false;
    AcquireSRWLockExclusive(&gLock);
    const int ordinal = ReadOrdinalUnlocked();
    if (ordinal >= 0 && !gLoadDepth) {
        int reason = -1;
        if (returnAddress == gLayout.initialCall + 5 && callerFrame) {
            __try { memcpy(&reason, reinterpret_cast<void*>(callerFrame - 4), sizeof(reason)); }
            __except (EXCEPTION_EXECUTE_HANDLER) { reason = -1; }
        }
        newCampaign = reason == 0;
        const bool progressing = returnAddress == gLayout.progressCall + 5;
        if (newCampaign || progressing) {
            gSuppressedOrdinal = -1;
            notify = true;
            forceNew = progressing;
        } else if (gSuppressedOrdinal == ordinal) {
            // A completed native load may install its script in the caller
            // after returning from the save loader. Consume exactly that
            // installation, without interpreting it as a new battle.
            gSuppressedOrdinal = -1;
        } else {
            gSuppressedOrdinal = -1;
            notify = true;
            forceNew = returnAddress == gLayout.reloadCall + 5;
        }
    }
    ReleaseSRWLockExclusive(&gLock);
    if (notify) NotifyBattleStatsScenarioInstalled(ordinal, newCampaign, forceNew, now);
}

void __cdecl CompleteInstallBridge(int succeeded, uintptr_t returnAddress, uintptr_t callerFrame) {
    try { CompleteInstall(succeeded != 0, returnAddress, callerFrame, GetTickCount64()); }
    catch (...) { /* The native result and stack must survive allocation errors. */ }
}

__declspec(naked) void InstalledBridge() {
    __asm {
        // Replay both overwritten instructions before observing success.
        mov eax, dword ptr [ebp - 14h]
        mov ecx, dword ptr [ebp - 0Ch]
        pushfd
        pushad
        push dword ptr [ebp]
        push dword ptr [ebp + 4]
        push eax
        call CompleteInstallBridge
        add esp, 12
        popad
        popfd
        jmp dword ptr [gScenarioInstalledContinuation]
    }
}

bool WriteCode(BYTE* site, const BYTE* bytes, size_t size) {
    DWORD previous = 0;
    if (!VirtualProtect(site, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    memcpy(site, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), site, size);
    DWORD ignored = 0;
    return VirtualProtect(site, size, previous, &ignored) != FALSE;
}
} // namespace

bool InitializeScenarioLifecycle() {
    ShutdownScenarioLifecycle();
    BYTE* image = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    ScenarioLifecycleImageLayout layout;
    if (!Resolve(image, reinterpret_cast<uintptr_t>(image), layout)) return false;
    BYTE* site = reinterpret_cast<BYTE*>(layout.installedSite);
    memcpy(gOriginal, site, sizeof(gOriginal));
    memset(gPatched, 0x90, sizeof(gPatched));
    gPatched[0] = 0xE9;
    const int32_t relative = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&InstalledBridge) -
        (layout.installedSite + 5));
    memcpy(gPatched + 1, &relative, 4);
    AcquireSRWLockExclusive(&gLock);
    gLayout = layout;
    gScenarioInstalledContinuation = layout.installedContinuation;
    ReleaseSRWLockExclusive(&gLock);
    if (!WriteCode(site, gPatched, sizeof(gPatched))) {
        if (!memcmp(site, gPatched, sizeof(gPatched))) WriteCode(site, gOriginal, sizeof(gOriginal));
        return false;
    }
    InterlockedExchange(&gReady, TRUE);
    return true;
}

void ShutdownScenarioLifecycle() {
    InterlockedExchange(&gReady, FALSE);
    AcquireSRWLockExclusive(&gLock);
    BYTE* site = reinterpret_cast<BYTE*>(gLayout.installedSite);
    bool restored = !site;
    if (site) {
        if (!memcmp(site, gOriginal, sizeof(gOriginal))) restored = true;
        else if (!memcmp(site, gPatched, sizeof(gPatched))) restored = WriteCode(site, gOriginal, sizeof(gOriginal));
    }
    // A foreign edit might still branch into our bridge; keep its native
    // continuation valid, but close the observation gate immediately.
    if (restored) { gLayout = ScenarioLifecycleImageLayout(); gScenarioInstalledContinuation = 0; }
    gLoadDepth = 0;
    gSuppressedOrdinal = -1;
    gSuccessfulLoadGeneration = 0;
    gLoadFrames.clear();
    ReleaseSRWLockExclusive(&gLock);
}

int CurrentScenarioOrdinal() {
    AcquireSRWLockShared(&gLock);
    const int result = ReadOrdinalUnlocked();
    ReleaseSRWLockShared(&gLock);
    return result;
}

void BeginScenarioLoad() {
    AcquireSRWLockExclusive(&gLock);
    try {
        gLoadFrames.push_back({gSuppressedOrdinal, gSuccessfulLoadGeneration});
        gSuppressedOrdinal = -1;
        ++gLoadDepth;
    } catch (...) {
        ReleaseSRWLockExclusive(&gLock);
        throw;
    }
    ReleaseSRWLockExclusive(&gLock);
}

void EndScenarioLoad(bool succeeded) {
    AcquireSRWLockExclusive(&gLock);
    if (gLoadDepth && !gLoadFrames.empty()) {
        const LoadFrame frame = gLoadFrames.back();
        gLoadFrames.pop_back();
        --gLoadDepth;
        if (succeeded) {
            ++gSuccessfulLoadGeneration;
            gSuppressedOrdinal = ReadOrdinalUnlocked();
        } else if (gSuccessfulLoadGeneration == frame.successfulGenerationAtBegin) {
            // A failed outer load must not roll back a nested successful load.
            gSuppressedOrdinal = frame.previousSuppressedOrdinal;
        }
    }
    ReleaseSRWLockExclusive(&gLock);
}

bool ScenarioLoadInProgress() {
    AcquireSRWLockShared(&gLock);
    const bool loading = gLoadDepth != 0;
    ReleaseSRWLockShared(&gLock);
    return loading;
}

bool ResolveScenarioLifecycleForTest(BYTE* image, uintptr_t virtualBase, ScenarioLifecycleImageLayout& layout) {
    return Resolve(image, virtualBase, layout);
}

void ConfigureScenarioLifecycleForTest(WORD* ordinal, uintptr_t initialReturn,
    uintptr_t progressReturn, uintptr_t reloadReturn) {
    ShutdownScenarioLifecycle();
    AcquireSRWLockExclusive(&gLock);
    gLayout.ordinalAddress = reinterpret_cast<uintptr_t>(ordinal);
    gLayout.initialCall = initialReturn - 5;
    gLayout.progressCall = progressReturn - 5;
    gLayout.reloadCall = reloadReturn - 5;
    ReleaseSRWLockExclusive(&gLock);
    InterlockedExchange(&gReady, TRUE);
}

void CompleteScenarioInstallForTest(bool succeeded, uintptr_t returnAddress,
    uintptr_t callerFrame, ULONGLONG now) {
    CompleteInstall(succeeded, returnAddress, callerFrame, now);
}

uintptr_t ScenarioInstalledBridgeForTest(uintptr_t continuation) {
    gScenarioInstalledContinuation = continuation;
    return reinterpret_cast<uintptr_t>(&InstalledBridge);
}
