#include "AnimationRuntime.h"
#include "Features.h"
#include "RPresentation.h"
#include "SPresentation.h"
#include "PresentationDiagnostics.h"
#include "InstallCommon.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <limits>
#include <intrin.h>

// The native entry points all establish EBP frames. Keep our observer frames
// linked as well, so an SEH/longjmp unwind can be distinguished from a live
// action before its next wait or screen presentation.
#pragma optimize("y", off)

namespace {
constexpr uintptr_t kOrigin = 0x400000;
constexpr size_t kMaximumHooks = 129;
constexpr size_t kMaximumDepth = 64;
constexpr size_t kMaximumSurfaces = 64;
struct Hook {
    uintptr_t address = 0;
    BYTE original[16] = {};
    BYTE replacement[16] = {};
    BYTE current[16] = {};
    size_t length = 0;
    void* allocation = nullptr;
    DWORD protection = 0;
    bool protectionKnown = false;
    bool protectionPending = false;
    bool cachePending = false;
    bool currentKnown = true;
};
struct Context {
    uintptr_t returnAddress = 0;
    uintptr_t returnSlot = 0;
    unsigned kind = 0;
    int camp = -1;
    bool rScene = false;
    bool suppress = false;
    unsigned kindMask = 0;
    unsigned campMask = 0;
    uintptr_t externalToken = 0;
    uintptr_t rOwner = 0;
    uintptr_t diagnostic = 0;
    uintptr_t ownerEntry = 0;
    uintptr_t mapScript = 0;
    ULONGLONG mapGeneration = 0;
    unsigned mapOpcode = 0;
    ULONGLONG scenarioGeneration = 0;
};
struct Pending {
    bool used = false;
    int surface = 0, left = 0, top = 0;
    int64_t right = 0, bottom = 0;
    uint64_t sequence = 0;
};
Hook gHooks[kMaximumHooks];
size_t gHookCount = 0;
EngineLayout gLayout;
uintptr_t gBase = 0;
bool gPrepared = false, gInstalled = false, gSuperS = false, gSuperR = false, gCancelDialog = false,
    gCancelMapText = false, gAllPresentation = false;
bool gRecoveryPending = false;
#if defined(XA66_TEST) || defined(ANIMATION_TEST_OBSERVER)
int gFailForward = -1, gFailRollback = -1;
int gForwardWrites = 0, gRollbackWrites = 0;
bool gAfterForwardCopy = false, gAfterRollbackCopy = false;
bool gFailCache = false;
#endif
int (*gScenePredicate)() = nullptr;
bool (*gMapIdentityPredicate)(uintptr_t,unsigned,ULONGLONG&) = nullptr;
ULONGLONG (*gScenarioGenerationProvider)() = nullptr;
volatile LONG gActive = 0;
using Present = int (__stdcall*)(int, int, int, int, int);
Present gPresent = nullptr;
thread_local Context gContexts[kMaximumDepth];
thread_local size_t gDepth = 0;
thread_local Pending gPending[kMaximumSurfaces];
thread_local uint64_t gSequence = 0;
thread_local bool gFlushing = false;
thread_local bool gNeutralDrawing = false;
thread_local uintptr_t gScopeSerial = 0;
using RCompose = void (__cdecl*)();
RCompose gRCompose = nullptr;
RCompose gRComposeAdvance = nullptr;
thread_local bool gRComposeDirty = false;
thread_local bool gRFinalizing = false;
thread_local bool gPreserveNativeChain = false;
thread_local ULONGLONG gObservedScenarioGeneration = 0;
thread_local bool gScenarioGenerationKnown = false;
thread_local bool gScenarioGenerationInvalidated = false;
extern "C" void AnimationExitBridge();

void ValidateScenarioGeneration() noexcept {
    if (!gScenarioGenerationProvider) return;
    const ULONGLONG current = gScenarioGenerationProvider();
    bool changed = gScenarioGenerationKnown && current != gObservedScenarioGeneration;
    for (size_t i = 0; i < gDepth; ++i)
        if (gContexts[i].scenarioGeneration != current) changed = true;
    gObservedScenarioGeneration = current; gScenarioGenerationKnown = true;
    if (!changed) return;
    gScenarioGenerationInvalidated = true;
    // A load can enter and exit entirely inside a still-live native frame.
    // Its new scene byte/ordinal may equal the old values. Keep that native
    // stack returning normally, but none of it may lend the old skip policy
    // to descendants or create a detached R token when its owner returns.
    for (size_t i = 0; i < gDepth; ++i) {
        gContexts[i].suppress = false;
        gContexts[i].mapGeneration = 0;
        gContexts[i].rOwner = 0;
    }
    gPreserveNativeChain = gDepth != 0;
    ResetRPresentation(); gRComposeDirty = false;
    // Old dirty regions belong to the previous scene; do not submit them to
    // a newly restored surface or run its old final-compose callback.
    for (auto& pending : gPending) pending = Pending();
}

bool LiveMapIdentity(const Context& context) noexcept {
    if (!context.mapGeneration || !gMapIdentityPredicate) return false;
    ULONGLONG generation = 0;
    return gMapIdentityPredicate(context.mapScript,context.mapOpcode,generation) &&
        generation == context.mapGeneration;
}
int PresentationScene() noexcept {
    const int scene = gScenePredicate ? gScenePredicate() : -1;
    if (scene >= 0) return scene;
    // State zero also belongs to title/menus. Only a live exact dispatcher
    // scope may supply the missing R-map identity, never the byte by itself.
    for (size_t i = gDepth; i; --i)
        if (LiveMapIdentity(gContexts[i - 1])) return 0;
    return -1;
}

bool Read(uintptr_t address, void* data, size_t length) noexcept {
    __try { memcpy(data, reinterpret_cast<const void*>(address), length); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Write(Hook& hook, const void* data, bool rollback = false) noexcept {
    bool failProtection = false;
    bool failCache = false;
#ifdef XA66_TEST
    int& count = rollback ? gRollbackWrites : gForwardWrites;
    const int failure = rollback ? gFailRollback : gFailForward;
    if (failure >= 0 && count++ == failure) {
        if (!(rollback ? gAfterRollbackCopy : gAfterForwardCopy)) return false;
        failCache = gFailCache;
        failProtection = !failCache;
    }
#else
    (void)rollback;
#endif
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(hook.address), hook.length, PAGE_EXECUTE_READWRITE, &old)) return false;
    if (!hook.protectionKnown) { hook.protection = old; hook.protectionKnown = true; }
    hook.protectionPending = true;
    bool result = false;
    __try { memcpy(reinterpret_cast<void*>(hook.address), data, hook.length); result = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    DWORD unused = 0;
    const BOOL restored = !failProtection &&
        VirtualProtect(reinterpret_cast<void*>(hook.address), hook.length, hook.protection, &unused);
    if (restored) hook.protectionPending = false;
    const BOOL flushed = !failCache &&
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(hook.address), hook.length);
    hook.cachePending = !flushed;
    hook.currentKnown = Read(hook.address, hook.current, hook.length);
    return result && restored && flushed && hook.currentKnown && !memcmp(hook.current, data, hook.length);
}
void Dword(BYTE*& cursor, uintptr_t value) noexcept {
    const uint32_t operand = static_cast<uint32_t>(value);
    memcpy(cursor, &operand, sizeof(operand)); cursor += sizeof(operand);
}
void Jump(BYTE* cursor, uintptr_t from, uintptr_t to) noexcept {
    cursor[0] = 0xE9;
    const uint32_t displacement = static_cast<uint32_t>(to - from - 5);
    memcpy(cursor + 1, &displacement, sizeof(displacement));
}
int ActorCamp(uintptr_t actor) noexcept {
    const uintptr_t unit = gBase + gLayout.animationUnitBase - kOrigin;
    if (!gLayout.animationUnitBase || actor < unit || (actor - unit) % 0x30 ||
        (actor - unit) / 0x30 >= 250) return -1;
    BYTE camp = 0xFF;
    WORD person = 0xFFFF;
    return Read(actor, &person, sizeof(person)) && person != 0xFFFF &&
        Read(actor + 5, &camp, 1) && camp < 3 ? camp : -1;
}
int PhaseCamp() noexcept {
    BYTE phase = 0xFF;
    return gLayout.phase && Read(gBase + gLayout.phase - kOrigin, &phase, 1) && phase < 3 ? phase : -1;
}
int PersonIdCamp(uintptr_t id) noexcept {
    if (!gLayout.animationUnitBase || id >= 0xFFFF) return -1;
    int result = -1;
    bool found = false;
    for (unsigned slot = 0; slot < 250; ++slot) {
        const uintptr_t unit = gBase + gLayout.animationUnitBase - kOrigin + slot * 0x30u;
        WORD person = 0xFFFF;
        if (!Read(unit, &person, sizeof(person))) return -1;
        if (person != id) continue;
        if (found) return -1; // Ambiguous duplicate actors retain native behavior.
        found = true;
        result = ActorCamp(unit);
    }
    return result;
}
int DialogueCamp(uintptr_t id) noexcept {
    if (id >= 0x400 && id <= 0x403) return PhaseCamp();
    uintptr_t slot = 250;
    if (gLayout.animationDialogueEncodedSlots && id >= 0x404) slot = id - 0x404;
    else {
        BYTE directSlots = 0;
        if (!gLayout.animationDialogueSlotFlag ||
            !Read(gBase + gLayout.animationDialogueSlotFlag - kOrigin, &directSlots, 1)) return -1;
        if (directSlots) slot = static_cast<WORD>(id);
        // The native dialogue mapper returns no unit in person-ID mode.
        // Both verified dialogue bodies then resolve IDs below 400 through
        // the person table. Match a single deployed unit, never the phase.
        else if (id < 0x400 && gLayout.animationPersonTable) return PersonIdCamp(id);
    }
    return slot < 250 ? ActorCamp(gBase + gLayout.animationUnitBase - kOrigin + slot * 0x30u) : -1;
}
int PersonCamp(uintptr_t actor) noexcept {
    uintptr_t table = 0;
    if (!gLayout.animationPersonTable ||
        !Read(gBase + gLayout.animationPersonTable - kOrigin, &table, sizeof(table)) ||
        !table || actor < table || (actor - table) % 0x48) return -1;
    return PersonIdCamp((actor - table) / 0x48);
}
bool EverySKindAndCampSelected() noexcept {
    if (!gAllPresentation) return false;
    for (bool kind : gFeatureSettings.animationKinds) if (!kind) return false;
    for (bool camp : gFeatureSettings.animationCamps) if (!camp) return false;
    return true;
}
bool Selected(unsigned kind, int camp, bool rScene) noexcept {
    if (gPreserveNativeChain) return false;
    const int scene = PresentationScene();
    if (rScene) return gSuperR && scene == 0;
    // With every super skip option and every S kind/camp selected, an
    // unprovable actor (hidden appearance, withdrawal, revive, script area
    // effect) must no longer leak presentation. Business and script parameter
    // consumption still execute inside the resulting native scope.
    if (scene == 1 && gSuperS && EverySKindAndCampSelected()) return true;
    const bool kindSelected = kind < static_cast<unsigned>(AnimationKind::Count) &&
        (gDepth ? (gContexts[0].kindMask & (1u << kind)) != 0 : gFeatureSettings.animationKinds[kind]);
    const bool campSelected = camp >= 0 && camp < 3 &&
        (gDepth ? (gContexts[0].campMask & (1u << camp)) != 0 : gFeatureSettings.animationCamps[camp]);
    return gSuperS && scene == 1 && kindSelected && campSelected;
}
bool NeutralizePhysicalSprite(uintptr_t actor, uintptr_t nativeReturn) noexcept {
    if (gNeutralDrawing || !gLayout.animationNeutralSprite || ActorCamp(actor) < 0) return false;
    const uintptr_t caller = nativeReturn - gBase + kOrigin;
    bool verifiedCaller = false;
    for (size_t i = 0; i < gLayout.animationPhysicalSpriteCallerCount; ++i)
        if (caller == gLayout.animationPhysicalSpriteCallers[i]) verifiedCaller = true;
    if (!verifiedCaller) return false;
    bool selected = false;
    for (size_t i = gDepth; i; --i) {
        const Context& context = gContexts[i - 1];
        if (context.externalToken && !context.suppress) return false;
        if (context.rScene || context.kind != static_cast<unsigned>(AnimationKind::Physical)) continue;
        selected = context.suppress;
        break;
    }
    if (!selected) return false;
    const int scene = PresentationScene();
    if (scene != 1) return false;
    // These two sprite routines only compose graphics. The native neutral
    // renderer preserves a complete board when excluded nested numbers/status
    // routines present the shared buffer. Merely omitting the sprite would
    // erase the unit after the native background redraw.
    gNeutralDrawing = true;
    __try {
        using Neutral = void (__thiscall*)(void*);
        reinterpret_cast<Neutral>(gBase + gLayout.animationNeutralSprite - kOrigin)(
            reinterpret_cast<void*>(actor));
    } __finally { gNeutralDrawing = false; }
    return true;
}
bool NeutralizeMapSprite(uintptr_t actor, uintptr_t* returnSlot, unsigned argumentBytes) noexcept {
    if (gNeutralDrawing || !gLayout.animationNeutralMapSprite || !gLayout.animationMapDraw) return false;
    uintptr_t argumentSlot = 0;
    if (argumentBytes == 24 && !Read(reinterpret_cast<uintptr_t>(returnSlot + 5),
        &argumentSlot, sizeof(argumentSlot))) return false;
    const unsigned slot = argumentBytes == 24 ? static_cast<BYTE>(argumentSlot) : static_cast<BYTE>(actor);
    if (slot >= 250 || ActorCamp(gBase + gLayout.animationUnitBase - kOrigin + slot * 0x30u) < 0) return false;
    uintptr_t nativeReturn = 0;
    int coordinates[2] = {};
    if (!Read(reinterpret_cast<uintptr_t>(returnSlot), &nativeReturn, sizeof(nativeReturn)) ||
        !Read(reinterpret_cast<uintptr_t>(returnSlot + 1), coordinates, sizeof(coordinates))) return false;
    bool verifiedCaller = false;
    for (size_t i = 0; i < gLayout.animationMapSpriteCallerCount; ++i)
        if (nativeReturn - gBase + kOrigin == gLayout.animationMapSpriteCallers[i]) verifiedCaller = true;
    bool scriptCaller = false;
    if (!verifiedCaller) {
        for (size_t i = 0; i < gLayout.animationScriptPhysicalSpriteCallerCount; ++i)
            if (nativeReturn - gBase + kOrigin == gLayout.animationScriptPhysicalSpriteCallers[i]) scriptCaller = true;
    }
    if ((!verifiedCaller && !scriptCaller) || (PresentationScene()) != 1) return false;
    const bool automaticStatus = gLayout.profile == EngineProfileId::Ccz66 &&
        AnimationPresentationOwnerActive(0x43CDDD);
    bool selected = false;
    for (size_t i = gDepth; i; --i) {
        const Context& context = gContexts[i - 1];
        if (context.externalToken && !context.suppress) return false;
        const bool automaticStatusAction = automaticStatus && context.ownerEntry == 0x45778F &&
            context.kind == static_cast<unsigned>(AnimationKind::Status);
        if (context.rScene || (context.kind != static_cast<unsigned>(AnimationKind::Physical) &&
            !automaticStatusAction)) continue;
        selected = context.suppress && (!scriptCaller || context.ownerEntry == 0x45778F);
        break;
    }
    if (!selected) return false;
    if (scriptCaller) {
        int dimensions[2] = {};
        if (argumentBytes != 24 || !Read(reinterpret_cast<uintptr_t>(returnSlot + 3),dimensions,sizeof(dimensions)) ||
            dimensions[0] != dimensions[1] || (dimensions[0] != 48 && dimensions[0] != 64)) return false;
        const int offset = (dimensions[0] - 48) / 2;
        if (coordinates[0] > (std::numeric_limits<int>::max)() - offset ||
            coordinates[1] > (std::numeric_limits<int>::max)() - offset) return false;
        coordinates[0] += offset; coordinates[1] += offset;
    }
    // Only replace the pure sprite composition, retaining the original map
    // controller and its background/number/status/cleanup calls. Calling the
    // full neutral unit renderer here would also consume random status RNG.
    gNeutralDrawing = true;
    bool rendered = false;
    __try {
        const uintptr_t idle = gBase + gLayout.animationNeutralMapSprite - kOrigin;
        uintptr_t sprite = 0;
        if (gLayout.profile == EngineProfileId::Ccz64)
            sprite = reinterpret_cast<uintptr_t (__cdecl*)(unsigned)>(idle)(slot);
        else sprite = reinterpret_cast<uintptr_t (__stdcall*)(unsigned)>(idle)(slot);
        if (sprite) {
            using Draw = void (__stdcall*)(int,int,int,int,unsigned,uintptr_t);
            reinterpret_cast<Draw>(gBase + gLayout.animationMapDraw - kOrigin)(
                coordinates[0], coordinates[1], 48, 48, slot, sprite);
            rendered = true;
        }
    } __finally { gNeutralDrawing = false; }
    return rendered;
}
void FlushPending() noexcept {
    ValidateScenarioGeneration();
    if (gFlushing || !gPresent) return;
    gFlushing = true;
    // Each surface keeps the complete union. Replay surfaces in their last-use
    // order, so a background surface cannot cover the final foreground board.
    for (;;) {
        size_t next = kMaximumSurfaces;
        for (size_t i = 0; i < kMaximumSurfaces; ++i)
            if (gPending[i].used && (next == kMaximumSurfaces ||
                gPending[i].sequence < gPending[next].sequence)) next = i;
        if (next == kMaximumSurfaces) break;
        const Pending pending = gPending[next];
        gPending[next].used = false;
        gPresent(pending.surface, pending.left, pending.top,
            static_cast<int>(pending.right - pending.left), static_cast<int>(pending.bottom - pending.top));
    }
    gFlushing = false;
}
bool Push(unsigned kind, int camp, bool rScene, uintptr_t returnAddress, uintptr_t returnSlot = 0,
    int explicitPolicy = -1, bool verifiedMap = false) noexcept {
    ValidateScenarioGeneration();
    if (gDepth == kMaximumDepth) {
        // An untracked nested entry must never inherit its parent's skip.
        // Disable the entire live chain until every tracked frame has returned.
        PreserveNativePresentationUntilIdle();
        return false;
    }
    // An explicitly excluded nested action must run its complete native path.
    // Flush earlier completed drawing before entering that native action.
    const bool suppress = !gPreserveNativeChain && (explicitPolicy < 0 ? Selected(kind, camp, rScene) :
        explicitPolicy != 0 && gInstalled && !gRecoveryPending &&
        (verifiedMap || PresentationScene() >= 0));
    unsigned kindMask = 0, campMask = 0;
    if (gDepth) { kindMask = gContexts[0].kindMask; campMask = gContexts[0].campMask; }
    else {
        for (unsigned i = 0; i < static_cast<unsigned>(AnimationKind::Count); ++i)
            if (gFeatureSettings.animationKinds[i]) kindMask |= 1u << i;
        for (unsigned i = 0; i < 3; ++i)
            if (gFeatureSettings.animationCamps[i]) campMask |= 1u << i;
    }
    if (!suppress && gDepth && gContexts[gDepth - 1].suppress) FlushPending();
    gContexts[gDepth++] = {returnAddress, returnSlot, kind, camp, rScene, suppress, kindMask, campMask};
    gContexts[gDepth - 1].scenarioGeneration = gScenarioGenerationProvider ? gScenarioGenerationProvider() : 0;
    InterlockedIncrement(&gActive);
    return true;
}
void FinalizeRCompose() noexcept {
    ValidateScenarioGeneration();
    if (!gRComposeDirty || gRFinalizing || !gRCompose) return;
    if ((PresentationScene()) != 0) {
        gRComposeDirty = false; ResetRPresentation(); return;
    }
    if (RPresentationAsyncActive()) return;
    for (size_t i = 0; i < gDepth; ++i)
        if (gContexts[i].rScene && gContexts[i].suppress) return;
    gRComposeDirty = false; gRFinalizing = true;
    __try { gRCompose(); } __finally { gRFinalizing = false; }
}
uintptr_t Pop() noexcept {
    ValidateScenarioGeneration();
    if (!gDepth) return 0;
    const Context completed = gContexts[--gDepth];
    const uintptr_t result = completed.returnAddress;
    EndRPresentationOwner(completed.rOwner);
    InterlockedDecrement(&gActive);
    if (!gDepth || !gContexts[gDepth - 1].suppress) FlushPending();
    FinalizeRCompose();
    EndPresentationDiagnostic(completed.diagnostic);
    if (!gDepth) gPreserveNativeChain = false;
    return result;
}
void ValidateLiveFrames() noexcept {
    if (!gDepth || !gContexts[gDepth - 1].returnSlot) return;
    uintptr_t frame = 0;
    __asm mov frame, ebp
    const uintptr_t stackLow = __readfsdword(8);
    const uintptr_t stackHigh = __readfsdword(4);
    size_t liveDepth = gDepth;
    // Search from the current frame toward the native callers. Every tracked
    // native entry has an independently verified push-ebp/mov-ebp-esp prolog.
    // A saved return word alone is insufficient: unwind leaves old words on
    // the stack, sometimes until after another, deeper action has started.
    while (liveDepth) {
        const uintptr_t slot = gContexts[liveDepth - 1].returnSlot;
        if (!slot) return; // Unit-test-only scopes have no native frame.
        uintptr_t actualReturn = 0;
        bool found = false;
        uintptr_t cursor = frame;
        for (unsigned hops = 0; hops < 2048 && cursor >= stackLow &&
            cursor < stackHigh - 2 * sizeof(uintptr_t) && !(cursor & 3); ++hops) {
            if (cursor + sizeof(uintptr_t) == slot) { found = true; break; }
            uintptr_t parent = 0;
            if (!Read(cursor, &parent, sizeof(parent)) || parent <= cursor) break;
            cursor = parent;
        }
        const bool readable = Read(slot, &actualReturn, sizeof(actualReturn));
        if (found && readable && (gContexts[liveDepth - 1].externalToken ||
            actualReturn == reinterpret_cast<uintptr_t>(&AnimationExitBridge))) break;
        if (!gContexts[liveDepth - 1].externalToken && readable &&
            actualReturn == reinterpret_cast<uintptr_t>(&AnimationExitBridge)) {
            // An opaque/FPO intermediary may hide a still-live native frame.
            // Cancel our return replacement before removing its shadow record:
            // that action will now return through its original native address.
            // This also removes stale bridge words left behind by an unwind.
            bool restored = false;
            __try {
                *reinterpret_cast<uintptr_t*>(slot) = gContexts[liveDepth - 1].returnAddress;
                restored = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (!restored) {
                for (size_t i = 0; i < gDepth; ++i) gContexts[i].suppress = false;
                FlushPending();
                return;
            }
        }
        --liveDepth;
    }
    if (liveDepth != gDepth) {
        // Unwound owners must never create asynchronous tokens from stale
        // stack scopes or leave a native child policy active.
        ResetRPresentation();
        for (size_t i = gDepth; i > liveDepth; --i)
            EndPresentationDiagnostic(gContexts[i - 1].diagnostic,"unwind");
        InterlockedExchangeAdd(&gActive, -static_cast<LONG>(gDepth - liveDepth));
        gDepth = liveDepth;
        if (!gDepth) gPreserveNativeChain = false;
        FlushPending();
    }
}
void ValidateLiveScene() noexcept {
    ValidateScenarioGeneration();
    ValidateLiveFrames();
    for (size_t i = 0; i < gDepth; ++i)
        if (gContexts[i].mapGeneration && !LiveMapIdentity(gContexts[i])) {
            // Loading, same-ordinal reinstall, changed opcode or context ends
            // this ownership even if another scene happens to be readable.
            for (size_t child = i; child < gDepth; ++child) gContexts[child].suppress = false;
            gContexts[i].mapGeneration = 0;
        }
    const int scene = PresentationScene();
    if (scene != 0) { ResetRPresentation(); gRComposeDirty = false; }
    if (!gDepth) return;
    if (scene >= 0 && (gContexts[gDepth - 1].rScene ? scene == 0 : scene == 1)) return;
    for (size_t i = 0; i < gDepth; ++i) gContexts[i].suppress = false;
    FlushPending();
}

extern "C" uintptr_t __cdecl AnimationLeave() noexcept { return Pop(); }
extern "C" __declspec(naked) void AnimationExitBridge() {
    __asm {
        pushfd
        pushad
        call AnimationLeave
        mov [esp + 12], eax
        popad
        popfd
        push dword ptr [esp - 24]
        ret
    }
}
extern "C" int __cdecl AnimationEnter(unsigned index, uintptr_t actor, uintptr_t* returnSlot) noexcept {
    if (index >= gLayout.animationEntryCount || !returnSlot) return 0;
    ValidateLiveScene();
    // A native SEH/longjmp may bypass an exit bridge. A later entry at the same
    // or shallower stack position proves those frames have been unwound.
    while (gDepth && gContexts[gDepth - 1].returnSlot &&
        reinterpret_cast<uintptr_t>(returnSlot) >= gContexts[gDepth - 1].returnSlot) {
        EndPresentationDiagnostic(gContexts[gDepth - 1].diagnostic,"unwind");
        --gDepth; InterlockedDecrement(&gActive);
    }
    if (!gDepth) FlushPending();
    const AnimationEntryLayout& entry = gLayout.animationEntries[index];
    const int scene = PresentationScene();
    ULONGLONG mapGeneration = 0;
    uintptr_t mapScript = 0;
    unsigned mapOpcode = entry.address == 0x417143 ? 0x29u : entry.address == 0x4171BC ? 0x2Au : 0x2Bu;
    bool verifiedMap = false;
    if (scene < 0 && entry.rPresentationRole == RPresentationMapPortraitCommand &&
        gLayout.rCommandReturn && gLayout.rScriptContext && gMapIdentityPredicate) {
        uintptr_t caller = 0;
        verifiedMap = Read(reinterpret_cast<uintptr_t>(returnSlot),&caller,sizeof(caller)) &&
            caller - gBase + kOrigin == gLayout.rCommandReturn &&
            Read(reinterpret_cast<uintptr_t>(returnSlot + 1),&mapScript,sizeof(mapScript)) &&
            mapScript == gBase + gLayout.rScriptContext - kOrigin &&
            gMapIdentityPredicate(mapScript,mapOpcode,mapGeneration) && mapGeneration;
    }
    if (entry.rPresentationRole == RPresentationReset) { ResetRPresentation(); gRComposeDirty = false; return 0; }
    if (entry.rPresentationRole == RPresentationObjectReset) { ClearRPresentationObject(actor); return 0; }
    if (entry.rPresentationRole == RPresentationCompose) {
        // A native notification/interaction child overrides an R parent or
        // asynchronous object. Pure composition contains no route/business
        // advancement; the real dispatcher still executes every native step.
        const bool suppress = gDepth ? gContexts[gDepth - 1].suppress :
            (scene == 0 && RPresentationAsyncActive());
        if (!gRFinalizing && scene == 0 && suppress) {
            RecordPresentationCompose(true);
            if (!gDepth) RecordRPresentationAsyncCompose();
            // Native 6.6 embeds graphic-object lifetime advancement inside its
            // composer. Preserve that update while omitting the drawing work.
            if (gRComposeAdvance) gRComposeAdvance();
            gRComposeDirty = true; return 1;
        }
        gRComposeDirty = false;
        RecordPresentationCompose(false);
        return 0;
    }
    if (entry.dialogueRouter && gCancelDialog) return 1;
    // The real ending controller retains its resources, audio, text cursor,
    // backing-store cleanup and return. Its pure text/blit descendants alone
    // omit pixels. An excluded child or unknown caller stays fully native.
    if (entry.rPresentationRole == RPresentationEndingText ||
        entry.rPresentationRole == RPresentationPortraitBlit) {
        bool endingOwner = false;
        if (scene == 0 && gDepth && gContexts[gDepth - 1].suppress)
            for (size_t i = gDepth; i; --i)
                if (gContexts[i - 1].ownerEntry == (gLayout.profile == EngineProfileId::Ccz66 ? 0x4317ABu : 0x430470u)) {
                    endingOwner = true; break;
                }
        if (endingOwner) return 1;
        if (entry.rPresentationRole == RPresentationEndingText) return 0;
    }
    if (entry.rPresentationRole == RPresentationStaticNumbers || entry.rPresentationRole == RPresentationStaticIllustration ||
        entry.rPresentationRole == RPresentationEndingPerson) {
        // Static R overlays remain native objects, but their pure visible
        // leaves are independently omitted. Unknown/shared callers stay native
        // without adding a synthetic owner or changing lifetime advancement.
        return ShouldSkipRPresentationEntry(gLayout,gBase,entry,actor,returnSlot,
            AnimationPresentationSelected(AnimationPresentationCategory::RAction) &&
            (!gDepth || gContexts[gDepth - 1].suppress)) ? 1 : 0;
    }
    if (entry.rPresentationRole == RPresentationPortraitBlit &&
        OmitSPresentationDuelBlit(gLayout,gBase,entry,returnSlot,scene)) return 1;
    if (entry.rPresentationRole == RPresentationPortraitBlit &&
        OmitSPresentationTransientBlit(gLayout,gBase,entry,returnSlot,scene)) return 1;
    if (entry.rPresentationRole == RPresentationPortraitBlit || entry.rPresentationRole == RPresentationPortraitFade) {
        uintptr_t caller = 0;
        // Resource loading, backing-store capture and later disappearance all
        // execute in their native owners. Only the final portrait pixels have
        // this exact return identity; all other users of the shared blitter run.
        if (!Read(reinterpret_cast<uintptr_t>(returnSlot),&caller,sizeof(caller))) return 0;
        caller = caller - gBase + kOrigin;
        const bool exact = entry.rPresentationRole == RPresentationPortraitBlit ? caller == 0x432E4F :
            caller == 0x432E33 || caller == (gLayout.profile == EngineProfileId::Ccz66 ? 0x403CEBu : 0x403D12u);
        if (!exact || !gDepth ||
            !gContexts[gDepth - 1].suppress) return 0;
        for (size_t i = gDepth; i; --i) {
            const Context& owner = gContexts[i - 1];
            if (owner.ownerEntry == 0x417143 || owner.ownerEntry == 0x4171BC || owner.ownerEntry == 0x4172EC)
                return AnimationPresentationSelected(AnimationPresentationCategory::MapText) ? 1 : 0;
        }
        return 0;
    }
    if (entry.mapPhysicalSprite) {
        if (OmitSPresentationSprite(gLayout,gBase,entry,returnSlot)) return 1;
        return NeutralizeMapSprite(actor, returnSlot, entry.argumentBytes) ? 1 : 0;
    }
    if (entry.physicalSprite) {
        uintptr_t nativeReturn = 0;
        return Read(reinterpret_cast<uintptr_t>(returnSlot), &nativeReturn, sizeof(nativeReturn)) &&
            NeutralizePhysicalSprite(actor, nativeReturn) ? 1 : 0;
    }
    int camp = -1;
    uintptr_t argument = 0;
    BYTE slot = 0xFF;
    if (entry.actorSource == 9) camp = gDepth ? gContexts[gDepth - 1].camp : PersonCamp(actor);
    else if (entry.actorSource == 8 && Read(reinterpret_cast<uintptr_t>(returnSlot + 2), &argument, sizeof(argument)))
        camp = DialogueCamp(argument);
    else if (entry.actorSource == 1) camp = ActorCamp(actor);
    else if (entry.actorSource == 2 && Read(actor, &slot, 1) && slot < 250) {
        const uintptr_t unit = gBase + gLayout.animationUnitBase - kOrigin + slot * 0x30u;
        uintptr_t controllerUnit = 0;
        if (Read(actor + 0x0C, &controllerUnit, sizeof(controllerUnit)) && controllerUnit == unit)
            camp = ActorCamp(unit);
    }
    else if (entry.actorSource == 7 && gDepth) camp = gContexts[gDepth - 1].camp;
    else if ((entry.actorSource == 3 || entry.actorSource == 4 || entry.actorSource == 6 ||
        entry.actorSource == 7) && Read(reinterpret_cast<uintptr_t>(returnSlot +
            (entry.actorSource == 6 ? 2 : 1)), &argument, sizeof(argument)))
        camp = ActorCamp(entry.actorSource == 3 ? argument :
            (argument < 250 ? gBase + gLayout.animationUnitBase - kOrigin + argument * 0x30u : 0));
    else if (entry.actorSource == 5) camp = PhaseCamp();
    else if (entry.actorSource == 0 && gDepth) camp = gContexts[gDepth - 1].camp;
    unsigned kind = entry.kind;
    bool rScene = entry.rScene;
    ResolveSPresentationEntryPolicy(gLayout,gBase,entry,actor,returnSlot,kind,camp,rScene,scene);
    int explicitPolicy = ResolveSPresentationCategoryPolicy(gLayout,gBase,entry,returnSlot,scene,kind,camp);
    if (entry.rPresentationRole == RPresentationEndingOwner) {
        rScene = true;
        explicitPolicy = scene == 0 && RPresentationEndingCaller(gLayout,gBase,returnSlot) &&
            AnimationPresentationSelected(AnimationPresentationCategory::RAction) ? 1 : 0;
    }
    const bool portrait = entry.rPresentationRole == RPresentationMapPortraitCommand ||
        entry.rPresentationRole == RPresentationPortraitMove;
    if (portrait) {
        // Scene identity guards a live scope against scene changes; category
        // selection is independent and is supplied explicitly below.
        rScene = scene == 0 || verifiedMap;
        explicitPolicy = verifiedMap ? (gCancelMapText ? 1 : 0) :
            (AnimationPresentationSelected(AnimationPresentationCategory::MapText) ? 1 : 0);
    }
    // Complete skip mode: owners without an independent category policy (the
    // unresolved kind/camp case) establish a suppressed native scope. Owners
    // that explicitly returned native for an unknown shared caller keep that
    // protection. Pure leaves still pass their exact caller/ABI checks and
    // mixed handlers still consume parameters and run business.
    if (explicitPolicy < 0 && AnimationPresentationAllSelected()) explicitPolicy = 1;
    const bool selected = explicitPolicy < 0 ? Selected(kind,camp,rScene) : explicitPolicy != 0;
    if (entry.rPresentationRole == RPresentationCommand && !entry.sPresentationRole && scene != 0) return 0;
    if (ShouldSkipRPresentationEntry(gLayout,gBase,entry,actor,returnSlot,
        selected && (portrait || !gDepth || gContexts[gDepth - 1].suppress))) {
        RecordPresentationCompose(true);
        return 1;
    }
    if (ShouldSkipSPresentationEntry(gLayout,gBase,entry,returnSlot,selected) &&
        OmitSPresentationEntry(gLayout,gBase,entry,returnSlot)) return 1;
    // An excluded range leaf must establish a native child scope: the action
    // ancestor can still be selected while the player is choosing a target.
    // Returning without a scope would let AnimationPresent hide that preview.
    if (entry.sPresentationRole == SPresentationEffectFrame ||
        entry.sPresentationRole == SPresentationCastingMask || entry.sPresentationRole == SPresentationPanelGlyph ||
        entry.sPresentationRole == SPresentationAutomaticStatusTile ||
        entry.sPresentationRole == SPresentationAutomaticStatusGearPanel ||
        entry.sPresentationRole == SPresentationAutomaticStatusGearWait ||
        (entry.sPresentationRole == SPresentationAutomaticStatusTextOwner && !selected))
        return 0;
    if (entry.numberPresentation) {
        uintptr_t nativeReturn = 0;
        bool verifiedCaller = false;
        if (Read(reinterpret_cast<uintptr_t>(returnSlot), &nativeReturn, sizeof(nativeReturn)))
            for (size_t i = 0; i < gLayout.animationNumberPresentationCallerCount; ++i)
                if (nativeReturn - gBase + kOrigin == gLayout.animationNumberPresentationCallers[i]) verifiedCaller = true;
        if (verifiedCaller && Selected(kind, camp, rScene)) return 1;
        // Excluded/unknown Numbers must establish a native child scope. Simply
        // continuing without one would inherit a selected Physical parent's
        // presentation gate and accidentally suppress the excluded numbers.
        if (!verifiedCaller) camp = -1;
    }
    uintptr_t previous = 0;
    if (Read(reinterpret_cast<uintptr_t>(returnSlot), &previous, sizeof(previous)) &&
        Push(kind, camp, rScene, previous, reinterpret_cast<uintptr_t>(returnSlot),explicitPolicy,verifiedMap)) {
        Context& context = gContexts[gDepth - 1];
        if (verifiedMap) {
            context.mapScript = mapScript; context.mapOpcode = mapOpcode; context.mapGeneration = mapGeneration;
        }
        context.ownerEntry = entry.address;
        context.diagnostic = BeginPresentationDiagnostic(rScene ? 1u : 2u,entry.address,kind,camp,
            context.suppress,rScene && gLayout.rObjectBase ? gBase + gLayout.rObjectBase - kOrigin : actor,
            rScene && gLayout.rObjectBase ? 128 * 0x40 : ActorCamp(actor) >= 0 ? 0x30 : 0);
        context.rOwner = BeginRPresentationOwner(entry.rPresentationRole,context.suppress);
        *returnSlot = reinterpret_cast<uintptr_t>(&AnimationExitBridge);
    }
    return 0;
}
extern "C" int __stdcall AnimationPresentBridge(int surface, int left, int top, int width, int height) noexcept {
    ValidateLiveScene();
    const int64_t right = static_cast<int64_t>(left) + width;
    const int64_t bottom = static_cast<int64_t>(top) + height;
    if (!gFlushing && !gRFinalizing && gDepth && gContexts[gDepth - 1].suppress && surface >= 0 &&
        surface < static_cast<int>(kMaximumSurfaces) && width > 0 && height > 0 &&
        right <= (std::numeric_limits<int>::max)() && bottom <= (std::numeric_limits<int>::max)()) {
        Pending& pending = gPending[surface];
        const int64_t combinedWidth = (std::max)(pending.used ? pending.right : right, right) -
            (std::min)(pending.used ? pending.left : left, left);
        const int64_t combinedHeight = (std::max)(pending.used ? pending.bottom : bottom, bottom) -
            (std::min)(pending.used ? pending.top : top, top);
        if (combinedWidth > (std::numeric_limits<int>::max)() ||
            combinedHeight > (std::numeric_limits<int>::max)()) {
            FlushPending();
        }
        if (!pending.used) pending = {true, surface, left, top, right, bottom, ++gSequence};
        else {
            pending.left = (std::min)(pending.left, left);
            pending.top = (std::min)(pending.top, top);
            pending.right = (std::max)(pending.right, right);
            pending.bottom = (std::max)(pending.bottom, bottom);
            pending.sequence = ++gSequence;
        }
        RecordPresentationSubmit(true);
        return 0;
    }
    if (!gFlushing) FlushPending();
    RecordPresentationSubmit(false);
    return gPresent ? gPresent(surface, left, top, width, height) : 0;
}
bool PrepareHook(Hook& hook, uintptr_t address, const BYTE* original, size_t length,
    uintptr_t target, size_t index, bool entry, unsigned char relativeCallOffset = 0xFF) noexcept {
    if (!address || length < 5 || length > sizeof(hook.original)) return false;
    hook.address = gBase + address - kOrigin;
    hook.length = length;
    if (!Read(hook.address, hook.original, length) || memcmp(hook.original, original, length)) return false;
    memcpy(hook.current, hook.original, length);
    MEMORY_BASIC_INFORMATION memory = {};
    if (!VirtualQuery(reinterpret_cast<const void*>(hook.address), &memory, sizeof(memory))) return false;
    hook.protection = memory.Protect; hook.protectionKnown = true;
    hook.allocation = VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!hook.allocation) return false;
    BYTE* trampoline = static_cast<BYTE*>(hook.allocation);
    memcpy(trampoline, hook.original, length);
    if (relativeCallOffset != 0xFF) {
        if (static_cast<size_t>(relativeCallOffset) + 5 > length || original[relativeCallOffset] != 0xE8) return false;
        int32_t originalDisplacement = 0;
        memcpy(&originalDisplacement, original + relativeCallOffset + 1, sizeof(originalDisplacement));
        const uintptr_t destination = hook.address + relativeCallOffset + 5 + originalDisplacement;
        const uint32_t replacementDisplacement = static_cast<uint32_t>(destination -
            reinterpret_cast<uintptr_t>(trampoline + relativeCallOffset + 5));
        memcpy(trampoline + relativeCallOffset + 1, &replacementDisplacement, sizeof(replacementDisplacement));
    }
    Jump(trampoline + length, reinterpret_cast<uintptr_t>(trampoline + length), hook.address + length);
    BYTE* cursor = trampoline + 24;
    uintptr_t replacement = target;
    if (entry) {
        replacement = reinterpret_cast<uintptr_t>(cursor);
        *cursor++ = 0x9C; *cursor++ = 0x60; // Preserve native flags and registers.
        const BYTE stack[] = {0x8D,0x44,0x24,0x24,0x8B,0x54,0x24,0x18,0x50,0x52,0x68};
        memcpy(cursor, stack, sizeof(stack)); cursor += sizeof(stack);
        Dword(cursor, index);
        *cursor++ = 0xB8; Dword(cursor, reinterpret_cast<uintptr_t>(&AnimationEnter));
        *cursor++ = 0xFF; *cursor++ = 0xD0;
        *cursor++ = 0x83; *cursor++ = 0xC4; *cursor++ = 0x0C;
        // Only independently verified presentation leaves may return early.
        // Every action scope containing business logic runs its native body.
        if (gLayout.animationEntries[index].dialogueRouter || gLayout.animationEntries[index].physicalSprite ||
            gLayout.animationEntries[index].mapPhysicalSprite || gLayout.animationEntries[index].numberPresentation ||
            gLayout.animationEntries[index].sPresentationRole ||
            gLayout.animationEntries[index].rPresentationRole == RPresentationPortraitMove ||
            gLayout.animationEntries[index].rPresentationRole == RPresentationPortraitBlit ||
            gLayout.animationEntries[index].rPresentationRole == RPresentationStaticNumbers ||
            gLayout.animationEntries[index].rPresentationRole == RPresentationStaticIllustration ||
            gLayout.animationEntries[index].rPresentationRole == RPresentationPortraitFade ||
            gLayout.animationEntries[index].rPresentationRole == RPresentationEndingPerson ||
            gLayout.animationEntries[index].rPresentationRole == RPresentationEndingText ||
            gLayout.animationEntries[index].rPresentationRole == RPresentationCompose) {
            if (gLayout.animationEntries[index].rPresentationRole == RPresentationEndingPerson) {
                // This pure renderer has a meaningful result: valid entries
                // return zero, while native -1 sentinel handling runs above.
                const BYTE skip[]={0x85,0xC0,0x74,7,0x61,0x9D,0x33,0xC0,0xC2,4,0};
                memcpy(cursor,skip,sizeof(skip));cursor+=sizeof(skip);
            } else {
                BYTE skip[] = {0x85,0xC0,0x74,0x05,0x61,0x9D,0xC2,0x0C,0};
                if (gLayout.animationEntries[index].physicalSprite || gLayout.animationEntries[index].mapPhysicalSprite ||
                    gLayout.animationEntries[index].numberPresentation || gLayout.animationEntries[index].sPresentationRole ||
                    gLayout.animationEntries[index].rPresentationRole == RPresentationPortraitMove ||
                    gLayout.animationEntries[index].rPresentationRole == RPresentationPortraitBlit ||
                    gLayout.animationEntries[index].rPresentationRole == RPresentationStaticNumbers ||
                    gLayout.animationEntries[index].rPresentationRole == RPresentationStaticIllustration ||
                    gLayout.animationEntries[index].rPresentationRole == RPresentationPortraitFade ||
                    gLayout.animationEntries[index].rPresentationRole == RPresentationEndingText ||
                    gLayout.animationEntries[index].rPresentationRole == RPresentationCompose)
                    skip[7] = gLayout.animationEntries[index].argumentBytes;
                memcpy(cursor, skip, sizeof(skip)); cursor += sizeof(skip);
            }
        }
        *cursor++ = 0x61; *cursor++ = 0x9D;
        *cursor++ = 0x68; Dword(cursor, reinterpret_cast<uintptr_t>(trampoline)); *cursor++ = 0xC3;
    }
    memset(hook.replacement, 0x90, length);
    Jump(hook.replacement, hook.address, replacement);
    return true;
}
void FreeHooks() noexcept {
    for (Hook& hook : gHooks) {
        if (hook.allocation) VirtualFree(hook.allocation, 0, MEM_RELEASE);
        hook = Hook();
    }
    gHookCount = 0; gPresent = nullptr; gPrepared = false;
    gRCompose = nullptr; gRComposeAdvance = nullptr; gRComposeDirty = false; ResetRPresentation();
}
}

