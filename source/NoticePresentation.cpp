#include "NoticePresentation.h"
#include "AnimationRuntime.h"
#include "PresentationDiagnostics.h"
#include <cstring>
#include <intrin.h>

#pragma optimize("y", off)
namespace {
constexpr uintptr_t kOrigin = 0x400000;
constexpr size_t kHookCount = 7, kDepthLimit = 64;
enum Role : unsigned { Information, Loot, Conditions, Chapter, ChapterLoop, GiftLoop, GiftTail };
struct Hook {
    uintptr_t address = 0;
    BYTE original[16] = {}, replacement[16] = {}, current[16] = {};
    size_t length = 0;
    void* allocation = nullptr;
    DWORD protection = 0;
    bool protectionKnown = false, protectionPending = false, cachePending = false, currentKnown = true;
#ifdef XA66_AUDIT
    unsigned relativeCallOffset = 0xFF, argumentBytes = 0;
#endif
};
struct Frame {
    uintptr_t slot = 0, nativeReturn = 0, token = 0, diagnostic = 0;
    bool returnReplaced = false;
};
Hook gHooks[kHookCount];
thread_local Frame gFrames[kDepthLimit];
thread_local size_t gDepth = 0;
uintptr_t gBase = 0;
bool g66 = false, g64 = false, gPrepared = false, gInstalled = false;
bool gCancelNotice = false, gProtect = false, gRecovery = false;
bool gStrategyNameReady = false;
uintptr_t gStrategyNameFormat = 0;
uintptr_t gStrategyNameReturn = 0;
volatile LONG gActive = 0;
#ifdef XA66_TEST
int gFailForward = -1, gFailRollback = -1, gForwardCount = 0, gRollbackCount = 0;
bool gAfterCopy = false, gFailCache = false;
#endif
extern "C" void NoticeExitBridge();
bool Read(uintptr_t address, void* data, size_t size) noexcept {
    __try { memcpy(data, reinterpret_cast<const void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Dword(BYTE*& cursor, uintptr_t value) noexcept {
    const DWORD v = static_cast<DWORD>(value); memcpy(cursor, &v, 4); cursor += 4;
}
void Jump(BYTE* at, uintptr_t address, uintptr_t target) noexcept {
    at[0] = 0xE9; const DWORD d = static_cast<DWORD>(target - address - 5); memcpy(at + 1, &d, 4);
}
bool Write(Hook& h, const BYTE* bytes, bool rollback = false) noexcept {
    bool failProtection = false, failCache = false;
#ifdef XA66_TEST
    int& count = rollback ? gRollbackCount : gForwardCount;
    const int failure = rollback ? gFailRollback : gFailForward;
    if (failure >= 0 && count++ == failure) {
        if (!gAfterCopy) return false;
        failCache = gFailCache; failProtection = !failCache;
    }
#else
    (void)rollback;
#endif
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(h.address), h.length, PAGE_EXECUTE_READWRITE, &old)) return false;
    if (!h.protectionKnown) { h.protection = old; h.protectionKnown = true; }
    h.protectionPending = true;
    bool copied = false;
    __try { memcpy(reinterpret_cast<void*>(h.address), bytes, h.length); copied = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    DWORD ignored = 0;
    const bool restored = !failProtection && VirtualProtect(reinterpret_cast<void*>(h.address), h.length, h.protection, &ignored);
    if (restored) h.protectionPending = false;
    h.cachePending = failCache || !FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(h.address), h.length);
    h.currentKnown = Read(h.address, h.current, h.length);
    return copied && restored && !h.cachePending && h.currentKnown && !memcmp(h.current, bytes, h.length);
}
void PopFrame(const char* result = "complete") noexcept {
    if (!gDepth) return;
    const Frame frame = gFrames[--gDepth];
    if (frame.token) EndAnimationPresentationScope(frame.token);
    EndPresentationDiagnostic(frame.diagnostic,result);
    InterlockedDecrement(&gActive);
}
void ValidateFrames() noexcept {
    uintptr_t frame = 0; __asm mov frame, ebp
    const uintptr_t low = __readfsdword(8), high = __readfsdword(4);
    uintptr_t stackPointer = 0; __asm mov stackPointer, esp
    while (gDepth) {
        const Frame& top = gFrames[gDepth - 1];
        bool found = false;
        uintptr_t cursor = frame;
        for (unsigned hops = 0; hops < 2048 && cursor >= low && cursor < high - 8 && !(cursor & 3); ++hops) {
            if (cursor + 4 == top.slot) { found = true; break; }
            uintptr_t next = 0;
            if (!Read(cursor, &next, 4) || next <= cursor) break;
            cursor = next;
        }
        if (found) break;
        // Never leave our exit bridge in a live opaque frame when its shadow
        // record is discarded. External scope validation independently goes native.
        if (top.returnReplaced && top.slot >= stackPointer && top.slot >= low &&
            high >= 4 && top.slot <= high - 4 && !(top.slot & 3)) {
            uintptr_t actual = 0;
            if (Read(top.slot, &actual, 4) && actual == reinterpret_cast<uintptr_t>(&NoticeExitBridge)) {
                __try { *reinterpret_cast<uintptr_t*>(top.slot) = top.nativeReturn; }
                __except (EXCEPTION_EXECUTE_HANDLER) { return; }
            }
        }
        PopFrame("unwind");
    }
}
bool PushFrame(uintptr_t slot, uintptr_t nativeReturn, bool replace, unsigned role) noexcept {
    if (gDepth >= kDepthLimit) { PreserveNativePresentationUntilIdle();return false; }
    // A verified native frame anchors the child policy to the native stack.
    // The shared presentation gate removes it automatically after SEH/longjmp.
    const uintptr_t token = gProtect ? BeginAnimationPresentationScopeForFrame(false, slot) : 0;
    if (gProtect && !token) { PreserveNativePresentationUntilIdle();return false; }
    const uintptr_t diagnostic=BeginPresentationDiagnostic(3,gHooks[role].address-gBase+kOrigin,role,-1,false);
    gFrames[gDepth++] = {slot,nativeReturn,token,diagnostic,replace};
    InterlockedIncrement(&gActive);
    if (replace) *reinterpret_cast<uintptr_t*>(slot) = reinterpret_cast<uintptr_t>(&NoticeExitBridge);
    return true;
}
bool InformationCaller(uintptr_t normalized) noexcept {
    // Reviewed automatic notices only. Shared formatter callers used by active
    // queries, confirmations, allocation and tutorial input stay native.
    constexpr uintptr_t common[] = {0x4084EF,0x414AA2,0x41DEAA};
    constexpr uintptr_t native6465[] = {
        0x407543,0x410072,0x412886,0x413A35,0x423A90,0x43499A,
        0x434B41,0x434D13,0x434F25,0x434F78
    };
    constexpr uintptr_t native66[] = {
        0x40750C,0x412876,0x413A26,0x414521,0x417DBC,
        0x432D81,0x433E3A,0x433F99
    };
    for (uintptr_t call : common) if (normalized == call + 5) return true;
    if (g66) {
        for (uintptr_t call : native66) if (normalized == call + 5) return true;
    } else {
        for (uintptr_t call : native6465) if (normalized == call + 5) return true;
        if (g64 && (normalized == 0x414502 || normalized == 0x459ADD || normalized == 0x40BB46)) return true;
        if (!g64 && (normalized == 0x414526 || normalized == 0x41BAB6)) return true;
    }
    return false;
}
bool DirectNativeCaller(uintptr_t nativeReturn, uintptr_t target) noexcept {
    const uintptr_t normalized = nativeReturn - gBase + kOrigin;
    if (normalized < 0x401005 || normalized >= 0x488000) return false;
    BYTE call[5] = {};
    int32_t displacement = 0;
    if (!Read(nativeReturn - 5, call, 5) || call[0] != 0xE8) return false;
    memcpy(&displacement, call + 1, 4);
    return nativeReturn + displacement == gBase + target - kOrigin;
}
extern "C" unsigned __cdecl NoticeEnter(unsigned role, uintptr_t* slot, uintptr_t nativeFrame) noexcept {
    if (!slot || role >= kHookCount) return 0;
    ValidateFrames();
    if (!ConsumeAnimationScenarioGenerationInvalidation()) return 0;
    if (!gInstalled || gRecovery) return 0;
    if (role == GiftTail) {
        if (gDepth && !gFrames[gDepth - 1].returnReplaced && gFrames[gDepth - 1].slot == nativeFrame + 4) PopFrame();
        return 0;
    }
    if (role == ChapterLoop) {
        if(gCancelNotice) { const auto diagnostic=BeginPresentationDiagnostic(3,0x40B98D,role,-1,true);EndPresentationDiagnostic(diagnostic,"skipped"); }
        return gCancelNotice ? 1u : 0u;
    }
    if (role == GiftLoop) {
        if (gCancelNotice) { const auto diagnostic=BeginPresentationDiagnostic(3,0x4599DD,role,-1,true);EndPresentationDiagnostic(diagnostic,"skipped");return 1; }
        PushFrame(nativeFrame + 4, 0, false, role);
        return 0;
    }
    uintptr_t nativeReturn = *slot;
    uintptr_t resolved = 0;
    if (ResolveNativeAnimationReturn(reinterpret_cast<uintptr_t>(slot), nativeReturn, resolved)) nativeReturn = resolved;
    bool skip = false;
    bool automaticStatusNotice = false;
    if (role == Information && g66 && AnimationPresentationOwnerActive(0x43CDDD)) {
        const uintptr_t caller = nativeReturn - gBase + kOrigin;
        DWORD mode = 0xFFFFFFFF;
        // Phase status settlement can cross level/equipment thresholds. These
        // four noninteractive notices belong to that live Status operation,
        // including its explicit native choice when Status is excluded.
        // Mode 1 still returns a user choice and always runs natively.
        automaticStatusNotice = (caller == 0x407511 || caller == 0x4084F4 ||
            caller == 0x414AA7 || caller == 0x41DEAF) &&
            DirectNativeCaller(nativeReturn,0x42F699) &&
            Read(reinterpret_cast<uintptr_t>(slot + 1),&mode,4) && (mode == 0 || mode == 2);
        if (automaticStatusNotice) skip = AnimationPresentationOwnerSelected(0x43CDDD);
    }
    if (role == Information && gStrategyNameReady &&
        nativeReturn - gBase + kOrigin == gStrategyNameReturn &&
        DirectNativeCaller(nativeReturn, 0x42F699)) {
        DWORD mode = 0xFFFFFFFF;
        uintptr_t format = 0;
        // This exact cdecl call formats the strategy's automatic name. Its
        // caller ignores the result and owns all three arguments. Decide
        // before the ordinary notice establishes its protective native child;
        // an existing unselected child remains a boundary in the owner query.
        skip = Read(reinterpret_cast<uintptr_t>(slot + 1), &mode, 4) && mode == 2 &&
            Read(reinterpret_cast<uintptr_t>(slot + 2), &format, sizeof(format)) &&
            format == gStrategyNameFormat && AnimationSelectedStrategyOwner();
    }
    if (gCancelNotice && !automaticStatusNotice) {
        if (role == Information) {
            // Mode 1 returns a user choice. Modes 0/2 have no business return;
            // their native callers parse and settle outside this formatter.
            DWORD mode = 0xFFFFFFFF;
            skip = skip || (Read(reinterpret_cast<uintptr_t>(slot + 1), &mode, 4) &&
                (mode == 0 || mode == 2) && InformationCaller(nativeReturn - gBase + kOrigin) &&
                DirectNativeCaller(nativeReturn, 0x42F699));
        } else if (role == Loot) {
            skip = nativeReturn - gBase + kOrigin == (g66 ? uintptr_t(0x41695F) : uintptr_t(0x416982));
        } else if (role == Conditions) {
            skip = nativeReturn - gBase + kOrigin == 0x413C10;
        }
    }
    if (skip) { const auto diagnostic=BeginPresentationDiagnostic(3,gHooks[role].address-gBase+kOrigin,role,-1,true);EndPresentationDiagnostic(diagnostic,"skipped");return 1; }
    PushFrame(reinterpret_cast<uintptr_t>(slot), *slot, true, role);
    return 0;
}
extern "C" uintptr_t __cdecl NoticeLeave() noexcept {
    if (!gDepth || !gFrames[gDepth - 1].returnReplaced) return 0;
    const uintptr_t result = gFrames[gDepth - 1].nativeReturn;
    PopFrame(); return result;
}
extern "C" __declspec(naked) void NoticeExitBridge() {
    __asm {
        pushfd
        pushad
        call NoticeLeave
        mov [esp + 12], eax
        popad
        popfd
        push dword ptr [esp - 24]
        ret
    }
}
bool PrepareHook(unsigned role, uintptr_t address, const BYTE* expected, size_t length,
    unsigned argumentBytes = 0, uintptr_t skipAddress = 0, unsigned relativeCall = 0xFF) noexcept {
    Hook& h = gHooks[role]; h.address = gBase + address - kOrigin; h.length = length;
#ifdef XA66_AUDIT
    h.relativeCallOffset = relativeCall; h.argumentBytes = argumentBytes;
#endif
    if (length < 5 || length > 16 || !Read(h.address, h.original, length) || memcmp(h.original, expected, length)) return false;
    memcpy(h.current, h.original, length);
    MEMORY_BASIC_INFORMATION memory = {};
    if (!VirtualQuery(reinterpret_cast<void*>(h.address), &memory, sizeof(memory))) return false;
    h.protection = memory.Protect; h.protectionKnown = true;
    h.allocation = VirtualAlloc(nullptr, 160, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!h.allocation) return false;
    BYTE* trampoline = static_cast<BYTE*>(h.allocation);
    memcpy(trampoline, h.original, length);
    if (relativeCall != 0xFF) {
        if (relativeCall + 5 > length || h.original[relativeCall] != 0xE8) return false;
        int32_t d = 0; memcpy(&d, h.original + relativeCall + 1, 4);
        const DWORD moved = static_cast<DWORD>(h.address + relativeCall + 5 + d - reinterpret_cast<uintptr_t>(trampoline + relativeCall + 5));
        memcpy(trampoline + relativeCall + 1, &moved, 4);
    }
    Jump(trampoline + length, reinterpret_cast<uintptr_t>(trampoline + length), h.address + length);
    BYTE* code = trampoline + 24;
    const uintptr_t entry = reinterpret_cast<uintptr_t>(code);
    *code++ = 0x9C; *code++ = 0x60;
    // Saved EBP + original return slot are independent: ranges use the native
    // frame; entries have not executed their push-ebp prolog yet.
    const BYTE args[] = {0xFF,0x74,0x24,0x08,0x8D,0x44,0x24,0x28,0x50,0x68};
    memcpy(code,args,sizeof(args)); code += sizeof(args); Dword(code,role);
    *code++=0xB8; Dword(code,reinterpret_cast<uintptr_t>(&NoticeEnter)); *code++=0xFF; *code++=0xD0;
    *code++=0x83; *code++=0xC4; *code++=0x0C;
    if (role != Chapter && role != GiftTail) {
        *code++=0x85; *code++=0xC0; *code++=0x74;
        if (skipAddress) {
            *code++=0x08; *code++=0x61; *code++=0x9D; *code++=0x68;
            Dword(code,gBase+skipAddress-kOrigin); *code++=0xC3;
        } else {
            *code++=0x07; *code++=0x61; *code++=0x9D;
            *code++=0x31; *code++=0xC0; *code++=0xC2;
            *code++=static_cast<BYTE>(argumentBytes); *code++=0;
        }
    }
    *code++=0x61; *code++=0x9D; *code++=0x68; Dword(code,reinterpret_cast<uintptr_t>(trampoline)); *code++=0xC3;
    memset(h.replacement,0x90,length); Jump(h.replacement,h.address,entry);
    return FlushInstructionCache(GetCurrentProcess(),h.allocation,160)!=FALSE;
}
void FreeHooks() noexcept {
    for (Hook& h:gHooks) { if (h.allocation) VirtualFree(h.allocation,0,MEM_RELEASE); h=Hook(); }
    gPrepared=false; gStrategyNameReady=false; gStrategyNameFormat=0; gStrategyNameReturn=0;
}
bool Validate(const Hook& h) noexcept {
    BYTE actual[16] = {};
    return h.currentKnown && Read(h.address,actual,h.length) && !memcmp(actual,h.current,h.length);
}
}
bool PrepareNoticePresentation(const EngineLayout& layout, uintptr_t gameBase, uintptr_t virtualBase) noexcept {
    if (!RestoreNoticePresentation()) return false;
    FreeHooks(); gBase=gameBase; g66=layout.profile==EngineProfileId::Ccz66;g64=layout.profile==EngineProfileId::Ccz64;
    if (!gBase || (layout.profile!=EngineProfileId::Ccz64 && layout.profile!=EngineProfileId::Ccz65 && !g66)) return false;
    const uintptr_t operandBase=virtualBase?virtualBase:gBase;
    // Keep this optional presentation path native if a MOD changes the exact
    // caller, mode or format. This is independent of the dialog checkbox.
    // The 6.4/6.5 caller (also present in decoded 2.2.0) is nineteen bytes
    // later than 6.6. Both use cdecl and the same three-argument formatter;
    // keep the selected profile's complete identity instead of matching a
    // shared formatter or scanning for a similar instruction sequence.
    const uintptr_t strategyStart = g66 ? 0x421A41 : 0x421A54;
    BYTE strategyName[21] = {};
    BYTE formatText[3] = {};
    const uintptr_t strategyFormat = operandBase + 0x48B58C - kOrigin;
    DWORD encodedFormat = 0;
    if (Read(gBase + strategyStart - kOrigin, strategyName, sizeof(strategyName)) &&
        Read(gBase + 0x48B58C - kOrigin, formatText, sizeof(formatText))) {
        memcpy(&encodedFormat, strategyName + 2, sizeof(encodedFormat));
        gStrategyNameReady = strategyName[0] == 0x51 && strategyName[1] == 0x68 &&
            encodedFormat == strategyFormat && strategyName[6] == 0x6A && strategyName[7] == 2 &&
            formatText[0] == '%' && formatText[1] == 's' && formatText[2] == 0 &&
            DirectNativeCaller(gBase + strategyStart + 13 - kOrigin, 0x42F699) &&
            strategyName[13] == 0x83 && strategyName[14] == 0xC4 && strategyName[15] == 12 &&
            // Native clock sampling immediately overwrites EAX, proving that
            // no strategy business consumes the omitted formatter's result.
            DirectNativeCaller(gBase + strategyStart + 21 - kOrigin, 0x47EB97);
    }
    if (gStrategyNameReady) {
        gStrategyNameFormat = strategyFormat;
        gStrategyNameReturn = strategyStart + 13;
    }
    const BYTE info[]={0x55,0x8B,0xEC,0x6A,0x01};
    const BYTE loot[]={0x55,0x8B,0xEC,0x83,0xEC,0x44};
    const BYTE loot66[]={0x55,0x8B,0xEC,0x68,0,0,0,0};
    BYTE relocatedLoot66[8]; memcpy(relocatedLoot66,loot66,8);
    const DWORD loot66Operand=static_cast<DWORD>(operandBase+0x48E1A8-kOrigin); memcpy(relocatedLoot66+4,&loot66Operand,4);
    const BYTE cond[]={0x55,0x8B,0xEC,0x83,0xEC,0x4C};
    const BYTE cond66[]={0x55,0x8B,0xEC,0x83,0xEC,0x10};
    const BYTE chapter[]={0x55,0x8B,0xEC,0x83,0xEC,0x10};
    BYTE chapterLoop[]={0x6A,0x04,0xB9,0,0,0,0};
    const DWORD chapterContext=static_cast<DWORD>(operandBase+0x492F38-kOrigin); memcpy(chapterLoop+3,&chapterContext,4);
    const BYTE giftLoop[]={0x8B,0x4D,0xF4,0xE8,0x7B,0x99,0xFA,0xFF};
    const BYTE tail[]={0x8B,0x4D,0x08,0xE8,0x27,0x9D,0x02,0};
    const BYTE tail64[]={0xFF,0x75,0x08,0xE8,0xCF,0x1A,0xFB,0xFF};
    const bool ok=PrepareHook(Information,0x42F699,info,sizeof(info),0) &&
        PrepareHook(Loot,0x44B5A8,g66?relocatedLoot66:loot,g66?8:6,16) &&
        PrepareHook(Conditions,g66?0x44BE80:0x44BD27,g66?cond66:cond,6,4) &&
        PrepareHook(Chapter,0x40B935,chapter,6) &&
        PrepareHook(ChapterLoop,0x40B98D,chapterLoop,7,0,g66?0x40B9FD:0x40BA02) &&
        PrepareHook(GiftLoop,0x4599DD,giftLoop,8,0,0x459A93,3) &&
        PrepareHook(GiftTail,0x459A93,layout.profile==EngineProfileId::Ccz64?tail64:tail,8,0,0,3);
    if (!ok) { FreeHooks(); return false; }
    // Verify business/interactive boundaries as well as hook prefixes. The
    // 3D handler must retain its inventory writer and 1A its explicit UI call.
    BYTE call[5]={}; int32_t d=0;
    if (!Read(gBase+0x413C0B-kOrigin,call,5) || call[0]!=0xE8) { FreeHooks();return false; }
    memcpy(&d,call+1,4);
    if (0x413C10+d!=(g66?0x44BE80:0x44BD27)) { FreeHooks();return false; }
    const uintptr_t mandatory[]={0x4084EF,0x414AA2,0x41DEAA,
        g66?uintptr_t(0x40750C):uintptr_t(0x407543),
        g66?uintptr_t(0x412876):uintptr_t(0x412886),
        g66?uintptr_t(0x413A26):uintptr_t(0x413A35),
        g66||!g64?uintptr_t(0x414521):uintptr_t(0x459AD8),
        g66?uintptr_t(0x417DBC):g64?uintptr_t(0x40BB41):uintptr_t(0x41BAB1)};
    for(uintptr_t site:mandatory) if(!DirectNativeCaller(gBase+site+5-kOrigin,0x42F699)) { FreeHooks();return false; }
    BYTE mode0[5]={},mode1[2]={},mode2[2]={},epilogue[7]={};
    const BYTE mode0Expected[]={0x8B,0x45,0x08,0x3C,0x00};
    const BYTE epilogueExpected[]={0x8B,0x45,0xFC,0x8B,0xE5,0x5D,0xC3};
    const uintptr_t mode0Site=g64?0x42F6CC:0x42F6BF;
    const uintptr_t mode1Site=g64?0x42F6FE:0x42F6EE;
    const uintptr_t mode2Site=g64?0x42F72A:0x42F708;
    const uintptr_t epilogueSite=g64?0x42F74D:0x42F729;
    if(!Read(gBase+mode0Site-kOrigin,mode0,5)||memcmp(mode0,mode0Expected,5)||
        !Read(gBase+mode1Site-kOrigin,mode1,2)||mode1[0]!=0x3C||mode1[1]!=1||
        !Read(gBase+mode2Site-kOrigin,mode2,2)||mode2[0]!=0x3C||mode2[1]!=2||
        !Read(gBase+epilogueSite-kOrigin,epilogue,7)||memcmp(epilogue,epilogueExpected,7)) {
        FreeHooks();return false;
    }
    gPrepared=true; return true;
}
bool ConfigureNoticePresentation(bool cancelDialog, bool protectPresentation) noexcept {
    if (!NoticePresentationIdle()) return false;
    const bool install=cancelDialog||protectPresentation;
    if (gRecovery) {
        if (install) return false;
        bool restored=true;
        for (size_t i=kHookCount;i;--i) {
            Hook& h=gHooks[i-1];
            if (!Validate(h)) { restored=false;continue; }
            if (memcmp(h.current,h.original,h.length)||h.protectionPending||h.cachePending)
                restored=Write(h,h.original,true)&&restored;
        }
        if (!restored) return false;
        gRecovery=gInstalled=false;
    }
    if (!install&&!gInstalled) { gCancelNotice=gProtect=false; return true; }
    if (install&&!gPrepared) return false;
    for (const Hook& h:gHooks) if (!Validate(h)) {
        if (gInstalled) { gRecovery=true;gCancelNotice=gProtect=false; }
        return false;
    }
    if (install!=gInstalled) {
        size_t changed=0;
        for (;changed<kHookCount;++changed) if (!Write(gHooks[changed],install?gHooks[changed].replacement:gHooks[changed].original)) break;
        if (changed<kHookCount) {
            bool restored=true;
            for (size_t i=changed+1;i;--i) {
                Hook& h=gHooks[i-1];
                restored=Write(h,install?h.original:h.replacement,true)&&restored;
            }
            if (!restored) { gRecovery=true;gCancelNotice=gProtect=false; }
            return false;
        }
        gInstalled=install;
    }
    gCancelNotice=cancelDialog;gProtect=protectPresentation;return true;
}
bool RestoreNoticePresentation() noexcept { return ConfigureNoticePresentation(false,false); }
bool NoticePresentationIdle() noexcept { ValidateFrames();return InterlockedCompareExchange(&gActive,0,0)==0; }
bool NoticePresentationAvailable() noexcept { return gPrepared&&!gRecovery; }
bool NoticePresentationRecoveryPending() noexcept { return gRecovery; }
void SuspendNoticePresentationForRecovery() noexcept { gCancelNotice=gProtect=false; }
#ifdef XA66_AUDIT
bool ReadNoticeHookAudit(size_t index, NoticeHookAudit& output) noexcept {
    if (!gPrepared || index >= kHookCount) return false;
    const Hook& hook = gHooks[index];
    output = {};
    output.address = hook.address - gBase + kOrigin;
    memcpy(output.original,hook.original,sizeof(output.original));
    output.length = static_cast<unsigned>(hook.length);
    output.relativeCallOffset = hook.relativeCallOffset;
    output.argumentBytes = hook.argumentBytes;
    return true;
}
bool NoticeStrategyNameAuditReady() noexcept { return gPrepared && gStrategyNameReady; }
#endif
#ifdef XA66_TEST
void NoticePresentationTestFailWrites(int forwardIndex,int rollbackIndex,bool afterCopy,bool failCache) noexcept {
    gFailForward=forwardIndex;gFailRollback=rollbackIndex;gForwardCount=gRollbackCount=0;
    gAfterCopy=afterCopy;gFailCache=failCache;
}
#endif
