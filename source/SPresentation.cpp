#include "SPresentation.h"
#include "AnimationRuntime.h"
#include "Features.h"
#include <cstring>

// Retargeting validates the native EBP chain while called from this adapter.
// Do not let FPO/general-register EBP hide the still-live script owner.
#pragma optimize("y", off)

namespace {
constexpr uintptr_t kOrigin = 0x400000;
bool Read(uintptr_t address, void* destination, size_t size) noexcept {
    if (!address) return false;
    __try { memcpy(destination, reinterpret_cast<const void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Argument(uintptr_t* stack, unsigned index, uintptr_t& value) noexcept {
    return stack && index < 8 && Read(reinterpret_cast<uintptr_t>(stack + index + 1), &value, sizeof(value));
}
int UnitCamp(const EngineLayout& layout, uintptr_t base, uintptr_t unit) noexcept {
    if (!layout.animationUnitBase) return -1;
    const uintptr_t first = base + layout.animationUnitBase - kOrigin;
    if (unit < first || (unit - first) % 0x30 || (unit - first) / 0x30 >= 250) return -1;
    WORD person = 0xFFFF;
    BYTE camp = 0xFF;
    return Read(unit, &person, sizeof(person)) && person != 0xFFFF &&
        Read(unit + 5, &camp, 1) && camp < 3 ? camp : -1;
}
int SlotCamp(const EngineLayout& layout, uintptr_t base, uintptr_t slot) noexcept {
    return slot < 250 && layout.animationUnitBase ?
        UnitCamp(layout, base, base + layout.animationUnitBase - kOrigin + slot * 0x30u) : -1;
}
int PersonCamp(const EngineLayout& layout, uintptr_t base, uintptr_t person) noexcept {
    if (!layout.animationUnitBase || person >= 0x400) return -1;
    int result = -1;
    bool found = false;
    for (unsigned slot = 0; slot < 250; ++slot) {
        const uintptr_t unit = base + layout.animationUnitBase - kOrigin + slot * 0x30u;
        WORD id = 0xFFFF;
        if (!Read(unit, &id, sizeof(id))) return -1;
        if (id != person) continue;
        if (found) return -1;
        found = true;
        result = UnitCamp(layout, base, unit);
    }
    return result;
}
int ScriptPersonCamp(const EngineLayout& layout, uintptr_t base, uintptr_t person) noexcept {
    // Match the verified native person/encoded-slot mapper without executing
    // game code from a hook. Special party selectors are not acting units.
    if (layout.animationDialogueEncodedSlots && person >= 0x404)
        return SlotCamp(layout, base, person - 0x404);
    BYTE direct = 0;
    if (layout.animationDialogueSlotFlag &&
        Read(base + layout.animationDialogueSlotFlag - kOrigin, &direct, 1) && direct)
        return SlotCamp(layout, base, static_cast<WORD>(person));
    return PersonCamp(layout, base, person);
}
int PhaseCamp(const EngineLayout& layout, uintptr_t base) noexcept {
    BYTE phase = 0xFF;
    return layout.phase && Read(base + layout.phase - kOrigin, &phase, 1) && phase < 3 ? phase : -1;
}
uintptr_t Caller(uintptr_t base, uintptr_t* stack) noexcept {
    uintptr_t caller = 0;
    return stack && Read(reinterpret_cast<uintptr_t>(stack), &caller, sizeof(caller)) && caller >= base ?
        caller - base + kOrigin : 0;
}
bool NumericBarCaller(uintptr_t caller) noexcept {
    const uintptr_t callers[] = {0x450492,0x4504BA,0x450515,0x4506BD,0x4506F1,0x450757};
    for (uintptr_t verified : callers) if (caller == verified) return true;
    return false;
}
bool EffectFrameCaller(const EngineLayout& layout, uintptr_t caller) noexcept {
    return caller == (layout.profile == EngineProfileId::Ccz66 ? 0x401F51u : 0x401F72u);
}
bool AiPreviewCaller(uintptr_t caller) noexcept {
    return caller == 0x43A9E9 || caller == 0x43ABD7 || caller == 0x43B301;
}
bool AiInformationCaller(const EngineLayout& layout, uintptr_t caller) noexcept {
    return caller == (layout.profile == EngineProfileId::Ccz64 ? 0x4414D0u :
        layout.profile == EngineProfileId::Ccz66 ? 0x441543u : 0x4414C6u);
}
bool CallerIn(const uintptr_t* callers, size_t count, uintptr_t caller) noexcept {
    for (size_t i = 0; i < count; ++i) if (callers[i] == caller) return true;
    return false;
}
bool AutoInformationCaller(const EngineLayout& layout, uintptr_t caller) noexcept {
    return CallerIn(layout.animationAutoInformationCallers,
        layout.animationAutoInformationCallerCount, caller);
}
bool MapInformationCaller(const EngineLayout& layout, uintptr_t caller) noexcept {
    return CallerIn(layout.animationMapInformationCallers,
        layout.animationMapInformationCallerCount, caller);
}
bool DirectPanelCaller(const EngineLayout& layout, uintptr_t caller) noexcept {
    return CallerIn(layout.animationDirectPanelCallers,
        layout.animationDirectPanelCallerCount, caller);
}
bool AiPreviewPanelCaller(const EngineLayout& layout, uintptr_t caller) noexcept {
    return CallerIn(layout.animationAiPreviewPanelCallers,
        layout.animationAiPreviewPanelCallerCount, caller);
}
bool AiPreviewMoveCaller(const EngineLayout& layout, uintptr_t caller) noexcept {
    return CallerIn(layout.animationAiPreviewMoveCallers,
        layout.animationAiPreviewMoveCallerCount, caller);
}
bool AiPreviewRangeCaller(const EngineLayout& layout, uintptr_t caller) noexcept {
    return CallerIn(layout.animationAiPreviewRangeCallers,
        layout.animationAiPreviewRangeCallerCount, caller);
}
bool PreviewArrowCaller(const EngineLayout& layout, uintptr_t caller) noexcept {
    return CallerIn(layout.animationPreviewArrowCallers,
        layout.animationPreviewArrowCallerCount, caller);
}
bool HighlightFillCaller(const EngineLayout& layout, uintptr_t caller) noexcept {
    return CallerIn(layout.animationHighlightFillCallers,
        layout.animationHighlightFillCallerCount, caller);
}
bool NativeCall(uintptr_t base, uintptr_t caller, uintptr_t target) noexcept {
    BYTE opcode = 0;
    int32_t displacement = 0;
    return caller >= kOrigin + 5 && Read(base + caller - kOrigin - 5,&opcode,1) && opcode == 0xE8 &&
        Read(base + caller - kOrigin - 4,&displacement,sizeof(displacement)) &&
        static_cast<uint32_t>(caller + displacement) == target;
}
bool AutomaticStatusAction(const EngineLayout& layout, uintptr_t base, uintptr_t* stack) noexcept {
    if (layout.profile != EngineProfileId::Ccz66 || !AnimationPresentationOwnerActive(0x43CDDD)) return false;
    const uintptr_t caller = Caller(base,stack);
    const uintptr_t sources[] = {0x4074FA,0x408455,0x40846B,0x40847B,0x408499,0x4084A9,0x4084C0,0x4084D0,0x4084E0};
    return CallerIn(sources,_countof(sources),caller) && NativeCall(base,caller,0x45778F);
}
bool AutomaticStatusText(const EngineLayout& layout, uintptr_t base, uintptr_t* stack) noexcept {
    if (layout.profile != EngineProfileId::Ccz66 || !AnimationPresentationOwnerActive(0x43CDDD)) return false;
    const uintptr_t caller = Caller(base,stack);
    const uintptr_t sources[] = {0x4085C5,0x408601,0x408641,0x408686,0x40874A,0x408781,0x4087C0};
    return CallerIn(sources,_countof(sources),caller) && NativeCall(base,caller,0x40FAA0);
}
bool ItemPixelSource(uintptr_t base, uintptr_t* stack, unsigned index, uintptr_t caller,
    uintptr_t target) noexcept {
    uintptr_t source = 0;
    return stack && Read(reinterpret_cast<uintptr_t>(stack + index),&source,sizeof(source)) &&
        source >= base && source - base + kOrigin == caller && NativeCall(base,caller,target);
}
bool ItemTransientBlit(uintptr_t base, uintptr_t* stack, uintptr_t caller) noexcept {
    if (!AnimationPresentationOwnerSelected(0x41C74D)) return false;
    uintptr_t width = 0, height = 0;
    if (!Argument(stack,2,width) || !Argument(stack,3,height) || width != 32 || height != 32) return false;
    // 451913 retains its palette reset after the terminal copy. Its pushed
    // DWORD is a local (429D4C is ret0), so the native caller is stack[8].
    if (caller == 0x45195A)
        return NativeCall(base,caller,0x47E09B) && ItemPixelSource(base,stack,8,0x41C603,0x451913);
    // 401C61 retains all blend work and waits. Its four DWORD locals place
    // the item-specific source at stack[11]; unrelated dissolves stay native.
    return caller == 0x401D04 && NativeCall(base,caller,0x47E09B) &&
        ItemPixelSource(base,stack,11,0x41C917,0x401C61);
}
bool PanelOwner(const EngineLayout& layout, uintptr_t base, int scene) noexcept {
    if (CurrentAnimationWaitPolicy() == 1 && AnimationPresentationOwnerSelected(0x43CDDD) &&
        AnimationPresentationOwnerSelected(0x40FAA0)) return true;
    if (scene == 1 && SPresentationBattlePreviewPolicy(layout,base,scene) != 1) return false;
    return AnimationPresentationOwnerSelected(0x44049E) || AnimationPresentationOwnerSelected(0x45218F);
}
bool PanelGlyphCaller(uintptr_t caller) noexcept {
    return caller == 0x40F6A7 || caller == 0x40F7AF || caller == 0x40F7D3 ||
        caller == 0x40F7F5 || caller == 0x40F82B;
}
bool CastingMask(const EngineLayout& layout, uintptr_t base, uintptr_t* stack) noexcept {
    if (layout.profile != EngineProfileId::Ccz66 || Caller(base,stack) != 0x40EA22 ||
        !AnimationPresentationOwnerSelected(0x425627)) return false;
    uintptr_t pixels = 0, rate = 0, color = 0, mode = 0;
    return Argument(stack,0,pixels) && pixels == 0x1000 &&
        Argument(stack,2,rate) && rate <= 70 && rate % 10 == 0 &&
        Argument(stack,3,color) && color == 0xFFFFFF && Argument(stack,4,mode) && mode == 1;
}
bool DuelCaller(const EngineLayout& layout, unsigned role, uintptr_t caller) noexcept {
    const bool is66 = layout.profile == EngineProfileId::Ccz66;
    switch (role) {
    case SPresentationDuelOpen: return caller == 0x449A3A;
    case SPresentationDuelFinish: return caller == (layout.profile == EngineProfileId::Ccz64 ? 0x404E0Bu : 0x41584Cu);
    case SPresentationDuelAppearance: return caller == 0x4174B4;
    case SPresentationDuelResult: return caller == 0x4174CD;
    case SPresentationDuelDefeat: return caller == 0x41121D;
    case SPresentationDuelDialogue: return caller == (is66 ? 0x41762Au : 0x417639u) || caller == 0x448CAD;
    case SPresentationDuelPose: return caller == (is66 ? 0x417681u : 0x417692u) || caller == 0x448C9D;
    case SPresentationDuelAttack: return caller == (is66 ? 0x4176DAu : 0x4176EDu);
    case SPresentationDuelSpecial: return caller == (is66 ? 0x417734u : 0x417749u);
    default: return false;
    }
}
bool DuelHelperCaller(uintptr_t base, uintptr_t source, uintptr_t helper) noexcept {
    // This region consists of the native duel pose/attack controllers. The
    // shared helpers also have non-duel callers outside it; require a real
    // native E8 to this exact helper as well as the duel-owned record below.
    if (source < 0x443CE6 || source > 0x448F50) return false;
    BYTE opcode = 0;
    int displacement = 0;
    return Read(base + source - kOrigin - 5,&opcode,1) && opcode == 0xE8 &&
        Read(base + source - kOrigin - 4,&displacement,sizeof(displacement)) &&
        source + displacement == helper;
}
int DuelCamp(const EngineLayout& layout, uintptr_t base, uintptr_t controller, uintptr_t side) noexcept {
    if (controller != base + 0x4B3C80 - kOrigin) return -1;
    uintptr_t unit = 0;
    return Read(controller + (side ? 0x34 : 8),&unit,sizeof(unit)) ? UnitCamp(layout,base,unit) : -1;
}
}

bool ResolveSPresentationEntryPolicy(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t actor, uintptr_t* returnSlot,
    unsigned& kind, int& camp, bool& rScene, int scene) noexcept {
    if (!entry.sPresentationRole) return false;
    uintptr_t first = 0, second = 0;
    const bool haveFirst = Argument(returnSlot, 0, first);
    switch (entry.sPresentationRole) {
    case SPresentationTurn:
    case SPresentationAction: {
        // The exact subaction is essential: slow/fast/mid turns are status
        // poses, whereas opcode 4F is a separate movement/turn owner.
        kind = entry.sPresentationRole == SPresentationTurn ? 0u : 3u;
        if (entry.sPresentationRole == SPresentationAction) {
            if (!Argument(returnSlot, 1, second) || second > 13) { camp = -1; return true; }
            if (second == 1 || second == 2 || second == 3 || second == 5 || second == 6 || second == 7)
                kind = 1;
            if (AutomaticStatusAction(layout,gameBase,returnSlot)) kind = 3;
        }
        rScene = scene == 0;
        // These routines are actual acting-unit owners. Their own ECX or
        // person argument replaces a parent's target/actor for nested actions.
        const int actorCamp = UnitCamp(layout, gameBase, actor);
        camp = actorCamp >= 0 ? actorCamp : haveFirst ? ScriptPersonCamp(layout, gameBase, first) : -1;
        return true;
    }
    case SPresentationProperty:
    case SPresentationPropertyArea: {
        const bool area = entry.sPresentationRole == SPresentationPropertyArea;
        if (!Argument(returnSlot, area ? 5u : 1u, second) || second > 8) { camp = -1; return true; }
        kind = second >= 7 ? 4u : 3u;
        // A known outer attack/strategy owns its target's presentation. A
        // standalone script property operation instead uses the real unit.
        if (camp < 0 && !area) {
            uintptr_t explicitSlot = 0;
            if (Argument(returnSlot, 4, explicitSlot) && explicitSlot)
                camp = SlotCamp(layout, gameBase, explicitSlot);
            else camp = haveFirst ? ScriptPersonCamp(layout, gameBase, first) : -1;
        }
        // An area has per-unit descendants, which establish their own exact
        // target policy. The surrounding unitless effect belongs to phase.
        // The area parser is not an acting unit. Its per-unit property calls
        // must resolve each actual target independently. The later unitless
        // effect helper establishes its own phase policy, after those calls.
        if (area) camp = -1;
        return true;
    }
    case SPresentationNumbers: {
        kind = 4;
        const uintptr_t caller = Caller(gameBase,returnSlot);
        const bool automaticStatus = layout.profile == EngineProfileId::Ccz66 &&
            caller == 0x43CF8A && NativeCall(gameBase,caller,0x450986);
        if (automaticStatus) kind = 3;
        const int argument = SPresentationNumericActorArgument(layout.profile,caller);
        // Physical/strategy settlement can run after its animation owner has
        // returned. Its first slot is the target, and the second is the actual
        // attacker/caster, including a counterattacker in the opposing phase.
        // Those exact sources override an outer actor; independent changes
        // retain their known outer owner or resolve their own affected unit.
        if (argument < 0) camp = -1;
        else if (automaticStatus || argument == 1 || camp < 0) {
            uintptr_t slot = 0;
            camp = Argument(returnSlot,static_cast<unsigned>(argument),slot) ?
                SlotCamp(layout,gameBase,static_cast<BYTE>(slot)) : -1;
        }
        return true;
    }
    case SPresentationNumbersPanel:
        kind = 4;
        // The verified wrapper already resolved identity. Falling back to the
        // target here would undo its unknown-source/invalid-actor native scope.
        if (Caller(gameBase, returnSlot) != 0x4509D4) camp = -1;
        return true;
    case SPresentationNumericBar:
        kind = 4;
        // A person pointer in a general UI bar is not an action owner. Only
        // independently verified animation-panel callers may inherit an actor.
        if (!haveFirst || !NumericBarCaller(Caller(gameBase, returnSlot))) camp = -1;
        return true;
    case SPresentationScriptEffect:
        kind = 2;
        rScene = scene == 0;
        // The script 6B has only coordinates/effect ID; it is proven actorless.
        if (camp < 0) camp = PhaseCamp(layout, gameBase);
        return true;
    case SPresentationSharedEffectRouter: {
        const uintptr_t caller = Caller(gameBase,returnSlot);
        // The effect machine has exactly these audited production owners:
        // 72-person, 72-location/area, and the area-status controller. Never
        // label the latter as Strategy merely because it shares the renderer.
        const bool status = caller == (layout.profile == EngineProfileId::Ccz66 ? 0x45AECBu : 0x45AF52u);
        const bool strategy = caller == 0x40C572 ||
            caller == (layout.profile == EngineProfileId::Ccz66 ? 0x40233Fu : 0x402342u);
        kind = status ? 3u : 2u;
        rScene = scene == 0;
        if (!status && !strategy) { camp = -1; return true; }
        if (camp < 0 && strategy && caller == 0x40C572 && haveFirst) {
            BYTE slot = 0xFF;
            if (Read(first,&slot,1)) camp = SlotCamp(layout,gameBase,slot);
        }
        if (camp < 0) camp = PhaseCamp(layout,gameBase);
        return true;
    }
    case SPresentationStatusUnit:
        kind = 3;
        if (camp < 0) camp = UnitCamp(layout, gameBase, actor);
        return true;
    case SPresentationAutomaticStatusOwner:
        kind = 3;
        rScene = false;
        camp = UnitCamp(layout,gameBase,actor);
        return true;
    case SPresentationAutomaticStatusNumbers: {
        const uintptr_t caller = Caller(gameBase,returnSlot);
        kind = caller == 0x421CAF && NativeCall(gameBase,caller,0x44FB73) &&
            AnimationPresentationOwnerActive(0x43CDDD) ? 3u : 4u;
        rScene = false;
        // The number's actual affected slot belongs to this actor overlay.
        // All other map-number sources retain their existing parent/slot rule.
        if (kind == 3 || camp < 0)
            camp = haveFirst ? SlotCamp(layout,gameBase,kind == 3 ? static_cast<BYTE>(first) : first) : -1;
        return true;
    }
    case SPresentationAutomaticStatusTile:
    case SPresentationAutomaticStatusGearPanel:
    case SPresentationAutomaticStatusGearWait:
    case SPresentationAutomaticStatusTextOwner:
        kind = 3;
        rScene = false;
        return true;
    case SPresentationItemOwner: {
        kind = 3;
        rScene = false;
        BYTE slot = 0xFF;
        // ECX is the native item action record; +2 is the acting slot.
        // Resolve it afresh even inside a phase UI or another unit's scope.
        camp = Read(actor + 2,&slot,1) ? SlotCamp(layout,gameBase,slot) : -1;
        return true;
    }
    case SPresentationStatusEffectOwner:
        kind = 3;
        rScene = false;
        // 427CBC(effect, slot) owns the affected unit, including phase
        // recovery/buff effects whose phase differs from that unit's camp.
        camp = Argument(returnSlot,1,second) ? SlotCamp(layout,gameBase,static_cast<BYTE>(second)) : -1;
        return true;
    case SPresentationStatusValueOwner:
        kind = 3;
        rScene = scene == 0;
        camp = haveFirst ? SlotCamp(layout,gameBase,static_cast<BYTE>(first)) : -1;
        return true;
    case SPresentationMovementUnit:
        kind = 0;
        // Movement controllers define the moving actor, including appearance
        // and withdrawal, rather than inheriting another unit's policy.
        camp = UnitCamp(layout, gameBase, actor);
        return true;
    case SPresentationMovementPerson:
        kind = 0;
        camp = haveFirst ? ScriptPersonCamp(layout, gameBase, first) : -1;
        return true;
    case SPresentationAppearanceOwner:
        kind = 0;
        // 409229 is entered after 40216A has mapped/deployed the person.
        // Its ECX belongs to the mapper, while argument zero is the unit.
        camp = haveFirst ? UnitCamp(layout,gameBase,first) : -1;
        return true;
    case SPresentationStatusPerson:
        kind = 3;
        if (camp < 0) camp = haveFirst ? ScriptPersonCamp(layout, gameBase, first) : -1;
        return true;
    case SPresentationStatusField:
        kind = 3;
        if (camp < 0) camp = PhaseCamp(layout, gameBase);
        return true;
    case SPresentationHighlight:
        kind = 3;
        // The same helper also services active selection. Only the exact
        // noninteractive script call is eligible for presentation omission.
        camp = -1;
        return true;
    case SPresentationSharedStatus:
    case SPresentationSharedMovement:
        kind = entry.sPresentationRole == SPresentationSharedStatus ? 3u : 0u;
        rScene = scene == 0;
        if (camp < 0) camp = PhaseCamp(layout, gameBase);
        return true;
    case SPresentationTargetHighlight:
        kind = 3;
        // Parameters are not consumed yet. The exact parsed unit is captured
        // by 464A32 below, before any highlight drawing/waits begin.
        camp = -1;
        return true;
    case SPresentationTargetHighlightCapture:
        kind = 3;
        camp = -1;
        return true;
    case SPresentationEffectFrame:
    case SPresentationRangeOverlay:
    case SPresentationCastingMask:
    case SPresentationPanelGlyph:
        // The effect's already-established Strategy/Status/R owner controls
        // this pure renderer; category policy below reads that exact scope.
        camp = -1;
        return true;
    case SPresentationDepartureAnimation:
        kind = haveFirst && first == 0 ? 0u : 3u;
        camp = haveFirst && first <= 2 ? UnitCamp(layout, gameBase, actor) : -1;
        return true;
    case SPresentationAiPreview:
        kind = haveFirst && first <= 2 ? static_cast<unsigned>(first) : 0u;
        camp = haveFirst && first <= 2 && AiPreviewCaller(Caller(gameBase, returnSlot)) ?
            UnitCamp(layout, gameBase, actor) : -1;
        return true;
    case SPresentationDuelOpen:
    case SPresentationDuelFinish:
    case SPresentationDuelAppearance:
    case SPresentationDuelResult:
    case SPresentationDuelDefeat:
    case SPresentationDuelDialogue:
    case SPresentationDuelPose:
    case SPresentationDuelAttack:
    case SPresentationDuelSpecial: {
        const bool fixedSide = entry.sPresentationRole == SPresentationDuelOpen ||
            entry.sPresentationRole == SPresentationDuelFinish || entry.sPresentationRole == SPresentationDuelResult;
        kind = entry.sPresentationRole == SPresentationDuelOpen || entry.sPresentationRole == SPresentationDuelFinish ||
            entry.sPresentationRole == SPresentationDuelAppearance ? 0u :
            entry.sPresentationRole == SPresentationDuelDialogue || entry.sPresentationRole == SPresentationDuelResult ? 5u :
            entry.sPresentationRole == SPresentationDuelAttack || entry.sPresentationRole == SPresentationDuelSpecial ? 1u : 3u;
        camp = DuelCaller(layout,entry.sPresentationRole,Caller(gameBase,returnSlot)) &&
            (fixedSide || haveFirst) ? DuelCamp(layout,gameBase,actor,fixedSide ? 0 : first) : -1;
        rScene = scene == 0;
        // Preserve unknown native action codes; the verified tables contain
        // 17 poses, nine attacks and three special attacks respectively.
        if (entry.sPresentationRole == SPresentationDuelPose || entry.sPresentationRole == SPresentationDuelAttack ||
            entry.sPresentationRole == SPresentationDuelSpecial) {
            const uintptr_t limit = entry.sPresentationRole == SPresentationDuelPose ? 16u :
                entry.sPresentationRole == SPresentationDuelAttack ? 8u : 2u;
            if (!Argument(returnSlot,1,second) || second > limit) camp = -1;
            else if (entry.sPresentationRole == SPresentationDuelPose) {
                // Native 1 turns via 43FD34; 2/3/4 update horizontal position.
                // 5..10/12/14..16 are attack/guard/hit frames in the physical
                // sequence, including hit blinking. 0 and 11 are standalone
                // UNIT_MOV frames; those and 13 erase remain Status.
                kind = second >= 1 && second <= 4 ? 0u :
                    second >= 5 && second != 11 && second != 13 ? 1u : 3u;
            }
        }
        return true;
    }
    case SPresentationInformationPanel:
        kind = 3;
        camp = -1;
        return true;
    case SPresentationAiPreviewMove:
    case SPresentationAiPreviewRange:
    case SPresentationPreviewArrow:
    case SPresentationHighlightFill:
        kind = 0;
        camp = -1;
        return true;
    default:
        camp = -1;
        return true;
    }
}

int SPresentationBattlePreviewPolicy(const EngineLayout& layout, uintptr_t gameBase,int scene) noexcept {
    const int phase = PhaseCamp(layout,gameBase);
    return scene == 1 && (phase == 1 || phase == 2) && AnimationBattlePresentationEnabled() ? 1 : 0;
}

int ResolveSPresentationCategoryPolicy(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t* returnSlot, int scene, unsigned kind, int camp) noexcept {
    switch (entry.sPresentationRole) {
    case SPresentationAction:
        if (AutomaticStatusAction(layout,gameBase,returnSlot))
            return scene == 1 && camp >= 0 &&
                AnimationPresentationSelected(AnimationPresentationCategory::SAction,3,camp) ? 1 : 0;
        return -1;
    case SPresentationItemOwner:
    case SPresentationStatusEffectOwner:
    case SPresentationAutomaticStatusOwner:
        return scene == 1 && camp >= 0 &&
            AnimationPresentationSelected(AnimationPresentationCategory::SAction,3,camp) ? 1 : 0;
    case SPresentationAutomaticStatusNumbers:
        if (kind == 3)
            return scene == 1 && camp >= 0 &&
                AnimationPresentationSelected(AnimationPresentationCategory::SAction,3,camp) ? 1 : 0;
        return -1;
    case SPresentationAutomaticStatusTile:
        // The tile erase and following temporary actor pose form one visual
        // operation. Omit both, retaining the already composed static unit.
        return scene == 1 && Caller(gameBase,returnSlot) == 0x421C38 &&
            NativeCall(gameBase,0x421C38,0x4036F9) &&
            AnimationPresentationOwnerSelected(0x43CDDD) ? 1 : 0;
    case SPresentationAutomaticStatusGearPanel:
        return scene == 1 && Caller(gameBase,returnSlot) == 0x4129DF &&
            NativeCall(gameBase,0x4129DF,0x473202) && CurrentAnimationWaitPolicy() == 1 &&
            AnimationPresentationOwnerSelected(0x43CDDD) ? 1 : 0;
    case SPresentationAutomaticStatusGearWait:
        return scene == 1 && Caller(gameBase,returnSlot) == 0x4086B3 &&
            NativeCall(gameBase,0x4086B3,0x42C646) && CurrentAnimationWaitPolicy() == 1 &&
            AnimationPresentationOwnerSelected(0x43CDDD) ? 1 : 0;
    case SPresentationAutomaticStatusTextOwner:
        return scene == 1 && AutomaticStatusText(layout,gameBase,returnSlot) &&
            CurrentAnimationWaitPolicy() == 1 ? 1 : 0;
    case SPresentationNumbers:
        // The automatic phase owner also owns its HP/MP/XP display. Resolve
        // this exact native source independently, including its native choice.
        if (layout.profile == EngineProfileId::Ccz66 && Caller(gameBase,returnSlot) == 0x43CF8A &&
            NativeCall(gameBase,0x43CF8A,0x450986))
            return scene == 1 && camp >= 0 &&
                AnimationPresentationSelected(AnimationPresentationCategory::SAction,3,camp) ? 1 : 0;
        return -1;
    case SPresentationNumbersPanel:
        // The verified wrapper has already chosen Numbers or automatic
        // Status. Its pure panel must retain that exact native/omitted choice.
        if (Caller(gameBase,returnSlot) == 0x4509D4 && NativeCall(gameBase,0x4509D4,0x450033))
            return CurrentAnimationWaitPolicy() == 1 ? 1 : 0;
        return -1;
    case SPresentationNumericBar: {
        const uintptr_t caller = Caller(gameBase,returnSlot);
        if (NumericBarCaller(caller) && NativeCall(gameBase,caller,0x4509DA))
            return CurrentAnimationWaitPolicy() == 1 ? 1 : 0;
        return -1;
    }
    case SPresentationStatusValueOwner:
        return camp >= 0 && (scene == 0 ?
            AnimationPresentationSelected(AnimationPresentationCategory::RAction) : scene == 1 &&
            AnimationPresentationSelected(AnimationPresentationCategory::SAction,3,camp)) ? 1 : 0;
    case SPresentationDuelPose: {
        uintptr_t action = 0;
        if (!Argument(returnSlot,1,action) || action != 16) return -1;
        if (!DuelCaller(layout,entry.sPresentationRole,Caller(gameBase,returnSlot)) || camp < 0) return 0;
        if (scene == 0) return AnimationPresentationSelected(AnimationPresentationCategory::RAction) ? 1 : 0;
        const uintptr_t controller = gameBase + 0x4B3C80 - kOrigin;
        return scene == 1 && AnimationPresentationSelected(AnimationPresentationCategory::SAction,kind,DuelCamp(layout,gameBase,controller,0)) &&
            AnimationPresentationSelected(AnimationPresentationCategory::SAction,kind,DuelCamp(layout,gameBase,controller,1)) ? 1 : 0;
    }
    case SPresentationDuelOpen:
    case SPresentationDuelFinish:
        // A shared transition/wait belongs to both actual participants. Do
        // not hide an excluded side because the first participant is selected.
        if (!DuelCaller(layout,entry.sPresentationRole,Caller(gameBase,returnSlot)) || camp < 0) return 0;
        if (scene == 0) return AnimationPresentationSelected(AnimationPresentationCategory::RAction) ? 1 : 0;
        return scene == 1 && AnimationPresentationSelected(AnimationPresentationCategory::SAction,kind,camp) &&
            AnimationPresentationSelected(AnimationPresentationCategory::SAction,kind,
                DuelCamp(layout,gameBase,gameBase + 0x4B3C80 - kOrigin,1)) ? 1 : 0;
    case SPresentationDuelResult:
        // The result background/notification is Dialogue; its participant
        // sprite leaves below independently use S Status with their own camp.
        return scene >= 0 && DuelCaller(layout,entry.sPresentationRole,Caller(gameBase,returnSlot)) &&
            AnimationPresentationSelected(AnimationPresentationCategory::Dialog) ? 1 : 0;
    case SPresentationDuelDialogue:
        if (scene < 0 || !DuelCaller(layout,entry.sPresentationRole,Caller(gameBase,returnSlot))) return 0;
        return AnimationPresentationSelected(AnimationPresentationCategory::Dialog) ||
            (scene == 1 && camp >= 0 && AnimationPresentationSelected(AnimationPresentationCategory::SAction,kind,camp)) ? 1 : 0;
    case SPresentationRangeOverlay:
        return scene == 1 && Caller(gameBase,returnSlot) ==
            (layout.profile == EngineProfileId::Ccz66 ? 0x47C63Cu : 0x41E340u) ?
            SPresentationBattlePreviewPolicy(layout,gameBase,scene) : 0;
    case SPresentationEffectFrame:
        return EffectFrameCaller(layout, Caller(gameBase, returnSlot)) &&
            CurrentAnimationWaitPolicy() == 1 ? 1 : 0;
    case SPresentationCastingMask:
        return scene == 1 && CastingMask(layout,gameBase,returnSlot) ? 1 : 0;
    case SPresentationPanelGlyph:
        return scene >= 0 && PanelGlyphCaller(Caller(gameBase,returnSlot)) && PanelOwner(layout,gameBase,scene) ? 1 : 0;
    case SPresentationAiPreview: {
        uintptr_t subtype = 0;
        return scene == 1 && AiPreviewCaller(Caller(gameBase,returnSlot)) &&
            Argument(returnSlot,0,subtype) && subtype <= 2 ?
            SPresentationBattlePreviewPolicy(layout,gameBase,scene) : 0;
    }
    case SPresentationHighlight:
        // The helper also serves player interaction. The script 5B call is
        // the only owner whose display is controlled by Cancel Dialogue.
        if (scene != 1 || Caller(gameBase, returnSlot) != 0x416B46) return 0;
        break;
    case SPresentationTargetHighlight:
        if (scene != 1) return 0;
        break;
    case SPresentationTargetHighlightCapture: {
        // 464A32 also opens unit information from active gameplay. Its exact
        // battle callers use the current phase: our commands keep their UI,
        // while automatic ally/enemy previews ignore acting-unit filters.
        const uintptr_t captureCaller = Caller(gameBase, returnSlot);
        if (scene == 0 && MapInformationCaller(layout,captureCaller))
            return AnimationPresentationSelected(AnimationPresentationCategory::RAction) ? 1 : 0;
        if (scene != 1) return 0;
        if (AiInformationCaller(layout,captureCaller))
            return SPresentationBattlePreviewPolicy(layout,gameBase,scene);
        if (AutoInformationCaller(layout,captureCaller))
            return SPresentationBattlePreviewPolicy(layout,gameBase,scene);
        if (captureCaller != 0x416BE6) return 0;
        return SPresentationBattlePreviewPolicy(layout,gameBase,scene);
    }
    case SPresentationInformationPanel: {
        // Direct forecast/post-action panel calls bypass 464A32. The 6.4 AI
        // preview helper shares this leaf but is owned by the preview scope.
        const uintptr_t panelCaller = Caller(gameBase, returnSlot);
        // The mixed controller and cleanup still run. Battle UI has a phase
        // policy even when its enclosing counterattack belongs to our unit.
        if (scene == 0 && MapInformationCaller(layout,panelCaller))
            return AnimationPresentationSelected(AnimationPresentationCategory::RAction) ? 1 : 0;
        if (scene != 1) return 0;
        if (panelCaller == 0x464A42)
            return SPresentationBattlePreviewPolicy(layout,gameBase,scene) &&
                AnimationPresentationOwnerSelected(0x464A32) ? 1 : 0;
        if (AiPreviewPanelCaller(layout,panelCaller) || DirectPanelCaller(layout,panelCaller))
            return SPresentationBattlePreviewPolicy(layout,gameBase,scene);
        return 0;
    }
    case SPresentationAiPreviewMove:
    case SPresentationAiPreviewRange:
        if (scene != 1) return 0;
        if (!AiPreviewMoveCaller(layout,Caller(gameBase,returnSlot)) &&
            !AiPreviewRangeCaller(layout,Caller(gameBase,returnSlot))) return 0;
        return SPresentationBattlePreviewPolicy(layout,gameBase,scene);
    case SPresentationPreviewArrow:
        if (scene != 1 || !PreviewArrowCaller(layout,Caller(gameBase,returnSlot))) return 0;
        return SPresentationBattlePreviewPolicy(layout,gameBase,scene);
    case SPresentationHighlightFill:
        if (scene != 1 || !HighlightFillCaller(layout,Caller(gameBase,returnSlot))) return 0;
        return AnimationPresentationSelected(AnimationPresentationCategory::Dialog) ? 1 : 0;
    default:
        return -1;
    }
    return AnimationPresentationSelected(AnimationPresentationCategory::Dialog) ? 1 : 0;
}

bool OmitSPresentationDuelBlit(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t* returnSlot, int scene) noexcept {
    if (!(layout.sPresentationPathMask & SPathDuel) || entry.address != 0x47E09B ||
        entry.argumentBytes != 20 || scene < 0) return false;
    const uintptr_t caller = Caller(gameBase,returnSlot);
    if (caller == 0x443C93) {
        uintptr_t source = 0, record = 0;
        if (!Read(reinterpret_cast<uintptr_t>(returnSlot + 7),&source,sizeof(source)) ||
            !Read(reinterpret_cast<uintptr_t>(returnSlot + 13),&record,sizeof(record))) return false;
        source = source >= gameBase ? source - gameBase + kOrigin : 0;
        const uintptr_t controller = gameBase + 0x4B3C80 - kOrigin;
        if (!DuelHelperCaller(gameBase,source,0x443C1D) ||
            (record != controller + 4 && record != controller + 0x30)) return false;
    }
    if (caller == 0x443B7C) {
        // 443AD5's audited fixed frame: 5 blit args, 7 locals, saved EBP,
        // original return, then record argument. Its result-screen calls draw
        // each participant separately, so Dialogue cannot swallow an excluded
        // S Status sprite, and selecting one camp omits only that side.
        uintptr_t source = 0, record = 0;
        if (!Read(reinterpret_cast<uintptr_t>(returnSlot + 14),&source,sizeof(source)) ||
            !Read(reinterpret_cast<uintptr_t>(returnSlot + 15),&record,sizeof(record))) return false;
        source = source >= gameBase ? source - gameBase + kOrigin : 0;
        if (source == 0x448D41 || source == 0x448D4E || source == 0x448D74 || source == 0x448D81) {
            const uintptr_t controller = gameBase + 0x4B3C80 - kOrigin;
            if (record != controller + 4 && record != controller + 0x30) return false;
            const int camp = DuelCamp(layout,gameBase,controller,record == controller + 0x30);
            return camp >= 0 && (scene == 1 ? AnimationPresentationSelected(AnimationPresentationCategory::SAction,3,camp) :
                scene == 0 && AnimationPresentationSelected(AnimationPresentationCategory::RAction));
        }
        const uintptr_t controller = gameBase + 0x4B3C80 - kOrigin;
        if (!DuelHelperCaller(gameBase,source,0x443AD5) ||
            (record != controller + 4 && record != controller + 0x30)) return false;
    }
    if (CurrentAnimationWaitPolicy() != 1) return false;
    // 443C1D updates the animation rectangle before this blit; never skip
    // that mixed wrapper. These leaves only copy completed duel UI pixels.
    const uintptr_t callers[] = {0x443C93,0x443B1C,0x443B7C,0x443328,0x443350,
        0x443378,0x44897B,0x448D2F,0x448E1A};
    for (uintptr_t exact : callers) if (caller == exact) return true;
    return false;
}

bool OmitSPresentationTransientBlit(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t* returnSlot, int scene) noexcept {
    if (layout.profile != EngineProfileId::Ccz66 || entry.address != 0x47E09B ||
        entry.argumentBytes != 20 || scene < 0 || CurrentAnimationWaitPolicy() != 1) return false;
    const uintptr_t caller = Caller(gameBase,returnSlot);
    if (scene == 1 && ItemTransientBlit(gameBase,returnSlot,caller)) return true;
    if (scene == 1 && caller == 0x408579 && NativeCall(gameBase,caller,0x47E09B) &&
        AnimationPresentationOwnerSelected(0x43CDDD)) {
        uintptr_t width = 0, height = 0;
        return Argument(returnSlot,2,width) && Argument(returnSlot,3,height) && width == 32 && height == 32;
    }
    // 451913 owns palette/clipping cleanup; its following 4296B4(0) remains
    // native. Only this completed pixel copy belongs to the selected panel.
    if (caller == 0x45195A && NativeCall(gameBase,caller,0x47E09B)) return PanelOwner(layout,gameBase,scene);
    // Meff >=100 uses another frame machine. 402BED/402CBE and 4030DE restore
    // the background; 402CE4 alone copies the transient effect's final pixels.
    return scene == 1 && caller == 0x402CE9 && NativeCall(gameBase,caller,0x47E09B) &&
        AnimationPresentationOwnerSelected(0x414071);
}

bool OmitSPresentationSprite(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t* returnSlot) noexcept {
    if (entry.address != 0x451347 || entry.argumentBytes != 24 || !returnSlot ||
        CurrentAnimationWaitPolicy() != 1) return false;
    const uintptr_t caller = Caller(gameBase, returnSlot);
    uintptr_t width = 0, height = 0;
    if (!Argument(returnSlot,2,width) || !Argument(returnSlot,3,height)) return false;
    if (layout.profile == EngineProfileId::Ccz66 && NativeCall(gameBase,caller,0x451347)) {
        if (caller == 0x41C5DF)
            // 41C5C7 also closes its caller's frame transaction. Preserve
            // the helper, omit only its temporary pose; 41C87E stays native.
            return width == 48 && height == 48 && AnimationPresentationOwnerSelected(0x41C74D) &&
                (ItemPixelSource(gameBase,returnSlot,8,0x41C822,0x41C5C7) ||
                 ItemPixelSource(gameBase,returnSlot,8,0x41C851,0x41C5C7));
        if (caller == 0x40954B)
            // The native appearance branch has no later 43FCC9 compose: its
            // last height-48 copy is the final static unit, after the loop's
            // background restoration. Keep that completed frame visible.
            return width == 48 && height >= 4 && height < 48 && height % 4 == 0 &&
                AnimationPresentationOwnerSelected(0x409229);
        if (caller == 0x421A16 || caller == 0x40EA5E)
            return width == 64 && height == 64 && AnimationPresentationOwnerSelected(0x425627);
        if (caller == 0x421C4D)
            return width == 48 && height == 48 && AnimationPresentationOwnerSelected(0x43CDDD);
    }
    const bool known = layout.profile == EngineProfileId::Ccz64 ?
        caller == 0x441BC6 || caller == 0x441C53 || caller == 0x441EB9 :
        caller == 0x441A9D || caller == 0x441AF4 || caller == 0x441C15;
    return known && width == 48 && height <= 48;
}

bool ShouldSkipSPresentationEntry(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t* returnSlot, bool selected) noexcept {
    if (!selected || !returnSlot) return false;
    const uintptr_t caller = Caller(gameBase, returnSlot);
    // These mixed controllers retain native state writes and all cleanup.
    if (entry.sPresentationRole == SPresentationTargetHighlightCapture ||
        entry.sPresentationRole == SPresentationInformationPanel ||
        entry.sPresentationRole == SPresentationHighlightFill) return false;
    if (entry.sPresentationRole == SPresentationCastingMask) return CastingMask(layout,gameBase,returnSlot);
    if (entry.sPresentationRole == SPresentationPanelGlyph)
        return layout.profile == EngineProfileId::Ccz66 && PanelGlyphCaller(caller) &&
            PanelOwner(layout,gameBase,-1);
    if (entry.sPresentationRole == SPresentationRangeOverlay)
        return caller == (layout.profile == EngineProfileId::Ccz66 ? 0x47C63Cu : 0x41E340u);
    if (entry.sPresentationRole == SPresentationEffectFrame) return EffectFrameCaller(layout, caller);
    if (entry.sPresentationRole == SPresentationAutomaticStatusTile)
        return caller == 0x421C38 && NativeCall(gameBase,caller,0x4036F9);
    if (entry.sPresentationRole == SPresentationAutomaticStatusGearPanel)
        return caller == 0x4129DF && NativeCall(gameBase,caller,0x473202);
    if (entry.sPresentationRole == SPresentationAutomaticStatusGearWait)
        return caller == 0x4086B3 && NativeCall(gameBase,caller,0x42C646);
    if (entry.sPresentationRole == SPresentationAiPreviewMove) return AiPreviewMoveCaller(layout,caller);
    if (entry.sPresentationRole == SPresentationAiPreviewRange) return AiPreviewRangeCaller(layout,caller);
    if (entry.sPresentationRole == SPresentationPreviewArrow) return PreviewArrowCaller(layout,caller);
    if (entry.sPresentationRole == SPresentationNumbersPanel)
        return caller == 0x4509D4;
    if (entry.sPresentationRole != SPresentationNumericBar) return false;
    return NumericBarCaller(caller);
}

bool OmitSPresentationEntry(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t* returnSlot) noexcept {
    const uintptr_t caller = Caller(gameBase,returnSlot);
    if (entry.sPresentationRole == SPresentationTargetHighlightCapture ||
        entry.sPresentationRole == SPresentationInformationPanel ||
        entry.sPresentationRole == SPresentationHighlightFill) return false;
    if (entry.sPresentationRole == SPresentationCastingMask) return CastingMask(layout,gameBase,returnSlot);
    if (entry.sPresentationRole == SPresentationPanelGlyph)
        return layout.profile == EngineProfileId::Ccz66 && PanelGlyphCaller(caller) &&
            PanelOwner(layout,gameBase,-1);
    if (entry.sPresentationRole == SPresentationRangeOverlay)
        return caller == (layout.profile == EngineProfileId::Ccz66 ? 0x47C63Cu : 0x41E340u);
    if (entry.sPresentationRole == SPresentationEffectFrame) return EffectFrameCaller(layout, caller);
    if (entry.sPresentationRole == SPresentationAutomaticStatusTile)
        return caller == 0x421C38 && NativeCall(gameBase,caller,0x4036F9);
    if (entry.sPresentationRole == SPresentationAutomaticStatusGearPanel)
        return caller == 0x4129DF && NativeCall(gameBase,caller,0x473202);
    if (entry.sPresentationRole == SPresentationAutomaticStatusGearWait)
        return caller == 0x4086B3 && NativeCall(gameBase,caller,0x42C646);
    if (entry.sPresentationRole == SPresentationAiPreviewMove) return AiPreviewMoveCaller(layout,caller);
    if (entry.sPresentationRole == SPresentationAiPreviewRange) return AiPreviewRangeCaller(layout,caller);
    if (entry.sPresentationRole == SPresentationPreviewArrow) return PreviewArrowCaller(layout,caller);
    if (entry.sPresentationRole == SPresentationNumericBar) return NumericBarCaller(caller);
    if (entry.sPresentationRole != SPresentationNumbersPanel || caller != 0x4509D4) return false;
    BYTE alternateDisplay = 0;
    if (!Read(gameBase + 0x505FDC - kOrigin,&alternateDisplay,1)) return false;
    // Native 450033 bypasses the panel entirely in alternate display mode.
    if (alternateDisplay) return true;
    // Preserve precisely the native tail at 45090F..450922. In particular,
    // 45461C clears the previous panel's dirty coordinates and 453AFF restores
    // the map region. Returning before these would leave stale panel pixels.
    // Several callers settle HP/MP/XP AFTER this call; reading model values
    // here to fabricate a final panel would render the wrong numbers.
    using Cleanup = void (__thiscall*)(void*);
    void* controller = reinterpret_cast<void*>(gameBase + 0x4B4250 - kOrigin);
    reinterpret_cast<Cleanup>(gameBase + 0x45461C - kOrigin)(controller);
    reinterpret_cast<Cleanup>(gameBase + 0x453AFF - kOrigin)(controller);
    // Native cleanup exceptions propagate to the native caller. Falling back
    // to the panel after partial cleanup would repeat already completed work.
    return true;
}