bool PrepareAnimationRuntime(const EngineLayout& layout, uintptr_t gameBase) noexcept {
    if (!RestoreAnimationRuntime()) return false;
    FreeHooks(); gLayout = layout; gBase = gameBase;
    if (!gameBase || !layout.animationPresent || !layout.animationEntryCount ||
        layout.animationEntryCount >= kMaximumHooks) return false;
    if (!PrepareHook(gHooks[0], layout.animationPresent, layout.animationPresentOriginal,
        layout.animationPresentLength, reinterpret_cast<uintptr_t>(&AnimationPresentBridge), 0, false)) {
        FreeHooks(); return false;
    }
    gPresent = reinterpret_cast<Present>(gHooks[0].allocation);
    gHookCount = 1;
    for (size_t i = 0; i < layout.animationEntryCount; ++i) {
        const auto& entry = layout.animationEntries[i];
        if (!PrepareHook(gHooks[gHookCount], entry.address, entry.original, entry.length, 0, i, true,
            entry.relativeCallOffset)) {
            FreeHooks(); return false;
        }
        if (entry.rPresentationRole == RPresentationCompose)
            gRCompose = layout.rFinalRefresh ? reinterpret_cast<RCompose>(gameBase + layout.rFinalRefresh - kOrigin) :
                reinterpret_cast<RCompose>(gHooks[gHookCount].allocation);
        ++gHookCount;
    }
    gRComposeAdvance = layout.rComposeAdvance ? reinterpret_cast<RCompose>(gameBase + layout.rComposeAdvance - kOrigin) : nullptr;
    ConfigureRPresentation(layout,gameBase,false);
    gPrepared = true; return true;
}
bool ConfigureAnimationRuntime(bool superS, bool superR, bool cancelDialog, bool cancelMapText,
    bool allPresentation) noexcept {
    if (!AnimationRuntimeIdle()) return false;
    const bool install = superS || superR || cancelDialog || cancelMapText;
    allPresentation = allPresentation && superS && superR && cancelDialog && cancelMapText;
    if (gRecoveryPending) {
        if (install) return false;
        // Partial hooks must keep their trampolines alive. Recover only bytes
        // observed after our own writes; never overwrite a later foreign edit.
        bool restored = true;
        for (size_t i = gHookCount; i; --i) {
            Hook& hook = gHooks[i - 1];
            BYTE actual[16] = {};
            if (!Read(hook.address, actual, hook.length) ||
                (hook.currentKnown ? memcmp(actual, hook.current, hook.length) != 0 :
                memcmp(actual, hook.original, hook.length) != 0 && memcmp(actual, hook.replacement, hook.length) != 0)) {
                restored = false;
                continue;
            }
            if (memcmp(actual, hook.original, hook.length) || hook.protectionPending || hook.cachePending)
                restored = Write(hook, hook.original) && restored;
        }
        if (!restored) return false;
        gInstalled = gRecoveryPending = false;
    }
    // A completed detach owns no game bytes. A later preparation may inspect
    // another image or a legitimately relocated baseline.
    if (!install && !gInstalled) {
        gSuperS = gSuperR = gCancelDialog = gCancelMapText = false;
        gAllPresentation = false;
        return true;
    }
    if (install && !gPrepared) return false;
    if (cancelDialog && !AnimationRuntimeDialogAvailable()) return false;
    if (cancelMapText && !AnimationRuntimeMapTextAvailable()) return false;
    for (size_t i = 0; i < gHookCount; ++i) {
        const Hook& hook = gHooks[i];
        BYTE actual[16] = {};
        if (!Read(hook.address, actual, hook.length) || !hook.currentKnown ||
            memcmp(actual, hook.current, hook.length)) {
            if (gInstalled) {
                gRecoveryPending = true;
                gSuperS = gSuperR = gCancelDialog = gCancelMapText = false;
                gAllPresentation = false;
            }
            return false;
        }
    }
    if (install != gInstalled) {
        size_t changed = 0;
        bool attempted = false;
        for (; changed < gHookCount; ++changed) {
            Hook& hook = gHooks[changed];
            attempted = true;
            if (!Write(hook, install ? hook.replacement : hook.original)) break;
        }
        if (changed != gHookCount) {
            bool rolledBack = true;
            // Include the failed write: it may have copied its bytes and then
            // failed to flush or restore protection.
            size_t undo = changed + (attempted ? 1u : 0u);
            while (undo) {
                Hook& hook = gHooks[--undo];
                rolledBack = Write(hook, gInstalled ? hook.replacement : hook.original, true) && rolledBack;
            }
            if (!rolledBack) {
                gRecoveryPending = true;
                gSuperS = gSuperR = gCancelDialog = gCancelMapText = false;
                gAllPresentation = false;
            }
            return false;
        }
        gInstalled = install;
    }
    gSuperS = superS; gSuperR = superR; gCancelDialog = cancelDialog; gCancelMapText = cancelMapText;
    gAllPresentation = allPresentation;
    ConfigureRPresentation(gLayout,gBase,superR);
    if (!superR) FinalizeRCompose();
    return true;
}
bool RestoreAnimationRuntime() noexcept {
    if (!ConfigureAnimationRuntime(false, false)) return false;
    FlushPending(); return true;
}
bool AnimationRuntimeRecoveryPending() noexcept { return gRecoveryPending; }
void SuspendAnimationRuntimeForRecovery() noexcept {
    gSuperS = gSuperR = gCancelDialog = gCancelMapText = false;
    gAllPresentation = false;
    if (gInstalled || gRecoveryPending) gRecoveryPending = true;
}
bool AnimationRuntimeAvailable() noexcept { return gPrepared; }
bool AnimationRuntimeSupportsS() noexcept {
    if (!gPrepared || !gLayout.sPresentationVerified ||
        (gLayout.sPresentationPathMask & SPathRequired) != SPathRequired ||
        !gLayout.animationOrdinaryPhysical || !gLayout.animationNeutralSprite ||
        !gLayout.animationNeutralMapSprite || !gLayout.animationMapDraw || !gLayout.animationWaitRouterReturn ||
        gLayout.animationNumberPresentationCallerCount != 3 ||
        gLayout.animationPhysicalSpriteCallerCount != 7) return false;
    // The explicit per-path S audit supplements, rather than replaces, the
    // ordinary native controllers and safe physical/number renderers.
    bool ordinaryKinds[6] = {};
    for (size_t i = 0; i < gLayout.animationEntryCount; ++i) {
        const auto& entry = gLayout.animationEntries[i];
        if (!entry.rScene && !entry.sPresentationRole && !entry.physicalSprite &&
            !entry.mapPhysicalSprite && entry.kind < _countof(ordinaryKinds)) ordinaryKinds[entry.kind] = true;
    }
    for (bool kind : ordinaryKinds) if (!kind) return false;
    return true;
}
bool AnimationRuntimeSupportsR() noexcept {
    return gPrepared && gLayout.rPresentationVerified && gRCompose &&
        gLayout.rObjectBase && gLayout.rAsyncWaitReturn && gLayout.rFinalRefresh;
}
bool AnimationRuntimeDialogAvailable() noexcept {
    if (!gPrepared) return false;
    for (size_t i = 0; i < gLayout.animationEntryCount; ++i)
        if (gLayout.animationEntries[i].dialogueRouter) return true;
    return false;
}
bool AnimationRuntimeMapTextAvailable() noexcept {
    if (!gPrepared || !gLayout.rPresentationVerified) return false;
    bool commands[3] = {}, move = false, blit = false, fade = false;
    const uintptr_t owners[] = {0x417143,0x4171BC,0x4172EC};
    for (size_t i = 0; i < gLayout.animationEntryCount; ++i) {
        const auto& entry = gLayout.animationEntries[i];
        if (entry.rPresentationRole == RPresentationMapPortraitCommand)
            for (size_t j = 0; j < _countof(owners); ++j)
                if (entry.address == owners[j]) commands[j] = true;
        move = move || entry.rPresentationRole == RPresentationPortraitMove;
        blit = blit || entry.rPresentationRole == RPresentationPortraitBlit;
        fade = fade || entry.rPresentationRole == RPresentationPortraitFade;
    }
    return commands[0] && commands[1] && commands[2] && move && blit && fade;
}
bool AnimationRuntimeIdle() noexcept {
    ValidateScenarioGeneration();
    ValidateLiveFrames();
    return InterlockedCompareExchange(&gActive, 0, 0) == 0;
}
uintptr_t BeginAnimationPresentationScope(bool suppress) noexcept {
    ValidateLiveScene();
    const int scene = PresentationScene();
    if (!Push(static_cast<unsigned>(AnimationKind::Count), -1, scene == 0, 0, 0, suppress ? 1 : 0)) return 0;
    Context& context = gContexts[gDepth - 1];
    // This policy belongs to the concrete child presentation, independently
    // of the parent's kind/camp. Failed installation/recovery stays native.
    if (!++gScopeSerial) ++gScopeSerial;
    context.externalToken = gScopeSerial;
    return context.externalToken;
}
uintptr_t BeginAnimationPresentationScopeForFrame(bool suppress, uintptr_t nativeReturnSlot) noexcept {
    if (!nativeReturnSlot || (nativeReturnSlot & 3)) return 0;
    const uintptr_t token = BeginAnimationPresentationScope(suppress);
    if (token) gContexts[gDepth - 1].returnSlot = nativeReturnSlot;
    return token;
}
bool EndAnimationPresentationScope(uintptr_t token) noexcept {
    if (!token || !gDepth || gContexts[gDepth - 1].externalToken != token) return false;
    Pop();
    return true;
}
bool AnimationPresentationSelected(unsigned kind, int camp, bool rScene) noexcept {
    ValidateLiveScene();
    return !gPreserveNativeChain && !gRecoveryPending && Selected(kind, camp, rScene);
}
bool AnimationPresentationAllSelected() noexcept {
    ValidateLiveScene();
    if (!gInstalled || gPreserveNativeChain || gRecoveryPending ||
        !EverySKindAndCampSelected()) return false;
    const int scene = PresentationScene();
    return (scene == 1 && gSuperS) || (scene == 0 && gSuperR);
}
bool AnimationBattlePresentationEnabled() noexcept {
    ValidateLiveScene();
    return gInstalled && !gPreserveNativeChain && !gRecoveryPending &&
        PresentationScene() == 1 && gSuperS;
}
bool AnimationPresentationSelected(AnimationPresentationCategory category,unsigned kind,int camp) noexcept {
    ValidateLiveScene();
    if (!gInstalled || gPreserveNativeChain || gRecoveryPending ||
        (PresentationScene()) < 0) return false;
    switch (category) {
    case AnimationPresentationCategory::Dialog: return gCancelDialog;
    case AnimationPresentationCategory::MapText: return gCancelMapText;
    case AnimationPresentationCategory::RAction: return Selected(kind,camp,true);
    case AnimationPresentationCategory::SAction: return Selected(kind,camp,false);
    default: return false;
    }
}
void PreserveNativePresentationUntilIdle() noexcept {
    gPreserveNativeChain = gDepth != 0;
    for (size_t i = 0; i < gDepth; ++i) gContexts[i].suppress = false;
    ResetRPresentation();
    FlushPending();
    FinalizeRCompose();
}
bool RetargetAnimationPresentationOwner(uintptr_t ownerAddress,int camp) noexcept {
    ValidateLiveScene();
    if (!gDepth || gContexts[gDepth - 1].ownerEntry != ownerAddress ||
        gContexts[gDepth - 1].rScene || gContexts[gDepth - 1].kind != 3u ||
        (PresentationScene()) != 1) return false;
    Context& owner = gContexts[gDepth - 1];
    const bool selected = Selected(owner.kind,camp,false);
    if (owner.suppress && !selected) FlushPending();
    owner.camp = camp;
    owner.suppress = selected;
    return true;
}
bool ResolveNativeAnimationReturn(uintptr_t returnSlot, uintptr_t observedReturn,
    uintptr_t& nativeReturn) noexcept {
    ValidateScenarioGeneration();
    nativeReturn = 0;
    const uintptr_t bridge = reinterpret_cast<uintptr_t>(&AnimationExitBridge);
    if (!gPrepared || !gInstalled || !gDepth || !returnSlot || (returnSlot & 3) ||
        observedReturn != bridge) return false;
    uintptr_t stackPointer = 0;
    __asm mov stackPointer, esp
    const uintptr_t stackLow = __readfsdword(8);
    const uintptr_t stackHigh = __readfsdword(4);
    if (returnSlot < stackPointer || returnSlot < stackLow || stackHigh < sizeof(uintptr_t) ||
        returnSlot > stackHigh - sizeof(uintptr_t)) return false;
    uintptr_t actual = 0, original = 0;
    if (!Read(returnSlot, &actual, sizeof(actual)) || actual != bridge) return false;
    for (size_t i = gDepth; i; --i) {
        const Context& context = gContexts[i - 1];
        if (context.returnSlot != returnSlot) continue;
        // A copied bridge word or an ambiguous shadow slot never grants a
        // native caller identity. No scope validation here may rewrite slots.
        if (original || !context.returnAddress || context.returnAddress == bridge) return false;
        original = context.returnAddress;
    }
    if (!original || !Read(returnSlot, &actual, sizeof(actual)) || actual != bridge) return false;
    nativeReturn = original;
    return true;
}
int CurrentAnimationWaitPolicy(uintptr_t caller, uintptr_t callerFrame) noexcept {
    ValidateLiveScene();
    uintptr_t normalized = caller ? caller - gBase + kOrigin : 0;
    uintptr_t ownerFrame = callerFrame;
    if (normalized && normalized == gLayout.animationWaitRouterReturn) {
        uintptr_t outerReturn = 0;
        if (!Read(callerFrame + sizeof(uintptr_t), &outerReturn, sizeof(outerReturn)) ||
            !Read(callerFrame, &ownerFrame, sizeof(ownerFrame))) return 0;
        normalized = outerReturn - gBase + kOrigin;
    }
    if (normalized && normalized == gLayout.animationWaitLoopReturn) {
        uintptr_t outerReturn = 0;
        if (!Read(ownerFrame + sizeof(uintptr_t), &outerReturn, sizeof(outerReturn)) ||
            !Read(ownerFrame, &ownerFrame, sizeof(ownerFrame))) return 0;
        normalized = outerReturn - gBase + kOrigin;
    }
    bool numbersWait = normalized && (normalized == gLayout.animationNumberWaitCallers[0] ||
        normalized == gLayout.animationNumberWaitCallers[1]);
    if (normalized && normalized == gLayout.animationNumberTickWait) {
        // The old map presenter shares one wait across every animation tick.
        // Only ticks 17..24 keep the number overlay on screen; all other ticks
        // retain the physical policy. Never classify this whole caller as a
        // numeric wait, or movement/attack holds would inherit Numbers.
        int tick = -1;
        numbersWait = ownerFrame >= 0x10 && Read(ownerFrame - 0x10, &tick, sizeof(tick)) &&
            tick >= 17 && tick <= 24;
    }
    if (numbersWait) {
        for (size_t i = gDepth; i; --i) {
            const Context& context = gContexts[i - 1];
            if (!context.rScene && context.kind == static_cast<unsigned>(AnimationKind::Physical)) {
                const bool canceled = Selected(static_cast<unsigned>(AnimationKind::Numbers), context.camp, false);
                RecordPresentationWait(canceled);
                return canceled ? 1 : 0;
            }
        }
    }
    if (gDepth) {
        const bool canceled = gContexts[gDepth - 1].suppress;
        RecordPresentationWait(canceled);
        return canceled ? 1 : 0;
    }
    if ((PresentationScene()) == 0 && normalized == gLayout.rAsyncWaitReturn) {
        if (RPresentationAsyncActive()) {
            RecordPresentationWait(true);
            RecordRPresentationAsyncWait();
            return 1;
        }
        FinalizeRCompose();
    }
    return -1;
}
void SetAnimationScenePredicate(int (*predicate)()) noexcept { gScenePredicate = predicate; }
void SetAnimationScenarioGenerationProvider(ULONGLONG (*provider)()) noexcept {
    gScenarioGenerationProvider = provider; gScenarioGenerationKnown = false;
    gScenarioGenerationInvalidated = false;
}
bool ConsumeAnimationScenarioGenerationInvalidation() noexcept {
    ValidateScenarioGeneration();
    if (!gScenarioGenerationInvalidated) return true;
    gScenarioGenerationInvalidated = false;
    return false;
}
void SetAnimationMapIdentityPredicate(bool (*predicate)(uintptr_t,unsigned,ULONGLONG&)) noexcept {
    gMapIdentityPredicate = predicate;
}
bool AnimationSelectedStrategyOwner() noexcept {
    ValidateLiveScene();
    if (PresentationScene() != 1 || !gDepth) return false;
    const Context& owner = gContexts[gDepth - 1];
    return !owner.rScene && owner.kind == static_cast<unsigned>(AnimationKind::Strategy) &&
        owner.ownerEntry && owner.suppress && !gRecoveryPending && !gPreserveNativeChain;
}
bool AnimationPresentationOwnerActive(uintptr_t ownerEntry) noexcept {
    ValidateLiveScene();
    if (!ownerEntry || !gInstalled || PresentationScene() != 1 ||
        gRecoveryPending || gPreserveNativeChain) return false;
    for (size_t i = gDepth; i; --i)
        if (!gContexts[i - 1].rScene && gContexts[i - 1].ownerEntry == ownerEntry) return true;
    return false;
}
bool AnimationPresentationOwnerSelected(uintptr_t ownerEntry) noexcept {
    ValidateLiveScene();
    if (!ownerEntry || PresentationScene() < 0 || !gDepth || !gContexts[gDepth - 1].suppress ||
        gRecoveryPending || gPreserveNativeChain) return false;
    for (size_t i = gDepth; i; --i) {
        const Context& owner = gContexts[i - 1];
        if (!owner.suppress) return false;
        if (owner.ownerEntry == ownerEntry) return true;
    }
    return false;
}
void RecordUnclassifiedAnimationWait(uintptr_t caller) noexcept {
    const DWORD lastError = GetLastError();
    struct RestoreLastError { DWORD value; ~RestoreLastError() { SetLastError(value); } } restore{lastError};
    static SRWLOCK lock = SRWLOCK_INIT;
    static uintptr_t reported[128] = {};
    static size_t reportedCount = 0;
    AcquireSRWLockExclusive(&lock);
    bool record = reportedCount < _countof(reported);
    for (size_t i = 0; i < reportedCount; ++i) if (reported[i] == caller) record = false;
    if (record) reported[reportedCount++] = caller;
    ReleaseSRWLockExclusive(&lock);
    if (!record) return;
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    wchar_t message[256] = {};
    const int formatted = _snwprintf_s(message,_countof(message),_TRUNCATE,
        L"%04u-%02u-%02u %02u:%02u:%02u build=%ls profile=%u scene=%d "
        L"unclassified-wait=0x%08X native-preserved\r\n", time.wYear,time.wMonth,time.wDay,
        time.wHour,time.wMinute,time.wSecond,XiaoAnInstall::kPackageVersion,
        static_cast<unsigned>(CompiledEngineProfile()), PresentationScene(),
        static_cast<unsigned>(caller));
    if (formatted < 0) return;
    wchar_t path[MAX_PATH] = {};
    const DWORD pathLength = GetModuleFileNameW(nullptr,path,_countof(path));
    if (!pathLength || pathLength >= _countof(path)) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    *slash = 0;
    // Avoid the CRT invalid-parameter callback when a legitimate game path
    // leaves too little space for this optional, bounded diagnostics file.
    constexpr size_t longestSuffix = _countof(L"\\patch\\xiaoan66\\animation-events.log");
    if (wcslen(path) + longestSuffix > _countof(path)) return;
    if (wcscat_s(path, L"\\patch")) return;
    CreateDirectoryW(path, nullptr);
    if (wcscat_s(path, CompiledEngineProfile() == EngineProfileId::Ccz64 ? L"\\xiaoan64" :
        CompiledEngineProfile() == EngineProfileId::Ccz65 ? L"\\xiaoan65" : L"\\xiaoan66")) return;
    CreateDirectoryW(path, nullptr);
    if (wcscat_s(path, L"\\animation-events.log")) return;
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    char encoded[1024] = {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, message, -1, encoded, sizeof(encoded), nullptr, nullptr);
    DWORD written = 0;
    if (GetFileSizeEx(file, &size) && size.QuadPart < 1024 * 1024 && bytes > 1)
        WriteFile(file, encoded, static_cast<DWORD>(bytes - 1), &written, nullptr);
    CloseHandle(file);
}
#if defined(XA66_TEST) || defined(ANIMATION_TEST_OBSERVER)
bool AnimationTestBegin(unsigned kind, int camp, bool rScene) noexcept { return Push(kind, camp, rScene, 0); }
void AnimationTestEnd() noexcept { Pop(); }
int AnimationTestPresent(int surface, int left, int top, int width, int height) noexcept {
    return AnimationPresentBridge(surface, left, top, width, height);
}
void AnimationTestConfigure(bool superS, bool superR, Present present) noexcept {
    gSuperS = superS; gSuperR = superR; gPresent = present;
}
void AnimationTestAllPresentation(bool enabled) noexcept { gAllPresentation = enabled; }
void AnimationTestFailWrites(int forwardIndex, int rollbackIndex, bool afterForwardCopy, bool afterRollbackCopy,
    bool failCache) noexcept {
    gFailForward = forwardIndex; gFailRollback = rollbackIndex;
    gForwardWrites = gRollbackWrites = 0;
    gAfterForwardCopy = afterForwardCopy; gAfterRollbackCopy = afterRollbackCopy;
    gFailCache = failCache;
}
#endif
