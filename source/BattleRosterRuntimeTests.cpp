#include "BattleRosterRuntime.h"
#include "BattleStats.h"
#include "BattleRuntime.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {
int failures = 0;
int ordinal = 0;
bool capable = false;
int joins = 0, changes = 0, lastPerson = -1, lastCamp = -1;
size_t lastPosition = 0;
std::wstring lastName;
BattleStatsSnapshot snapshot;
uintptr_t bridge = 0;
void Check(bool ok,const char* message) {
    printf("%s %s\n",ok ? "PASS" : "FAIL",message);
    if (!ok) ++failures;
}
bool ReadImage(const wchar_t* path,std::vector<BYTE>& image) {
    FILE* file = nullptr;
    if (_wfopen_s(&file,path,L"rb") || !file) return false;
    fseek(file,0,SEEK_END); const long length = ftell(file); fseek(file,0,SEEK_SET);
    if (length < 256 || length > 128 * 1024 * 1024) { fclose(file); return false; }
    std::vector<BYTE> raw(static_cast<size_t>(length));
    const bool read = fread(raw.data(),1,raw.size(),file) == raw.size();
    fclose(file);
    if (!read) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 ||
        static_cast<size_t>(dos->e_lfanew) > raw.size() - sizeof(IMAGE_NT_HEADERS32)) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage > 128 * 1024 * 1024 ||
        nt->OptionalHeader.SizeOfHeaders > raw.size()) return false;
    image.assign(nt->OptionalHeader.SizeOfImage,0);
    memcpy(image.data(),raw.data(),nt->OptionalHeader.SizeOfHeaders);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        if (section.PointerToRawData > raw.size() || section.SizeOfRawData > raw.size() - section.PointerToRawData ||
            section.VirtualAddress > image.size() || section.SizeOfRawData > image.size() - section.VirtualAddress) return false;
        memcpy(image.data() + section.VirtualAddress,raw.data() + section.PointerToRawData,section.SizeOfRawData);
    }
    return true;
}
size_t CallTarget(const std::vector<BYTE>& image,size_t offset) {
    int32_t delta = 0; memcpy(&delta,image.data() + offset + 1,4);
    return static_cast<size_t>(static_cast<int64_t>(offset) + 5 + delta);
}
__declspec(naked) void NativeContinuation() {
    __asm { mov esp, ebp
        pop ebp
        ret }
}
// A native 3B frame: context argument plus three decoded locals. Tail-jump
// through the real observer proves its register/stack/result contract.
__declspec(naked) int __cdecl RunNativeFrame(void*,int,int) {
    __asm {
        push ebp
        mov ebp, esp
        sub esp, 10h
        mov eax, dword ptr [ebp + 0Ch]
        mov dword ptr [ebp - 10h], eax
        mov eax, dword ptr [ebp + 10h]
        mov dword ptr [ebp - 0Ch], eax
        mov eax, 12345678h
        mov ecx, 23456789h
        mov edx, 3456789Ah
        jmp dword ptr [bridge]
    }
}
void Validate(const wchar_t* path) {
    std::vector<BYTE> image;
    Check(ReadImage(path,image),"map native PE without executing the game");
    if (image.empty()) return;
    BattleRosterRuntimeLayout layout, changedLayout;
    constexpr uintptr_t base = 0x400000;
    const bool resolved = ResolveBattleRosterRuntimeForTest(image.data(),image.size(),base,layout);
    Check(resolved,"uniquely resolve 3B dispatch, parser, R/S callees and committed camp setter");
    if (!resolved) return;
    Check(ResolveBattleDeploymentForTest(image.data(),base),
        "verify deployed state 2, 250-slot loop, stride 30h and XY coordinate consumer");
    const size_t coordinateCall = image[0xC515] == 0xE8 ? 0xC515 : 0xC519;
    for (size_t offset : {size_t(0x18C96),size_t(0x1141A),coordinateCall + 0x36,coordinateCall + 0x39}) {
        auto changed = image;
        changed[offset] ^= 1;
        Check(!ResolveBattleDeploymentForTest(changed.data(),base),
            "changed deployed-state or coordinate ABI disables deployment capability");
    }
    Check(layout.handler == 0x414790 && layout.completedSite == 0x414813 && layout.peoplePointer == 0x4CEA00,
        "three engine samples agree with the independently disassembled ABI");
    Check(ResolveBattleRosterRuntimeForTest(image.data(),image.size(),0x17400000,changedLayout) &&
        changedLayout.handler - 0x17400000 == layout.handler - base,
        "rebased image uses RVAs for hook and authoritative people pointer");
    const size_t handler = layout.handler - base;
    const size_t reader = CallTarget(image,handler + 0x1E);
    const size_t rUpdate = CallTarget(image,handler + 0x6E);
    const size_t getter = CallTarget(image,rUpdate + 0x14);
    const size_t setter = CallTarget(image,rUpdate + 0x146);
    const size_t mutations[] = {handler + 0x23,handler + 0x61,handler + 0x83,
        handler + 0x8D,reader + (image[reader + 11] == 0xE8 ? 0xA2 : 0xA7),rUpdate + 0x13E,getter + 9,setter + 0x1B,
        0x10A38 + (0x3B - 2) * 4};
    for (size_t offset : mutations) {
        auto changed = image;
        changed[offset] ^= 1;
        Check(!ResolveBattleRosterRuntimeForTest(changed.data(),changed.size(),base,changedLayout),
            "changed parameter, dispatch, success, stride or camp relation is rejected");
    }
    auto duplicate = image;
    // Add a second executable dispatch to the same table. Even identical
    // handler candidates must have one authoritative dispatcher.
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(duplicate.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(duplicate.data() + dos->e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    size_t cave = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (!cave && (sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
            cave = sections[i].VirtualAddress + sections[i].Misc.VirtualSize - 64;
    if (cave) {
        memcpy(duplicate.data() + cave,image.data() + 0x10A13,14);
        Check(!ResolveBattleRosterRuntimeForTest(duplicate.data(),duplicate.size(),base,changedLayout),
            "ambiguous native dispatch is rejected");
    }
    BYTE* mapped = static_cast<BYTE*>(VirtualAlloc(nullptr,image.size(),MEM_COMMIT | MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    Check(mapped != nullptr,"allocate isolated hook fixture");
    if (!mapped) return;
    memcpy(mapped,image.data(),image.size());
    Check(InstallBattleRosterRuntimeForTest(mapped,image.size()) && capable,
        "install the observer and enable strict roster capability together");
    BattleRosterRuntimeLayout live;
    // Resolver correctly refuses already patched success bytes.
    Check(!ResolveBattleRosterRuntimeForTest(mapped,image.size(),reinterpret_cast<uintptr_t>(mapped),live),
        "installed success bytes cannot be mistaken for original native bytes");
    std::vector<BYTE> people(0x400 * 0x48,0);
    const uintptr_t peopleBase = reinterpret_cast<uintptr_t>(people.data());
    memcpy(mapped + layout.peoplePointer - base,&peopleBase,4);
    const BYTE caoCao[] = {0xB2,0xDC,0xB2,0xD9};
    memcpy(people.data() + 7 * 0x48 + 8,caoCao,sizeof(caoCao));
    DWORD context[16] = {}; context[2] = 1234;
    bridge = BattleRosterBridgeForTest(reinterpret_cast<uintptr_t>(&NativeContinuation));
    joins = changes = 0; ordinal = 2;
    Check(RunNativeFrame(context,7,0) == 1 && joins == 1 && lastPerson == 7 && lastPosition == 1234,
        "real x86 bridge preserves native return/stack and notifies after committed player join");
    Check(lastName == L"曹操","R join immediately copies the native GBK person name before any battlefield poll");
    CompleteBattleRosterJoinForTest(7,0,1234,20);
    Check(joins == 2,"repeat native 3B reaches the idempotent model notification");
    ordinal = 3;
    CompleteBattleRosterJoinForTest(8,0,0,21);
    Check(joins == 2,"S commands never create campaign membership");
    ordinal = 2;
    people[7 * 0x48 + 0x2A] = 1;
    CompleteBattleRosterJoinForTest(7,0,0,22);
    Check(joins == 2 && changes == 1 && lastCamp == 1,"failed committed camp verification cannot create a player join");
    CompleteBattleRosterJoinForTest(-1,0,0,23);
    CompleteBattleRosterJoinForTest(0x400,0,0,23);
    Check(joins == 2 && changes == 1,"invalid person IDs cannot index the authoritative table");
    snapshot.campaignRoster.clear();
    BattleRosterEntry entry; entry.personId = 7; entry.joined = true;
    snapshot.campaignRoster.push_back(entry);
    PollBattleRosterRuntime(24);
    Check(changes == 2 && lastPerson == 7 && lastCamp == 1,
        "R polling detects faction changes for joined units absent from the battlefield");
    BYTE patched[5] = {};
    BYTE* site = mapped + layout.completedSite - base;
    memcpy(patched,site,5); site[0] = 0x90;
    Check(!ShutdownBattleRosterRuntime() && !capable && site[0] == 0x90,
        "foreign hook mutation is preserved and observation capability closes");
    memcpy(site,patched,5);
    Check(ShutdownBattleRosterRuntime() && !memcmp(site,image.data() + handler + 0x83,5),
        "retry restores the exact native success bytes after ownership is repaired");
    for (int stage = 1; stage <= 3; ++stage) {
        FailBattleRosterWriteForTest(stage);
        Check(!InstallBattleRosterRuntimeForTest(mapped,image.size()) && !capable &&
            ShutdownBattleRosterRuntime() && !memcmp(site,image.data() + handler + 0x83,5),
            "protection/cache/write failure restores native bytes and retains recoverable ownership");
    }
    // Reset continuation replaced by the ABI harness; no game entry executed.
    VirtualFree(mapped,0,MEM_RELEASE);
}
}
int CurrentScenarioOrdinal() { return ordinal; }
void SetBattleRosterCapability(bool available,const wchar_t*) { capable = available; }
void NotifyBattleRosterJoined(int person,int,size_t position,ULONGLONG) {
    ++joins; lastPerson = person; lastPosition = position;
}
void NotifyBattleRosterCamp(int person,int camp,ULONGLONG) {
    ++changes; lastPerson = person; lastCamp = camp;
}
bool ReadBattleStatsSnapshot(BattleStatsSnapshot& result) { result = snapshot; return true; }
bool ReadBattleRosterTrackingIds(std::vector<int>& ids) {
    ids.clear();
    for (const auto& entry : snapshot.campaignRoster) if (entry.joined) ids.push_back(entry.personId);
    return true;
}
void NotifyBattleRosterLabel(int,const std::wstring& name) { lastName = name; }
int wmain(int argc,wchar_t** argv) {
    if (argc != 2) { fwprintf(stderr,L"Usage: BattleRosterRuntimeTests Ekd5.exe\n"); return 2; }
    Validate(argv[1]);
    printf("BattleRosterRuntimeTests: %d failures\n",failures);
    return failures ? 1 : 0;
}
