#pragma once
#include "EngineLayout.h"

enum RPresentationRole : unsigned char {
    RPresentationNone = 0,
    RPresentationCommand,
    RPresentationCompose,
    RPresentationReset,
    RPresentationObjectReset,
    RPresentationPortraitMove,
    RPresentationMapPortraitCommand,
    RPresentationPortraitBlit,
    RPresentationStaticNumbers,
    RPresentationStaticIllustration,
    RPresentationPortraitFade,
    RPresentationEndingPerson,
    RPresentationEndingOwner,
    RPresentationEndingText
};

// Resolver is a separate translation unit for installers and offline probes.
bool AppendRPresentationLayout(const BYTE* image, EngineLayout& layout,
    uintptr_t virtualBase = 0) noexcept;
// No executable writes: the shared AnimationRuntime owns every hook.
void ConfigureRPresentation(const EngineLayout& layout, uintptr_t gameBase, bool enabled) noexcept;
void ResetRPresentation() noexcept;
uintptr_t BeginRPresentationOwner(unsigned char role, bool selected) noexcept;
void EndRPresentationOwner(uintptr_t owner) noexcept;
bool RPresentationAsyncActive() noexcept;
void RecordRPresentationAsyncWait() noexcept;
void RecordRPresentationAsyncCompose() noexcept;
void ClearRPresentationObject(uintptr_t object) noexcept;
bool ShouldSkipRPresentationEntry(const EngineLayout& layout, uintptr_t gameBase,
    const AnimationEntryLayout& entry, uintptr_t actor, uintptr_t* returnSlot,
    bool selected) noexcept;
bool RPresentationEndingCaller(const EngineLayout& layout, uintptr_t gameBase,
    uintptr_t* returnSlot) noexcept;
bool RMapSceneIdentity(const EngineLayout& layout, uintptr_t gameBase, uintptr_t script,
    unsigned opcode, int installedOrdinal, ULONGLONG installedGeneration) noexcept;
