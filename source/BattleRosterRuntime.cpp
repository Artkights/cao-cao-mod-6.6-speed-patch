#include "BattleRosterRuntime.h"
#include "BattleStats.h"
#include "BattleRuntime.h"
#include "ScenarioLifecycle.h"
#include <cstring>
#include <initializer_list>

extern "C" uintptr_t gBattleRosterContinuation = 0;

namespace {
bool Copy(void* to, const void* from, size_t size) noexcept {
    __try { memcpy(to, from, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct Image {
    BYTE* bytes = nullptr;
    size_t size = 0;
    uintptr_t base = 0;
    uintptr_t preferred = 0;
    IMAGE_SECTION_HEADER sections[96] = {};
    WORD count = 0;
    bool Has(size_t offset, size_t length) const {
        return offset <= size && length <= size - offset;
    }
    bool Code(size_t offset, size_t length) const {
        if (!Has(offset, length)) return false;
        for (WORD i = 0; i < count; ++i) {
            const auto& section = sections[i];
            const size_t span = section.Misc.VirtualSize > section.SizeOfRawData ?
                section.Misc.VirtualSize : section.SizeOfRawData;
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && offset >= section.VirtualAddress &&
                offset - section.VirtualAddress <= span && length <= span - (offset - section.VirtualAddress)) return true;
        }
        return false;
    }
    bool Match(size_t offset, std::initializer_list<int> pattern) const {
        if (!Code(offset, pattern.size())) return false;
        size_t at = 0;
        for (int value : pattern) {
            if (value >= 0 && bytes[offset + at] != value) return false;
            ++at;
        }
        return true;
    }
    size_t Absolute(size_t offset, size_t length) const {
        uint32_t value = 0;
        if (!Has(offset, 4)) return SIZE_MAX;
        memcpy(&value, bytes + offset, 4);
        for (uintptr_t origin : {base, preferred})
            if (value >= origin && Has(value - origin, length)) return value - origin;
        return SIZE_MAX;
    }
    size_t Call(size_t offset) const {
        if (!Code(offset, 5) || bytes[offset] != 0xE8) return SIZE_MAX;
        int32_t delta = 0;
        memcpy(&delta, bytes + offset + 1, 4);
        const int64_t target = static_cast<int64_t>(offset) + 5 + delta;
        return target >= 0 && Code(static_cast<size_t>(target), 1) ?
            static_cast<size_t>(target) : SIZE_MAX;
    }
};
bool Open(BYTE* bytes, size_t size, uintptr_t base, Image& image) {
    IMAGE_DOS_HEADER dos = {};
    IMAGE_NT_HEADERS32 nt = {};
    if (!bytes || size < sizeof(dos) || !Copy(&dos, bytes, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        static_cast<size_t>(dos.e_lfanew) > size || sizeof(nt) > size - dos.e_lfanew ||
        !Copy(&nt, bytes + dos.e_lfanew, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.OptionalHeader.SizeOfImage > size || !nt.FileHeader.NumberOfSections ||
        nt.FileHeader.NumberOfSections > _countof(image.sections)) return false;
    const size_t table = dos.e_lfanew + 24 + nt.FileHeader.SizeOfOptionalHeader;
    const size_t tableSize = nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (table > nt.OptionalHeader.SizeOfHeaders || tableSize > nt.OptionalHeader.SizeOfHeaders - table ||
        table > size || tableSize > size - table || !Copy(image.sections, bytes + table, tableSize)) return false;
    image.bytes = bytes; image.size = nt.OptionalHeader.SizeOfImage;
    image.base = base; image.preferred = nt.OptionalHeader.ImageBase;
    image.count = nt.FileHeader.NumberOfSections;
    return true;
}
bool Resolve(BYTE* bytes, size_t size, uintptr_t base, BattleRosterRuntimeLayout& output) {
    output = {};
    Image image;
    if (!Open(bytes, size, base, image)) return false;
    size_t found = SIZE_MAX, people = SIZE_MAX, sceneMode = SIZE_MAX;
    unsigned matches = 0;
    for (size_t at = 0; at + 0x8F <= image.size; ++at) {
        if (!image.Match(at,{0x55,0x8B,0xEC,0x6A,0,0x83,0xEC,0x0C,0xEB,3,0xFF,0x45,0xFC,
            0x8B,0x45,0xFC,0x3C,2,0x77,0x28,0x8A,0x80,-1,-1,-1,-1,0x50,0x8B,0x4D,8,0xE8}) ||
            image.Absolute(at + 22,3) != at + 0x8C ||
            !image.Match(at + 0x23,{0x66,0x3D,0,0x80,0x75,7,0xB8,5,0,0,0,0xEB,0x58,
                0x0F,0xB7,0xC0,0x8B,0x4D,0xFC,0x89,0x44,0x8D,0xF0,0xEB,0xCE,
                0x66,0x81,0x7D,0xF0,0xFF,0xFF,0x74,0x3F,0x66,0x81,0x7D,0xF4,0xFF,0xFF,
                0x75,4,0xC1,0x6D,0xF4,8,0x66,0x81,0x7D,0xF8,0xFF,0xFF,0x75,4,0x66,0xFF,0x45,0xF8,0xE8}) ||
            !image.Match(at + 0x61,{0x85,0xC0,0x75,0x10,0xFF,0x75,0xF8,0xFF,0x75,0xF4,0xFF,0x75,0xF0,0xE8}) ||
            !image.Match(at + 0x73,{0xEB,0x0E,0xFF,0x75,0xF8,0xFF,0x75,0xF4,0xFF,0x75,0xF0,0xE8}) ||
            !image.Match(at + 0x83,{0xB8,1,0,0,0,0x8B,0xE5,0x5D,0xC3,2,0x0E,0x3E})) continue;
        const size_t reader = image.Call(at + 0x1E);
        const size_t scene = image.Call(at + 0x5C);
        const size_t rUpdate = image.Call(at + 0x6E);
        const size_t sUpdate = image.Call(at + 0x7E);
        const bool reader64 = image.Match(reader,{0x55,0x8B,0xEC,0x6A,0,0x51,0x68,0,0,0,0x80,0xE8}) &&
            image.Match(reader + 0x10,{0x0F,0xB7,0xC0,0x83,0x7D,8,1,0x0F,0x84,0x81,0,0,0}) &&
            image.Match(reader + 0x9E,{0x8B,0xE5,0x5D,0xC2,4,0});
        const bool readerNew = image.Match(reader,{0x55,0x8B,0xEC,0x6A,0,0x51,0x68,0,0,0,0x80,0xFF,0x71,0x2E,0xE8}) &&
            image.Match(reader + 0xA3,{0x8B,0xE5,0x5D,0xC2,4,0});
        const bool scene64 = image.Match(scene,{0x55,0x8B,0xEC,0xB9,-1,-1,-1,-1,0xE8,-1,-1,-1,-1,
            0x25,0xFF,0,0,0,0x33,0xC9,0x83,0xF8,3,0x0F,0x94,0xC1,0x8B,0xC1,0x5D,0xC3});
        const bool sceneNew = image.Match(scene,{0x55,0x8B,0xEC,0xE8,-1,-1,-1,-1,
            0x33,0xC9,0x3C,3,0x0F,0x94,0xC1,0x8B,0xC1,0x5D,0xC3});
        // The 6.4 base keeps the compact reader epilogue while sharing the
        // newer scene-mode accessor used by 6.5/6.6.  Reader and scene
        // layouts therefore cannot be required to use the same generation;
        // each callee is validated independently below.
        if ((!reader64 && !readerNew) || (!scene64 && !sceneNew) ||
            !image.Match(rUpdate,{0x55,0x8B,0xEC,0x6A,0,0x83,0xEC,0x2C,0x81,0x7D,8,0,4,0,0,0x73,0x5D,0xFF,0x75,8,0xE8}) ||
            !image.Match(sUpdate,{0x55,0x8B,0xEC,0x6A,0,0x6A,0,0x6A,0,0x83,0xEC,0x0C,0xFF,0x75,8,0xE8})) continue;
        const size_t getter = image.Call(rUpdate + 0x14);
        const size_t setCamp = image.Call(rUpdate + 0x146);
        const size_t sceneGetter = scene64 ? scene : image.Call(scene + 3);
        const size_t modeAccessor = image.Call(sceneGetter + 8);
        const size_t mode = image.Absolute(sceneGetter + 4,1);
        if (!image.Match(getter,{0x55,0x8B,0xEC,0x0F,0xB7,0x4D,8,0x6B,0xC9,0x48,3,0x0D,-1,-1,-1,-1,0x5D,0xC2,4,0}) ||
            (!scene64 && !image.Match(sceneGetter,{0x55,0x8B,0xEC,0xB9,-1,-1,-1,-1,0xE8,-1,-1,-1,-1,0x5D,0xC3})) ||
            !image.Match(modeAccessor,{0x55,0x8B,0xEC,0x0F,0xB6,1,0x5D,0xC3}) || mode == SIZE_MAX ||
            !image.Match(rUpdate + 0x13E,{0xFF,0x75,0x0C,0x6A,0,0x8B,0x4D,0xE4,0xE8}) ||
            !image.Match(setCamp,{0x55,0x8B,0xEC,0x6A,0,0x51,0x68,-1,-1,-1,-1,0x6A,0,
                0x8A,0x45,8,0x38,0x41,0x2A,0x0F,0x84,-1,-1,-1,-1,0x88,0x41,0x2A})) continue;
        const size_t pointer = image.Absolute(getter + 12,4);
        if (pointer == SIZE_MAX) continue;
        unsigned dispatches = 0;
        for (size_t call = 4; call + 10 < image.size; ++call) {
            if (!image.Match(call - 4,{0x52,0x83,0xE8,2,0xFF,0x14,0x85,-1,-1,-1,-1,0x83,0xC4,4})) continue;
            const size_t table = image.Absolute(call + 3,0x78 * 4);
            if (table != SIZE_MAX && image.Absolute(table + (0x3B - 2) * 4,1) == at) ++dispatches;
        }
        if (dispatches != 1) continue;
        ++matches; found = at; people = pointer; sceneMode = mode;
    }
    if (matches != 1) return false;
    output.handler = base + found;
    output.completedSite = base + found + 0x83;
    output.continuation = output.completedSite + 5;
    output.peoplePointer = base + people;
    output.sceneMode = base + sceneMode;
    return true;
}
BattleRosterRuntimeLayout gLayout;
BYTE gOriginal[5] = {}, gPatched[5] = {};
volatile LONG gReady = FALSE;
volatile LONG gInFlight = 0;
SRWLOCK gLock = SRWLOCK_INIT;
DWORD gOriginalProtection = 0;
bool gProtectionRecorded = false;
bool gProtectionPending = false;
bool gFlushPending = false;
ULONGLONG gLastPoll = 0;
#ifdef XA66_TEST
int gFailStage = 0;
#endif

bool ReadCamp(const BattleRosterRuntimeLayout& layout, int person, int& camp) {
    uintptr_t people = 0;
    BYTE value = 0;
    if (person < 0 || person >= 0x400 || !layout.peoplePointer ||
        !Copy(&people,reinterpret_cast<void*>(layout.peoplePointer),sizeof(DWORD)) || !people ||
        !Copy(&value,reinterpret_cast<void*>(people + person * 0x48 + 0x2A),1)) return false;
    camp = value;
    return true;
}
void Complete(int person, int requestedCamp, size_t commandPosition, ULONGLONG now) {
    if (!InterlockedCompareExchange(&gReady,FALSE,FALSE)) return;
    const int ordinal = CurrentScenarioOrdinal();
    if (ordinal < 0 || (ordinal & 1) || person < 0 || person >= 0x400) return;
    BattleRosterRuntimeLayout layout;
    AcquireSRWLockShared(&gLock); layout = gLayout; ReleaseSRWLockShared(&gLock);
    BYTE mode = 3;
    if (!Copy(&mode,reinterpret_cast<void*>(layout.sceneMode),1) || mode == 3) return;
    int camp = -1;
    if (!ReadCamp(layout,person,camp)) return;
    // FFFF means remove; only an explicit successful player-camp request can
    // reactivate a previously changed-faction row. Confirm the committed camp.
    if (requestedCamp == 0 && camp == 0) {
        NotifyBattleRosterJoined(person,ordinal,commandPosition,now);
        std::wstring name;
        if (ReadBattlePersonName(layout.peoplePointer,person,name)) NotifyBattleRosterLabel(person,name);
    } else NotifyBattleRosterCamp(person,camp,now);
}
void __cdecl CompleteBridge(uintptr_t frame) {
    try {
        int person = -1, camp = -1;
        uintptr_t context = 0;
        DWORD position = 0;
        if (!Copy(&person,reinterpret_cast<void*>(frame - 0x10),4) ||
            !Copy(&camp,reinterpret_cast<void*>(frame - 0x0C),4) ||
            !Copy(&context,reinterpret_cast<void*>(frame + 8),4) || !context ||
            !Copy(&position,reinterpret_cast<void*>(context + 8),4)) return;
        Complete(person,camp,position,GetTickCount64());
    } catch (...) { /* Observation cannot alter the native command result. */ }
}
__declspec(naked) void CompletedBridge() {
    __asm {
        mov eax, 1
        pushfd
        lock inc dword ptr [gInFlight]
        pushad
        push ebp
        call CompleteBridge
        add esp, 4
        popad
        lock dec dword ptr [gInFlight]
        popfd
        jmp dword ptr [gBattleRosterContinuation]
    }
}
bool WriteCode(void* address,const BYTE* bytes,size_t size) {
    DWORD before = 0, ignored = 0;
#ifdef XA66_TEST
    if (gFailStage == 1) { gFailStage = 0; return false; }
#endif
    if (!VirtualProtect(address,size,PAGE_EXECUTE_READWRITE,&before)) return false;
    if (!gProtectionRecorded) { gOriginalProtection = before; gProtectionRecorded = true; }
    gProtectionPending = true;
    const bool copied = Copy(address,bytes,size);
    gFlushPending = true;
    bool flushed = false, restored = false;
#ifdef XA66_TEST
    if (gFailStage == 2) gFailStage = 0;
    else
#endif
        flushed = FlushInstructionCache(GetCurrentProcess(),address,size) != FALSE;
#ifdef XA66_TEST
    if (gFailStage == 3) gFailStage = 0;
    else
#endif
        restored = VirtualProtect(address,size,gOriginalProtection,&ignored) != FALSE;
    gFlushPending = !flushed; gProtectionPending = !restored;
    return copied && flushed && restored;
}
bool Install(BYTE* image,size_t size) {
    if (!ShutdownBattleRosterRuntime()) return false;
    BattleRosterRuntimeLayout layout;
    if (!Resolve(image,size,reinterpret_cast<uintptr_t>(image),layout)) return false;
    memcpy(gOriginal,reinterpret_cast<void*>(layout.completedSite),5);
    gPatched[0] = 0xE9;
    const int32_t delta = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&CompletedBridge) - layout.completedSite - 5);
    memcpy(gPatched + 1,&delta,4);
    AcquireSRWLockExclusive(&gLock);
    gLayout = layout; gBattleRosterContinuation = layout.continuation;
    ReleaseSRWLockExclusive(&gLock);
    if (!WriteCode(reinterpret_cast<void*>(layout.completedSite),gPatched,5)) {
        if (!memcmp(reinterpret_cast<void*>(layout.completedSite),gPatched,5))
            WriteCode(reinterpret_cast<void*>(layout.completedSite),gOriginal,5);
        return false;
    }
    InterlockedExchange(&gReady,TRUE);
    gLastPoll = 0;
    SetBattleRosterCapability(true,L"3B runtime handler verified");
    return true;
}
}

bool InitializeBattleRosterRuntime() {
    BYTE* image = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos = {};
    IMAGE_NT_HEADERS32 nt = {};
    if (!Copy(&dos,image,sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 ||
        dos.e_lfanew > 0x100000 || !Copy(&nt,image + dos.e_lfanew,sizeof(nt))) return false;
    return Install(image,nt.OptionalHeader.SizeOfImage);
}
void PollBattleRosterRuntime(ULONGLONG now) {
    if (!InterlockedCompareExchange(&gReady,FALSE,FALSE)) return;
    if (gLastPoll && now - gLastPoll < 100) return;
    gLastPoll = now;
    BattleRosterRuntimeLayout layout;
    AcquireSRWLockShared(&gLock); layout = gLayout; ReleaseSRWLockShared(&gLock);
    try {
        // Reuse the small ID buffer; avoid copying both ledgers, strings and
        // all capability fields on every message-loop poll.
        static thread_local std::vector<int> ids;
        if (!ReadBattleRosterTrackingIds(ids)) return;
        for (int person : ids) {
            int camp = -1;
            if (ReadCamp(layout,person,camp)) NotifyBattleRosterCamp(person,camp,now);
        }
    } catch (...) { /* Observation failures cannot break the message loop. */ }
}
bool ShutdownBattleRosterRuntime() {
    InterlockedExchange(&gReady,FALSE);
    SetBattleRosterCapability(false,L"3B runtime handler unavailable");
    AcquireSRWLockExclusive(&gLock);
    BYTE* site = reinterpret_cast<BYTE*>(gLayout.completedSite);
    bool restored = !site;
    if (site) {
        const bool original = !memcmp(site,gOriginal,5);
        restored = original && !gProtectionPending && !gFlushPending;
        if (!restored && (original || !memcmp(site,gPatched,5)))
            restored = WriteCode(site,gOriginal,5);
    }
    if (restored) {
        gLayout = {}; gProtectionRecorded = false;
        gProtectionPending = gFlushPending = false;
        // A bridge that entered before gReady closed must retain its native
        // tail target even after the site is restored. Never zero it here.
        // Likewise retain it after a foreign edit that may still call us.
    }
    ReleaseSRWLockExclusive(&gLock);
    return restored;
}
bool ResolveBattleRosterRuntimeForTest(BYTE* image,size_t size,uintptr_t base,
    BattleRosterRuntimeLayout& layout) { return Resolve(image,size,base,layout); }
#ifdef XA66_TEST
bool InstallBattleRosterRuntimeForTest(BYTE* image,size_t size) { return Install(image,size); }
void CompleteBattleRosterJoinForTest(int person,int camp,size_t position,ULONGLONG now) {
    Complete(person,camp,position,now);
}
uintptr_t BattleRosterBridgeForTest(uintptr_t continuation) {
    gBattleRosterContinuation = continuation;
    return reinterpret_cast<uintptr_t>(&CompletedBridge);
}
void FailBattleRosterWriteForTest(int stage) { gFailStage = stage; }
#endif
