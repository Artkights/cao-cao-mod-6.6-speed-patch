#pragma once
#include "EngineLayout.h"

// These are native calls to the automatic numeric wrapper, not general UI
// queries. Argument indices describe the acting slot; native slots are bytes
// even when an unaligned DWORD push also carries neighbouring record fields.
struct SPresentationNumericSource {
    uintptr_t caller;
    unsigned char profiles; // 6.4, 6.5, 6.6
    unsigned char actorArgument;
};
inline constexpr SPresentationNumericSource kSPresentationNumericSources[] = {
    {0x406C5A,7,0},{0x42210F,7,1},{0x4224A2,7,1},{0x4229ED,7,1},
    {0x42541D,7,0},{0x43CF8A,7,0},{0x44997A,7,0},
    {0x405AE6,1,1},{0x42304B,1,0},{0x44E0A6,1,0},
    {0x405ABE,2,1},{0x41A711,2,0},{0x42DAAF,2,0},{0x44E07C,2,0},{0x45296C,2,0},
    {0x40CD20,4,1},{0x42308D,4,0},{0x44E087,4,0}
};
inline unsigned SPresentationNumericProfile(EngineProfileId profile) noexcept {
    return profile == EngineProfileId::Ccz64 ? 1u : profile == EngineProfileId::Ccz65 ? 2u :
        profile == EngineProfileId::Ccz66 ? 4u : 0u;
}
inline int SPresentationNumericActorArgument(EngineProfileId profile, uintptr_t caller) noexcept {
    const unsigned mask = SPresentationNumericProfile(profile);
    for (const auto& source : kSPresentationNumericSources)
        if (source.caller == caller && (source.profiles & mask)) return source.actorArgument;
    return -1;
}

// These are independently verified native owners, not opcode guesses. Mixed
// business/presentation functions always execute; only the explicitly verified
// UI leaf may return early with the native stack-cleanup ABI.
enum SPresentationRole : unsigned char {
    SPresentationNone = 0,
    SPresentationTurn,
    SPresentationAction,
    SPresentationProperty,
    SPresentationNumbers,
    SPresentationNumericBar,
    SPresentationScriptEffect,
    SPresentationStatusUnit,
    SPresentationStatusPerson,
    SPresentationStatusField,
    SPresentationNumbersPanel,
    SPresentationMovementUnit,
    SPresentationMovementPerson,
    SPresentationPropertyArea,
    SPresentationHighlight,
    SPresentationSharedStatus,
    SPresentationSharedMovement,
    SPresentationTargetHighlight,
    SPresentationTargetHighlightCapture,
    SPresentationSharedEffectRouter,
    SPresentationEffectFrame,
    SPresentationDepartureAnimation,
    SPresentationAiPreview,
    SPresentationRangeOverlay,
    SPresentationDuelOpen,
    SPresentationDuelFinish,
    SPresentationDuelAppearance,
    SPresentationDuelResult,
    SPresentationDuelDefeat,
    SPresentationDuelDialogue,
    SPresentationDuelPose,
    SPresentationDuelAttack,
    SPresentationDuelSpecial,
    // Automatic battle status/forecast panels and AI preview state setters.
    // These are verified pure presentation leaves/helpers, not script handlers,
    // so an early native-ABI return cannot consume script parameters.
    SPresentationInformationPanel,
    SPresentationAiPreviewMove,
    SPresentationAiPreviewRange,
    SPresentationPreviewArrow,
    SPresentationHighlightFill,
    // 409229 owns the already deployed unit in argument zero. Entry ECX is
    // unrelated to that actor and must never override this resolved camp.
    SPresentationAppearanceOwner,
    // Pure pixel consumers. These do not create independent native scopes;
    // their exact caller inherits the verified selected presentation owner.
    SPresentationCastingMask,
    SPresentationPanelGlyph,
    // Mixed item and automatic status effect owners. Both always execute
    // business/cleanup and establish Status from their own actual unit.
    SPresentationItemOwner,
    SPresentationStatusEffectOwner,
    SPresentationStatusValueOwner,
    // Complete native phase decay/recovery/buff owners. Their ECX unit is
    // authoritative even below another unit's or the phase UI's scope.
    SPresentationAutomaticStatusOwner,
    SPresentationAutomaticStatusNumbers,
    SPresentationAutomaticStatusTile,
    SPresentationAutomaticStatusGearPanel,
    SPresentationAutomaticStatusGearWait,
    SPresentationAutomaticStatusTextOwner
};

enum SPresentationPath : unsigned {
    SPathTurn = 1u << 0,
    SPathAction = 1u << 1,
    SPathProperty = 1u << 2,
    SPathNumbersPanel = 1u << 3,
    SPathNumericBar = 1u << 4,
    SPathScriptEffect = 1u << 5,
    SPathAppearance = 1u << 6,
    SPathWithdrawal = 1u << 7,
    SPathWeather = 1u << 8,
    SPathHighlight = 1u << 9,
    SPathMorph = 1u << 10,
    SPathObjects = 1u << 11,
    SPathSharedScene = 1u << 12,
    SPathExtensions = 1u << 13,
    SPathRequestBusiness = 1u << 14,
    SPathEffectFrames = 1u << 15,
    SPathDepartureAnimation = 1u << 16,
    SPathAiPreview = 1u << 17,
    SPathDuel = 1u << 18,
    SPathScriptMovement = 1u << 19,
    SPathRequired = (1u << 20) - 1
};

// Returns true only when all required owner/consumer relationships are verified.
// A false result must not be reported as fully supported S presentation.
bool AppendSPresentationLayout(const BYTE* image, EngineLayout& layout,
    uintptr_t virtualBase = 0) noexcept;
bool ResolveSPresentationEntryPolicy(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t actor, uintptr_t* returnSlot,
    unsigned& kind, int& camp, bool& rScene, int scene) noexcept;
// -1 keeps the typed S/R policy; 0/1 establishes an independent native/omitted
// category scope. Script highlights belong to Dialogue, including their
// resolved-target information child, and never inherit S's camp/type filters.
int ResolveSPresentationCategoryPolicy(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t* returnSlot, int scene,
    unsigned kind = 0, int camp = -1) noexcept;
// Battle UI follows the phase, independently of the acting unit's animation
// filters: player/unknown remains native; ally/enemy may omit under Super S.
int SPresentationBattlePreviewPolicy(const EngineLayout& layout, uintptr_t gameBase,
    int scene) noexcept;
// Shared blitter: only audited duel pixels under an existing selected owner.
bool OmitSPresentationDuelBlit(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t* returnSlot, int scene) noexcept;
// Exact terminal pixels beneath mixed information/highlight controllers and
// the alternate script-effect machine. Background capture/restore is native.
bool OmitSPresentationTransientBlit(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t* returnSlot, int scene) noexcept;
// Three verified death-animation sprite calls only compose temporary pixels.
// Runs before the shared physical-sprite adapter; all other sources stay native.
bool OmitSPresentationSprite(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t* returnSlot) noexcept;
bool ShouldSkipSPresentationEntry(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t* returnSlot, bool selected) noexcept;
// Called only after the policy above selected a verified pure UI leaf. The
// numeric panel still performs its native tail cleanup; caller-owned HP/MP/XP
// settlement has not necessarily happened yet and must never be synthesized.
bool OmitSPresentationEntry(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t* returnSlot) noexcept;
