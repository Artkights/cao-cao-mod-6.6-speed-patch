#include "RPresentation.h"
#include "PresentationDiagnostics.h"
#include <cstring>

namespace {
constexpr size_t kObjects = 128, kOwners = 64;
constexpr uintptr_t kOrigin = 0x400000;
struct NativeObject { BYTE data[0x40]; };
struct Owner {
    uintptr_t serial = 0;
    NativeObject before[kObjects] = {};
};
struct Token {
    uintptr_t serial = 0;
    uintptr_t diagnostic = 0;
    WORD identity = 0, route = 0;
};
thread_local Owner gOwners[kOwners];
thread_local Token gTokens[kObjects];
thread_local uintptr_t gSerial = 0;
uintptr_t gObjects = 0;
bool gEnabled = false;
WORD Word(const NativeObject& object, size_t offset) noexcept {
    WORD value = 0; memcpy(&value,object.data + offset,sizeof(value)); return value;
}
bool ReadObjects(NativeObject* objects) noexcept {
    if (!gObjects) return false;
    __try { memcpy(objects,reinterpret_cast<const void*>(gObjects),sizeof(NativeObject) * kObjects); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Moving(const NativeObject& object) noexcept {
    // Native 429C2A advances state 2 through the route processor, preserving
    // every route step. 40B1C9 changes state to 4 and clears the route at EOF.
    return (Word(object,0x28) & 0x8000) && Word(object,0x30) == 2 && Word(object,0x3C) != 0xFFFF;
}
void Retire(Token& token,const char* reason) noexcept {
    EndDetachedPresentationDiagnostic(token.diagnostic,reason);token=Token();
}
}
void ResetRPresentation() noexcept {
    for (auto& owner : gOwners) owner.serial = 0;
    for (auto& token : gTokens) Retire(token,"reset");
}
void ConfigureRPresentation(const EngineLayout& layout, uintptr_t base, bool enabled) noexcept {
    ResetRPresentation();
    gObjects = base && layout.rObjectBase ? base + layout.rObjectBase - kOrigin : 0;
    gEnabled = enabled && layout.rPresentationVerified && gObjects;
}
bool RMapSceneIdentity(const EngineLayout& layout,uintptr_t base,uintptr_t script,
    unsigned opcode,int ordinal,ULONGLONG generation) noexcept {
    if (!base || !layout.rPresentationVerified || !layout.rScriptContext || !layout.sceneState ||
        !generation || ordinal < 0 || ordinal >= 0xFFFF || (ordinal & 1) ||
        script != base + layout.rScriptContext - kOrigin || opcode < 0x29 || opcode > 0x2B) return false;
    __try {
        return *reinterpret_cast<const BYTE*>(base + layout.sceneState - kOrigin) == 0 &&
            *reinterpret_cast<const BYTE*>(script + 0x2E) == opcode;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uintptr_t BeginRPresentationOwner(unsigned char role, bool selected) noexcept {
    if (role == RPresentationReset) { ResetRPresentation(); return 0; }
    if (role != RPresentationCommand || !selected || !gEnabled) return 0;
    for (auto& owner : gOwners) {
        if (owner.serial) continue;
        if (!ReadObjects(owner.before)) return 0;
        if (!++gSerial) ++gSerial;
        owner.serial = gSerial;
        return owner.serial;
    }
    // No untracked ownership may inherit a previous object's skip policy.
    for (auto& token : gTokens) Retire(token,"owner-capacity");
    return 0;
}
void EndRPresentationOwner(uintptr_t serial) noexcept {
    if (!serial) return;
    for (auto& owner : gOwners) {
        if (owner.serial != serial) continue;
        NativeObject after[kObjects];
        if (gEnabled && ReadObjects(after)) {
            for (size_t i = 0; i < kObjects; ++i) {
                if (!memcmp(&owner.before[i],&after[i],sizeof(NativeObject))) continue;
                // Only verified native moving objects survive their handler.
                // Static overlays have independent pure drawing gates. They
                // must not acquire route tokens or change object lifetime.
                Retire(gTokens[i],"replaced");
                if (Moving(after[i])) {
                    auto& token=gTokens[i];token.serial=serial;token.identity=Word(after[i],0x3A);token.route=Word(after[i],0x3C);
                    token.diagnostic=BeginDetachedPresentationDiagnostic(4,gObjects+i*0x40,0,-1,true,gObjects+i*0x40,0x40);
                }
            }
        }
        owner.serial = 0;
        return;
    }
}
bool RPresentationAsyncActive() noexcept {
    if (!gEnabled) return false;
    NativeObject objects[kObjects];
    if (!ReadObjects(objects)) { ResetRPresentation(); return false; }
    bool active = false;
    for (size_t i = 0; i < kObjects; ++i) {
        auto& token = gTokens[i];
        if (!token.serial) continue;
        if (!Moving(objects[i]) || Word(objects[i],0x3A) != token.identity ||
            Word(objects[i],0x3C) != token.route) {
            Retire(token,Word(objects[i],0x30)==4 || Word(objects[i],0x3C)==0xFFFF ? "route-complete" : "identity-changed");
            continue;
        }
        active = true;
    }
    return active;
}
void RecordRPresentationAsyncWait() noexcept {
    for (const auto& token : gTokens) if (token.serial) RecordDetachedPresentationWait(token.diagnostic,true);
}
void RecordRPresentationAsyncCompose() noexcept {
    for (const auto& token : gTokens) if (token.serial) RecordDetachedPresentationCompose(token.diagnostic,true);
}

void ClearRPresentationObject(uintptr_t object) noexcept {
    if (!gObjects || object < gObjects || (object - gObjects) % 0x40 ||
        (object - gObjects) / 0x40 >= kObjects) return;
    Retire(gTokens[(object - gObjects) / 0x40],"native-clear");
}

bool RPresentationEndingCaller(const EngineLayout& layout,uintptr_t base,uintptr_t* returnSlot) noexcept {
    if (!returnSlot || !base) return false;
    __try { return *returnSlot-base+kOrigin == (layout.profile==EngineProfileId::Ccz66 ? 0x42BF0Bu : 0x426E55u); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ShouldSkipRPresentationEntry(const EngineLayout& layout,uintptr_t base,
    const AnimationEntryLayout& entry,uintptr_t actor,uintptr_t* returnSlot,bool selected) noexcept {
    if (entry.rPresentationRole == RPresentationEndingPerson) {
        if (!selected || !layout.rPresentationVerified || !base || !returnSlot) return false;
        __try {
            const uintptr_t caller=*returnSlot-base+kOrigin;
            const unsigned index=static_cast<unsigned>(returnSlot[1]);
            if (caller!=(layout.profile==EngineProfileId::Ccz66 ? 0x43186Du : 0x430567u) || index>=128) return false;
            // 72-9 only prepares this list. The delayed ending renderer is
            // purely graphical and returns zero for a valid person. Keep the
            // native sentinel path (-1), which controls the outer sequence.
            const DWORD person=reinterpret_cast<const DWORD*>(base+0x4B2B30-kOrigin)[index];
            return person<0x400;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    if (entry.rPresentationRole == RPresentationStaticNumbers || entry.rPresentationRole == RPresentationStaticIllustration) {
        if (!selected || !layout.rPresentationVerified || !layout.rObjectBase || !returnSlot) return false;
        const uintptr_t first = base + layout.rObjectBase - kOrigin;
        if (actor < first || (actor - first) % 0x40) return false;
        const uintptr_t slot = (actor - first) / 0x40;
        uintptr_t caller = 0;
        __try { caller = *returnSlot - base + kOrigin; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        // These fixed slots are native 72-25/28 overlays. Their data/resource
        // writers and active-list/lifetime bookkeeping execute outside these
        // pure drawing leaves; skipping does not alter any object byte.
        if (entry.rPresentationRole == RPresentationStaticNumbers)
            return slot >= 114 && slot < 128 && caller ==
                (layout.profile == EngineProfileId::Ccz64 ? 0x427B70u : 0x427BB8u);
        return layout.profile != EngineProfileId::Ccz64 && slot >= 106 && slot < 114 && caller == 0x427B99;
    }
    if (entry.rPresentationRole != RPresentationPortraitMove || !selected || !layout.rPresentationVerified ||
        !base || !returnSlot || !actor) return false;
    DWORD arguments[3] = {};
    __try { memcpy(arguments,returnSlot + 1,sizeof(arguments)); } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    // 40A33F searches precisely eight native portrait records, each containing
    // person ID and screen x/y. It returns the first match, not a business ID.
    unsigned slot = 8;
    __try {
        for (unsigned i = 0; i < 8; ++i)
            if (reinterpret_cast<const DWORD*>(actor)[i * 3] == arguments[0]) { slot = i; break; }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (slot == 8) return true; // Native not-found path has no visible effect.
    const uintptr_t record = actor + slot * 12;
    const bool is66 = layout.profile == EngineProfileId::Ccz66;
    using Frame = void (__cdecl*)();
    using Portrait = void (__thiscall*)(void*,unsigned,unsigned);
    const auto restore = reinterpret_cast<Portrait>(base + (is66 ? 0x4359B7 : 0x432CD6) - kOrigin);
    const auto begin = reinterpret_cast<Frame>(base + 0x41E5DB - kOrigin);
    const auto finish = reinterpret_cast<Frame>(base + 0x41E5F6 - kOrigin);
    // Audit of the native loop proves it only interpolates these coordinates
    // and manages graphics backing stores. Map-text cancellation restores the
    // old footprint and captures the destination background, without drawing
    // a final portrait. The later native disappear command still restores that
    // backing store and clears the record; no route/script callback is replaced.
    begin();
    __try {
        restore(reinterpret_cast<void*>(actor),slot,1);
        *reinterpret_cast<DWORD*>(record + 4) = arguments[1];
        *reinterpret_cast<DWORD*>(record + 8) = arguments[2] + 0x28u;
        restore(reinterpret_cast<void*>(actor),slot,0);
    } __finally { finish(); }
    return true;
}
