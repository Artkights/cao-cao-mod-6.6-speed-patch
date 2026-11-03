#include "SPresentation.h"
#include "RPresentation.h"
#include <cstring>
#include <initializer_list>

namespace {
constexpr uintptr_t kOrigin = 0x400000;
bool Copy(void* destination, const void* source, size_t length) noexcept {
    __try { memcpy(destination, source, length); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct Image {
    const BYTE* bytes = nullptr;
    DWORD size = 0;
    uintptr_t base = 0;
    IMAGE_SECTION_HEADER sections[96] = {};
    WORD count = 0;
    bool Contains(uintptr_t at, size_t length) const noexcept {
        return at >= kOrigin && at - kOrigin < size && length <= size - (at - kOrigin);
    }
    bool Executable(uintptr_t at, size_t length) const noexcept {
        if (!Contains(at, length)) return false;
        const uintptr_t rva = at - kOrigin;
        for (WORD i = 0; i < count; ++i) {
            const auto& section = sections[i];
            const DWORD span = (section.Misc.VirtualSize > section.SizeOfRawData) ?
                section.Misc.VirtualSize : section.SizeOfRawData;
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= section.VirtualAddress &&
                rva - section.VirtualAddress < span && length <= span - (rva - section.VirtualAddress)) return true;
        }
        return false;
    }
    const BYTE* At(uintptr_t at) const noexcept { return bytes + at - kOrigin; }
    bool Match(uintptr_t at, std::initializer_list<int> pattern) const noexcept {
        if (!Executable(at, pattern.size())) return false;
        BYTE buffer[32] = {};
        if (pattern.size() > sizeof(buffer) || !Copy(buffer, At(at), pattern.size())) return false;
        size_t i = 0;
        for (int byte : pattern) { if (byte >= 0 && buffer[i] != byte) return false; ++i; }
        return true;
    }
    uintptr_t Call(uintptr_t at) const noexcept {
        BYTE opcode = 0;
        int32_t displacement = 0;
        if (!Executable(at, 5) || !Copy(&opcode, At(at), 1) || opcode != 0xE8 ||
            !Copy(&displacement, At(at + 1), sizeof(displacement))) return 0;
        const uintptr_t target = static_cast<uint32_t>(at + 5 + displacement);
        return Executable(target, 3) ? target : 0;
    }
    uintptr_t Absolute(uintptr_t operand) const noexcept {
        uint32_t address = 0;
        if (!Contains(operand, sizeof(address)) || !Copy(&address, At(operand), sizeof(address)) ||
            address < base || address - base >= size) return 0;
        return kOrigin + address - base;
    }
    bool Dispatch(unsigned opcode, uintptr_t handler) const noexcept {
        return opcode >= 2 && opcode <= 0x7B && Absolute(0x410A38 + (opcode - 2) * 4) == handler;
    }
};
bool Parse(const BYTE* bytes, uintptr_t base, Image& image) noexcept {
    IMAGE_DOS_HEADER dos = {};
    IMAGE_NT_HEADERS32 nt = {};
    if (!bytes || !Copy(&dos, bytes, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 0x100000 ||
        !Copy(&nt, bytes + dos.e_lfanew, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.OptionalHeader.SizeOfImage < 0x86000 || nt.OptionalHeader.SizeOfImage > 64 * 1024 * 1024 ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > _countof(image.sections)) return false;
    const size_t offset = dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    const size_t sectionBytes = nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (offset >= nt.OptionalHeader.SizeOfHeaders || sectionBytes > nt.OptionalHeader.SizeOfHeaders - offset ||
        !Copy(image.sections, bytes + offset, sectionBytes)) return false;
    image.bytes = bytes;
    image.size = nt.OptionalHeader.SizeOfImage;
    image.count = nt.FileHeader.NumberOfSections;
    image.base = base ? base : reinterpret_cast<uintptr_t>(bytes);
    return true;
}
bool Add(const Image& image, EngineLayout& layout, uintptr_t at, unsigned char length,
    SPresentationRole role, unsigned char kind, unsigned char arguments = 0) noexcept {
    if (length < 5 || length > sizeof(AnimationEntryLayout::original) || !image.Executable(at, length)) return false;
    AnimationEntryLayout* item = nullptr;
    for (size_t i = 0; i < layout.animationEntryCount; ++i) {
        if (layout.animationEntries[i].address != at) continue;
        item = &layout.animationEntries[i];
        if (item->sPresentationRole && item->sPresentationRole != role) return false;
        break;
    }
    if (!item) {
        if (layout.animationEntryCount >= _countof(layout.animationEntries)) return false;
        item = &layout.animationEntries[layout.animationEntryCount++];
        *item = {};
        item->address = at;
        item->length = length;
        if (!Copy(item->original, image.At(at), length)) { --layout.animationEntryCount; return false; }
    }
    item->sPresentationRole = role;
    item->kind = kind;
    item->actorSource = 0;
    item->argumentBytes = arguments;
    if ((at == 0x413E49 || at == 0x413EC7 || at == 0x4036F9 || at == 0x40FAA0) && item->length == 8)
        item->relativeCallOffset = 3;
    return true;
}
bool Has(const EngineLayout& layout, uintptr_t address, unsigned char kind) noexcept {
    for (size_t i = 0; i < layout.animationEntryCount; ++i)
        if (layout.animationEntries[i].address == address && layout.animationEntries[i].kind == kind) return true;
    return false;
}
}

bool AppendSPresentationLayout(const BYTE* bytes, EngineLayout& layout, uintptr_t virtualBase) noexcept {
    layout.sPresentationVerified = false;
    layout.sPresentationPathMask = 0;
    Image image;
    if (!Parse(bytes, virtualBase, image) || !layout.animationUnitBase || !layout.phase) return false;
    const bool is66 = layout.profile == EngineProfileId::Ccz66;
    const bool is64 = layout.profile == EngineProfileId::Ccz64;
    unsigned paths = 0;
    bool terminalPixels = !is66;
    bool itemStatus = !is66, phaseStatus = !is66;
    layout.animationScriptPhysicalSpriteCallerCount = 0;

    // Automatic battle status/forecast panels and the 5B rectangle are mixed
    // controllers: track their complete native lifetime and omit only their
    // independently verified terminal pixel consumers below.
    // Each caller list is recorded from the exact native CALL operand; a MOD
    // that moves or changes any of these keeps that path native.
    auto record = [&](uintptr_t site, uintptr_t target, uintptr_t* callers, size_t& count,
        size_t capacity) {
        if (count >= capacity || image.Call(site) != target) return false;
        callers[count++] = site + 5;
        return true;
    };
    const uintptr_t autoInformation[] = {0x44E2FB, 0x454431,
        is64 ? 0x455332u : is66 ? 0x4552FCu : 0x4552F5u};
    for (uintptr_t site : autoInformation)
        record(site, 0x464A32, layout.animationAutoInformationCallers,
            layout.animationAutoInformationCallerCount, _countof(layout.animationAutoInformationCallers));
    const uintptr_t mapInformation[] = {is66 ? 0x433557u : 0x433661u,
        is66 ? 0x43361Cu : 0x4338D1u, is66 ? 0x433C4Fu : 0x43466Bu};
    for (uintptr_t site : mapInformation)
        record(site, 0x464A32, layout.animationMapInformationCallers,
            layout.animationMapInformationCallerCount, _countof(layout.animationMapInformationCallers));
    if (record(0x454427, 0x44049E, layout.animationDirectPanelCallers,
        layout.animationDirectPanelCallerCount, _countof(layout.animationDirectPanelCallers))) {
        // 6.4 begins 55 8B EC 83 EC 30: include the complete SUB ESP,30h.
        // 6.5/2.2.0 begin with their original NOP cave and 6.6 with PUSH -1;
        // both of those retain their independently audited five-byte prefix.
        Add(image, layout, 0x44049E, is64 ? 6 : 5, SPresentationInformationPanel, 3, 12);
    }
    if (is64) {
        const uintptr_t aiPanels[] = {0x4415DD, 0x4416A2};
        for (uintptr_t site : aiPanels)
            record(site, 0x44049E, layout.animationAiPreviewPanelCallers,
                layout.animationAiPreviewPanelCallerCount,
                _countof(layout.animationAiPreviewPanelCallers));
    } else if (is66) {
        // The shared-frame range fragment ends in this ordinary panel call.
        // Track the complete panel function; never hook the borrowed-EBP
        // 4415D4 fragment as a standalone function.
        record(0x441626,0x44049E,layout.animationAiPreviewPanelCallers,
            layout.animationAiPreviewPanelCallerCount,_countof(layout.animationAiPreviewPanelCallers));
    }
    if (!is64) {
        const uintptr_t move = is66 ? 0x4415A7u : 0x44156Du;
        const uintptr_t range = is66 ? 0x4415D4u : 0x44159Au;
        const uintptr_t moveSites[] = {is66 ? 0x4414F7u : 0x4414ECu,
            is66 ? 0x441502u : 0x4414FDu};
        const uintptr_t rangeSite = is66 ? 0x441509u : 0x441504u;
        size_t moveCalls = 0, rangeCalls = 0;
        uintptr_t moveCallers[4] = {}, rangeCallers[4] = {};
        for (uintptr_t site : moveSites)
            record(site, move, moveCallers, moveCalls, _countof(moveCallers));
        record(rangeSite, range, rangeCallers, rangeCalls, _countof(rangeCallers));
        // These are CALL fragments which borrow 441488's EBP, not functions
        // with independent frames. A generic owner/exit bridge corrupts their
        // caller's stack; preserve both fragments and their state writes.
        if (moveCalls == _countof(moveSites) && rangeCalls == 1) {
            for (size_t i = 0; i < moveCalls; ++i)
                layout.animationAiPreviewMoveCallers[layout.animationAiPreviewMoveCallerCount++] = moveCallers[i];
            for (size_t i = 0; i < rangeCalls; ++i)
                layout.animationAiPreviewRangeCallers[layout.animationAiPreviewRangeCallerCount++] = rangeCallers[i];
        }
    } else {
        const uintptr_t arrows[] = {0x441540, 0x44158E, 0x441649, 0x441709};
        for (uintptr_t site : arrows)
            record(site, 0x4539EF, layout.animationPreviewArrowCallers,
                layout.animationPreviewArrowCallerCount, _countof(layout.animationPreviewArrowCallers));
        if (layout.animationPreviewArrowCallerCount == _countof(arrows))
            Add(image, layout, 0x4539EF, 8, SPresentationPreviewArrow, 0, 12);
    }
    const uintptr_t highlightFillSite = is66 ? 0x41673Cu : 0x416758u;
    if (record(highlightFillSite, 0x45218F, layout.animationHighlightFillCallers,
        layout.animationHighlightFillCallerCount, _countof(layout.animationHighlightFillCallers)))
        // Include the entire MOV ECX,imm32 at +3 (five bytes).
        Add(image, layout, 0x45218F, 8, SPresentationHighlightFill, 0, 16);

    // Native formatting, text cursor movement and palette/resource updates
    // remain in 40E4BC/40ED56/40F5D9. Only their terminal bitmap glyph copy is
    // omitted under the exact information-panel owner.
    if (is66) terminalPixels = layout.animationAiPreviewPanelCallerCount == 1 &&
        image.Match(0x47E382,{0x55,0x8B,0xEC,0x83,0xEC,0x28}) &&
        image.Match(0x47E5E6,{0xC2,0x14,0}) &&
        image.Call(0x40F6A2) == 0x47E382 && image.Call(0x40F7AA) == 0x47E382 &&
        image.Call(0x40F7CE) == 0x47E382 && image.Call(0x40F7F0) == 0x47E382 &&
        image.Call(0x40F826) == 0x47E382 && image.Call(0x451955) == 0x47E09B &&
        image.Call(0x45195C) == 0x4296B4 &&
        Add(image,layout,0x47E382,6,SPresentationPanelGlyph,3,20);

    const uintptr_t turn = is66 ? 0x4575B8 : 0x457428;
    if (image.Dispatch(0x4F, 0x415F47) && image.Call(is66 ? 0x415F89 : 0x415FD5) == turn &&
        (is66 ? image.Match(turn, {0x55,0x8B,0xEC,0x6A,0,0x6A,0,0x83,0xEC,0x10}) &&
            image.Call(0x457766) == 0x423971 && image.Match(0x45776E, {0xC2,0x18,0}) :
            image.Match(turn, {0x55,0x8B,0xEC,0x83,0xEC,0x1C,0x89,0x4D,0xE8}) &&
            image.Call(0x457774) == layout.waitWrapper && image.Match(0x45777C, {0xC2,0x18,0})) &&
        Add(image, layout, turn, is66 ? 5 : 6, SPresentationTurn, 0)) paths |= SPathTurn;

    // All of these are pure 451347 sprite submissions. The shared 6.6
    // helpers are eligible only under the actual selected 50 physical owner,
    // never merely because another action calls the same drawing routine.
    const uintptr_t scriptSpriteCalls[] = {0x457AB0,0x457C6C,0x457D57,0x457E42,
        0x458000,0x458278,0x458542,0x458732,0x458879,0x4589C9,0x458B22,
        0x458E35,0x458EA0,0x4590B6};
    const uintptr_t scriptSpriteCalls66[] = {0x4578D0,0x419858,0x457D63};
    const uintptr_t* spriteCalls = is66 ? scriptSpriteCalls66 : scriptSpriteCalls;
    const size_t spriteCount = is66 ? _countof(scriptSpriteCalls66) : _countof(scriptSpriteCalls);
    bool scriptSprites = layout.animationNeutralMapSprite && layout.animationMapDraw == 0x451347;
    for (size_t i = 0; i < spriteCount; ++i)
        scriptSprites = scriptSprites && image.Call(spriteCalls[i]) == 0x451347;
    if (scriptSprites && image.Dispatch(0x50, is66 ? 0x415FEE : 0x415FE9) &&
        image.Call(is66 ? 0x416051 : 0x41605A) == 0x45778F &&
        (is66 ? image.Match(0x45778F, {0x55,0x8B,0xEC,0x33,0xC0,0x50,0x50,0x50}) &&
            image.Match(0x457B53, {0xC2,0x10,0}) :
            image.Match(0x45778F, {0x55,0x8B,0xEC,0x6A,0xFF,0x68}) &&
            image.Call(0x457972) == layout.waitWrapper && image.Match(0x45912A, {0xC2,0x10,0})) &&
        Add(image, layout, 0x45778F, 5, SPresentationAction, 3)) {
        paths |= SPathAction;
        for (size_t i = 0; i < spriteCount; ++i)
            layout.animationScriptPhysicalSpriteCallers[layout.animationScriptPhysicalSpriteCallerCount++] = spriteCalls[i] + 5;
    }

    const uintptr_t propertyArea = is66 ? 0x45ADE7 : 0x45ADED;
    if (image.Dispatch(0x4D, 0x415551) && image.Match(0x45AA37, {0x55,0x8B,0xEC,0x83,0xEC,0x0C}) &&
        image.Call(is66 ? 0x45AAAC : 0x45AAA7) == 0x43E6AA && image.Call(is66 ? 0x45AB0E : 0x45AB09) == 0x43F6DB &&
        image.Call(is66 ? 0x45AB2A : 0x45AB25) == 0x43F715 && image.Match(0x45AB46, {0xC2,0x14,0}) &&
        image.Match(propertyArea, {0x55,0x8B,0xEC,0x6A,0,0x83,0xEC,0x18}) &&
        image.Call(is66 ? 0x45AE69 : 0x45AEF0) == 0x45AA37 && image.Match(is66 ? 0x45AECE : 0x45AF55, {0xC2,0x1C,0}) &&
        Add(image, layout, 0x45AA37, 6, SPresentationProperty, 3) &&
        Add(image, layout, propertyArea, 5, SPresentationPropertyArea, 3)) paths |= SPathProperty;

    bool numericSources = true;
    for (const auto& source : kSPresentationNumericSources)
        if (source.profiles & SPresentationNumericProfile(layout.profile))
            numericSources = numericSources && image.Call(source.caller - 5) == 0x450986;
    // Prove the two packed-slot sources whose first parameter is a target.
    // 6.5 reaches its argument pushes through a native extension cave.
    numericSources = numericSources && image.Match(0x4229E0,{0xFF,0x71,1,0xFF,0x71,0x0C,0x8B,0xC8}) &&
        image.Match(0x422102,{0xFF,0x75,0x0C,0xFF,0x75,8,0x33,0xC9}) &&
        (layout.profile == EngineProfileId::Ccz64 ?
            image.Match(0x42206F,{0xFF,0x30,0xFF,0x75,0xF0}) && image.Call(0x422074) == 0x4220D3 &&
            image.Match(0x4220C0,{0xFF,0x71,1,0xFF,0x31,0x8B,0x4D,0xEC}) && image.Call(0x4220C8) == 0x4220D3 :
            image.Match(0x422099,{0x8B,0x4D,8,0xFF,0x71,4,0x8B,0x4D,0xFC,0xFF,0x71,4})) &&
        image.Match(0x422497,{0xFF,0x70,2,0xFF,0x70,3}) &&
        image.Match(0x41C651,{0x8A,0x51,2,0x88,0x51,3}) &&
        (is66 ? image.Match(0x40CD10,{0x8B,0x4D,0x2C,0xFF,0x31,0xFF,0x71,1,0x8B,0x4D,8}) :
        layout.profile == EngineProfileId::Ccz64 ?
            image.Match(0x405AD6,{0x8B,0x4D,0xF4,0xFF,0x31,0xFF,0x71,1,0x8B,0x4D,0xE4}) :
            image.Match(0x40D198,{0x8B,0x4D,0xF4,0xFF,0x31,0xFF,0x71,1,0xE9,0x11,0x89,0xFF,0xFF}) &&
            image.Match(0x405AB6,{0x8B,0x4D,0xD0}));
    const bool panel = numericSources && image.Match(0x450033, {0x55,0x8B,0xEC,0x83,0xEC,0x50}) &&
        image.Match(0x45004C, {0x80,0x3D,-1,-1,-1,-1,0,0x0F,0x85}) &&
        image.Absolute(0x45004E) == 0x505FDC &&
        image.Match(0x450986, {0x55,0x8B,0xEC,0x81,0xF9,0,1,0,0}) &&
        image.Call(0x4509CF) == 0x450033 && image.Match(0x4509D4, {0x90,0x90,0x5D,0xC2,0x20,0}) &&
        image.Match(0x45090F, {0xB9}) && image.Absolute(0x450910) == 0x4B4250 &&
        image.Call(0x450914) == 0x45461C &&
        image.Match(0x450919, {0xB9}) && image.Absolute(0x45091A) == 0x4B4250 &&
        image.Call(0x45091E) == 0x453AFF &&
        image.Match(0x45092B, {0x8B,0xE5,0x5D,0xC2,0x20,0});
    if (panel && Add(image, layout, 0x450986, 9, SPresentationNumbers, 4) &&
        Add(image, layout, 0x450033, 6, SPresentationNumbersPanel, 4, 32)) paths |= SPathNumbersPanel;
    const uintptr_t barCalls[] = {0x45048D,0x4504B5,0x450510,0x4506B8,0x4506EC,0x450752};
    bool bars = panel && image.Match(0x4509DA, {0x55,0x8B,0xEC,0x83,0xEC,0x10}) &&
        image.Call(0x450A28) == 0x43902B && image.Match(0x450B92, {0xC2,0x14,0});
    for (uintptr_t call : barCalls) bars = bars && image.Call(call) == 0x4509DA;
    if (bars && Add(image, layout, 0x4509DA, 6, SPresentationNumericBar, 4, 20)) paths |= SPathNumericBar;

    const uintptr_t effect = is66 ? 0x414071 : 0x41406F;
    if (image.Dispatch(0x6B, effect) && image.Match(effect, {0x55,0x8B,0xEC,0x6A,0,0x83,0xEC,0x1C}) &&
        image.Call(0x414187) == 0x401E5D && image.Call(0x414191) == 0x4016CD &&
        image.Match(0x41419B, {0x8B,0xE5,0x5D,0xC3}) &&
        Add(image, layout, effect, 5, SPresentationScriptEffect, 2)) paths |= SPathScriptEffect;

    // 401E5D advances effect objects, sound and lifetime. Its per-object frame
    // wrapper only transforms a pixel buffer and blits it; omit that work,
    // retaining every surrounding native iteration and final resource release.
    const uintptr_t effectFrameCall = is66 ? 0x401F4C : 0x401F6D;
    if (image.Call(effectFrameCall) == 0x402396 &&
        image.Match(0x402396,{0x55,0x8B,0xEC,0x51,0x8B,0x45,0x08}) &&
        image.Call(0x4023C7) == (is66 ? 0x4819E0u : 0x47FCFDu) &&
        image.Call(0x4023F6) == 0x40690C &&
        image.Call(0x402412) == (is66 ? 0x40E905u : 0x40E8FEu) &&
        image.Call(0x402437) == 0x47E09B && image.Match(0x40243F,{0xC2,0x0C,0}) &&
        Add(image,layout,0x402396,7,SPresentationEffectFrame,3,12)) paths |= SPathEffectFrames;

    const uintptr_t appear = is66 ? 0x40216A : 0x40216C;
    const uintptr_t revive = is66 ? 0x4092C0 : 0x4092C7;
    if (image.Dispatch(0x4C, 0x414ADC) && image.Call(0x414B53) == appear &&
        image.Match(appear, {0x55,0x8B,0xEC}) &&
        image.Call(is66 ? 0x4021D3 : 0x4021E3) == 0x409229 &&
        image.Call(0x4159EE) == revive && image.Match(revive, {0x55,0x8B,0xEC}) &&
        image.Call(is66 ? 0x4093DF : (is64 ? 0x4093E9 : 0x4093D8)) == 0x43FCC9 &&
        image.Match(0x442B03, {0x55,0x8B,0xEC,0x83,0xEC,is66 ? 0x0C : 0x10}) &&
        Add(image, layout, appear, is66 ? 5 : 8, SPresentationMovementPerson, 0) &&
        Add(image, layout, revive, is64 ? 8 : 5, SPresentationMovementPerson, 0) &&
        Add(image, layout, 0x442B03, 6, SPresentationMovementUnit, 0)) paths |= SPathAppearance;

    // The outer 4C mapper may not have a deployed actor at entry. Its internal
    // controller receives that actual unit as argument zero before any frames.
    if (is66) terminalPixels = terminalPixels && (paths & SPathAppearance) &&
        image.Match(0x409229,{0x55,0x8B,0xEC,0x8B,0x45,0x0C}) &&
        image.Call(0x409268) == 0x442B03 && image.Call(0x409270) == 0x40948C &&
        image.Call(0x409285) == 0x442B03 && image.Call(0x4092A4) == 0x43FCC9 &&
        image.Call(0x4092AE) == 0x44A88A && image.Match(0x4092BD,{0xC2,8,0}) &&
        image.Call(0x409546) == 0x451347 && image.Call(0x4094D1) == 0x46D0B7 &&
        image.Match(0x40956B,{0x8B,0xE5,0x5D,0xC3}) &&
        Add(image,layout,0x409229,6,SPresentationAppearanceOwner,0,8);

    // The animated casting pose has a separate white-pixel mask. Skipping its
    // precise pure consumer retains the owner's resource, audio and restore.
    if (is66) terminalPixels = terminalPixels && image.Call(0x4219EC) == 0x40E9E5 &&
        image.Call(0x40EA1D) == 0x40690C && image.Call(0x40EA39) == 0x451913 &&
        image.Call(0x40EA59) == 0x451347 && image.Call(0x421A11) == 0x451347 &&
        image.Match(0x40690C,{0x55,0x8B,0xEC,0x6A,0}) &&
        image.Match(0x40698E,{0xC2,0x18,0}) &&
        Add(image,layout,0x40690C,5,SPresentationCastingMask,2,24);
    if (is66) terminalPixels = terminalPixels && image.Call(0x414156) == 0x416A14 &&
        image.Call(0x416A2D) == 0x40345B && image.Call(0x403469) == 0x403035 &&
        image.Call(0x40308C) == 0x402B83 && image.Call(0x4030EB) == 0x402537 &&
        image.Call(0x402CE4) == 0x47E09B && image.Call(0x402BED) == 0x47E09B &&
        image.Call(0x402CBE) == 0x47E09B && image.Call(0x4030DE) == 0x47E09B;

    if (image.Call(is66 ? 0x41572F : 0x41572E) == 0x45A7A2 &&
        image.Match(0x45A7A2, {0x55,0x8B,0xEC,0x8A,0x4D,0x14}) &&
        image.Call(0x45A7F2) == 0x441744 && image.Match(0x45A80C, {0xC2,0x10,0}) &&
        image.Match(0x441744, {0x55,0x8B,0xEC}) &&
        Add(image, layout, 0x45A7A2, 6, SPresentationMovementPerson, 0) &&
        Add(image, layout, 0x441744, is64 ? 6 : 5, SPresentationMovementUnit, 0)) paths |= SPathWithdrawal;

    const uintptr_t departureSprites[] = {is64 ? 0x441BC1u : 0x441A98u,
        is64 ? 0x441C4Eu : 0x441AEFu,is64 ? 0x441EB4u : 0x441C10u};
    bool departure = image.Call(is64 ? 0x441782 : 0x4417CE) == 0x4419B6 &&
        image.Match(0x4419B6,{0x55,0x8B,0xEC,0x6A,0x01,0x6A,0}) &&
        image.Match(is64 ? 0x441F45 : 0x441C7E,{0xC2,0x04,0});
    for (uintptr_t call : departureSprites) departure = departure && image.Call(call) == 0x451347;
    if (departure && Add(image,layout,0x4419B6,5,SPresentationDepartureAnimation,3))
        paths |= SPathDepartureAnimation;

    // AI's preview wrapper still runs all route/range/forecast calculations.
    // The final range-overlay leaf has no target computation or model write.
    const uintptr_t overlayCaller = is66 ? 0x47C637 : 0x41E33B;
    if (image.Call(0x43A9E4) == 0x441488 && image.Call(0x43ABD2) == 0x441488 &&
        image.Call(0x43B2FC) == 0x441488 &&
        image.Match(0x441488,{0x55,0x8B,0xEC}) &&
        image.Call(is66 ? 0x44153E : (is64 ? 0x4414CB : 0x4414C1)) == 0x464A32 &&
        image.Call(is64 ? 0x4414A4 : 0x44149C) == 0x45503F &&
        image.Call(is66 ? 0x4414CE : (is64 ? 0x4414B5 : 0x4414AB)) == 0x4544D3 &&
        image.Call(overlayCaller) == 0x45288B &&
        image.Match(0x45288B,{0x55,0x8B,0xEC,0x51,0x81,0xC1,0x89,0x1B,0,0}) &&
        image.Call(is66 ? 0x4528A8 : 0x4528A4) == 0x4174D4 &&
        image.Call(is66 ? 0x4528F6 : 0x4528EF) == 0x41E254 &&
        image.Match(is66 ? 0x452904 : 0x4528F9,{0xC2,0x08,0}) &&
        Add(image,layout,0x441488,is64 ? 6 : 5,SPresentationAiPreview,0) &&
        Add(image,layout,0x45288B,10,SPresentationRangeOverlay,0,8)) paths |= SPathAiPreview;

    if (image.Dispatch(0x57, 0x414AB0) && image.Call(0x414AD0) == 0x41D9D1 &&
        image.Match(0x41D9D1, {0x55,0x8B,0xEC,0x51,0x0F,0xB6,0x51,0x11}) &&
        image.Call(0x41DA02) == 0x423971 && image.Call(0x41DA0D) == 0x449F91 &&
        image.Match(0x449F91, {0x55,0x8B,0xEC,0x51,0x83,0xEC,0x18}) &&
        image.Call(0x44A069) == 0x401E5D && image.Match(0x44A0A2, {0xC2,0x04,0}) &&
        Add(image, layout, 0x41D9D1, 8, SPresentationStatusField, 3) &&
        Add(image, layout, 0x449F91, 7, SPresentationStatusField, 3)) paths |= SPathWeather;

    if (image.Dispatch(0x5B, 0x416AE3) && image.Call(0x416B41) == 0x416609 &&
        image.Match(0x416609, {0x55,0x8B,0xEC,0x68}) && image.Call(is66 ? 0x416743 : 0x41675F) == 0x423971 &&
        image.Match(is66 ? 0x416763 : 0x416779, {0xC2,0x08,0}) &&
        image.Dispatch(0x5C,0x416B54) && image.Match(0x416B54,{0x55,0x8B,0xEC,0x83,0xEC,0x0C}) &&
        image.Call(0x416C09) == 0x423971 && image.Call(0x416C27) == 0x423971 &&
        image.Match(0x416C40,{0xC3}) &&
        image.Call(0x416BE1) == 0x464A32 &&
        image.Match(0x464A32,{0x55,0x8B,0xEC,0x33,0xC0,0xFE,0xC8,0x50,0x50,is64 ? 0x40 : 0x6A,is64 ? 0x50 : 0}) &&
        image.Call(0x464A3D) == 0x44049E && image.Match(0x464A42,{0x5D,0xC3}) &&
        Add(image, layout, 0x416609, 8, SPresentationHighlight, 3) &&
        Add(image, layout, 0x416B54, 6, SPresentationTargetHighlight, 3) &&
        Add(image, layout, 0x464A32, 5, SPresentationTargetHighlightCapture, 3)) paths |= SPathHighlight;

    // Duel controllers use a separate two-person record and never enter the
    // ordinary map attack/dialogue owners. Keep every native state update,
    // sound, RNG and resource operation; scope only their visible sequence.
    const uintptr_t victory = is66 ? 0x448BAE : 0x448B96;
    const uintptr_t duel63Call = is66 ? 0x417625 : 0x417634;
    const uintptr_t duel64Call = is66 ? 0x41767C : 0x41768D;
    const uintptr_t duel65Call = is66 ? 0x4176D5 : 0x4176E8;
    const uintptr_t duel66Call = is66 ? 0x41772F : 0x417744;
    const uintptr_t poseTargets[] = {0x443CE6,0x444D08,0x444A7C,0x444939,0x444DAD,0x443DFF,
        0x443F51,0x4440F1,0x4442EA,0x444496,0x4446D1,0x444243,0x444BBF,0x443D8D,0x444EEA,0x445943,0x444BBF};
    const uintptr_t attackTargets[] = {0x44663A,0x4469EB,0x446DB6};
    const uintptr_t specialTargets[] = {0x44711F,0x4475A2,0x448004};
    bool sharedDuelBlit = false;
    for (size_t i = 0; i < layout.animationEntryCount; ++i) {
        const auto& entry = layout.animationEntries[i];
        if (entry.address == 0x47E09B && entry.rPresentationRole == RPresentationPortraitBlit &&
            entry.argumentBytes == 20) sharedDuelBlit = true;
    }
    // R registration owns this shared leaf transactionally. If an R identity
    // check rolled it back, native CALL relationships alone cannot provide
    // duel pixel omission and must not advertise the complete S capability.
    bool duel = sharedDuelBlit && image.Match(0x47E09B,{0x55,0x8B,0xEC,0x83,0xEC,0x20}) &&
        image.Match(0x47E1DB,{0x8B,0xE5,0x5D,0xC2,0x14,0}) &&
        image.Dispatch(0x5F,is64 ? 0x404DFE : 0x41583F) &&
        image.Call(is64 ? 0x404E06 : 0x415847) == 0x4498E5 &&
        image.Dispatch(0x60,is64 ? 0x41743D : 0x417447) && image.Call(0x4174AF) == victory &&
        image.Absolute(0x4174AB) == 0x4B3C80 &&
        image.Dispatch(0x61,0x4174C0) && image.Call(0x4174C8) == 0x448CB3 &&
        image.Dispatch(0x62,0x4111F4) && image.Call(0x411218) == 0x448E55 &&
        image.Dispatch(0x63,0x4175C2) && image.Call(duel63Call) == 0x4431AF &&
        image.Dispatch(0x64,is66 ? 0x417636 : 0x417645) && image.Call(duel64Call) == 0x448497 &&
        image.Dispatch(0x65,is66 ? 0x41768C : 0x41769D) && image.Call(duel65Call) == 0x448389 &&
        image.Dispatch(0x66,is66 ? 0x4176E6 : 0x4176F9) && image.Call(duel66Call) == 0x44867A &&
        image.Dispatch(0x68,is66 ? 0x4103EE : 0x4103F0) &&
        image.Call(is66 ? 0x410455 : 0x41045C) == 0x4499A3 && image.Call(0x449A35) == 0x4486F4 &&
        image.Match(0x4498E5,{0x55,0x8B,0xEC,0x51,0xFF,0x71,0x2C}) && image.Match(0x4499A2,{0xC3}) &&
        image.Match(0x4486F4,{0x55,0x8B,0xEC,0x6A,0,0x51}) && image.Match(0x448A08,{0xC3}) &&
        image.Match(victory,{0x55,0x8B,0xEC,0x6A,0x0A}) && image.Match(0x448CB0,{0xC2,0x0C,0}) &&
        image.Match(0x448CB3,{0x55,0x8B,0xEC,0x83,0xEC,0x10}) && image.Match(0x448D90,{0xC3}) &&
        image.Match(0x448E55,{0x55,0x8B,0xEC,0x83,0xEC,0x34}) && image.Match(0x448F50,{0xC2,4,0}) &&
        image.Match(0x4431AF,{0x55,0x8B,0xEC,0x83,0xEC,0x4C}) && image.Match(0x443531,{0xC2,0x0C,0}) &&
        image.Match(0x448497,{0x55,0x8B,0xEC,0x83,0xEC,0x10}) && image.Match(0x4484E4,{0xC2,8,0}) &&
        image.Match(0x448389,{0x55,0x8B,0xEC,0x83,0xEC,8}) && image.Match(0x44842D,{0xC2,0x0C,0}) &&
        image.Match(0x44867A,{0x55,0x8B,0xEC,0x83,0xEC,0x10}) && image.Match(0x4486E5,{0xC2,0x0C,0}) &&
        image.Match(0x443AD5,{0x55,0x8B,0xEC,0x83,0xEC,0x1C}) && image.Match(0x443B7F,{0xC2,8,0}) &&
        image.Call(0x448D3C) == 0x443AD5 && image.Call(0x448D49) == 0x443AD5 &&
        image.Call(0x448D6F) == 0x443AD5 && image.Call(0x448D7C) == 0x443AD5 &&
        image.Call(0x449975) == 0x450986 && image.Call(0x448C98) == 0x448497 &&
        image.Call(0x448CA8) == 0x4431AF && image.Call(is66 ? 0x448C71 : 0x448C6B) == 0x443C1D;
    // 443C1D deliberately remains native: it updates the clipped animation
    // rectangle before its shared blit. Only the final pixels are omitted.
    const uintptr_t duelBlits[] = {0x443C8E,0x443B17,0x443B77,0x443323,0x44334B,
        0x443373,0x448976,0x448D2A,0x448E15};
    for (uintptr_t call : duelBlits) duel = duel && image.Call(call) == 0x47E09B;
    for (size_t i = 0; i < _countof(poseTargets); ++i)
        duel = duel && image.Absolute(0x4484E7 + i * 4) == poseTargets[i];
    for (size_t i = 0; i < _countof(attackTargets); ++i)
        duel = duel && image.Absolute(0x448430 + i * 4) == attackTargets[i] &&
            image.Absolute(0x4486E8 + i * 4) == specialTargets[i];
    if (duel && Add(image,layout,0x4486F4,5,SPresentationDuelOpen,0) &&
        Add(image,layout,0x4498E5,7,SPresentationDuelFinish,0) &&
        Add(image,layout,victory,5,SPresentationDuelAppearance,0) &&
        Add(image,layout,0x448CB3,6,SPresentationDuelResult,5) &&
        Add(image,layout,0x448E55,6,SPresentationDuelDefeat,3) &&
        Add(image,layout,0x4431AF,6,SPresentationDuelDialogue,5) &&
        Add(image,layout,0x448497,6,SPresentationDuelPose,3) &&
        Add(image,layout,0x448389,6,SPresentationDuelAttack,1) &&
        Add(image,layout,0x44867A,6,SPresentationDuelSpecial,1)) paths |= SPathDuel;

    if (image.Dispatch(0x6D,is66 ? 0x410D3B : 0x410D21) && image.Call(0x410E5C) == 0x4594EA &&
        image.Call(is66 ? 0x4595D2 : 0x4595CF) == 0x43AD3B &&
        image.Call(is66 ? 0x43B2A1 : 0x43B29E) == 0x43A959 &&
        image.Match(0x43A959,{0x55,0x8B,0xEC,0x51,0x6A,0}) &&
        image.Call(0x43A9D2) == 0x45503F && image.Call(0x43A9E4) == 0x441488 &&
        Add(image,layout,0x43A959,6,SPresentationMovementUnit,0)) paths |= SPathScriptMovement;

    if (image.Call(is66 ? 0x414C96 : 0x414C9E) == 0x43FE2C &&
        image.Match(0x43FE2C, {0x55,0x8B,0xEC,0x6A,0xFF,0x68}) &&
        image.Call(is66 ? 0x43FEBD : 0x43FED2) == 0x43FD34 && image.Match(is66 ? 0x43FED9 : 0x43FEF2, {0xC3}) &&
        Add(image, layout, 0x43FE2C, 5, SPresentationStatusUnit, 3)) paths |= SPathMorph;

    if (image.Call(0x416876) == 0x456670 && image.Call(0x413F9E) == 0x456670 &&
        image.Match(0x456670, {0x55,0x8B,0xEC,0x90,0x90}) &&
        image.Match(0x456ADE, {0xC2,0x10,0}) &&
        Add(image, layout, 0x456670, 5, SPresentationStatusField, 3)) paths |= SPathObjects;

    // Exact R/S command identities share one descriptor and select their
    // scene policy dynamically. Every mixed handler still runs its business.
    const uintptr_t camera = is66 ? 0x42B391 : 0x402348;
    const uintptr_t sceneAnimation = is66 ? 0x42C3C7 : (is64 ? 0x42F66D : 0x42F59A);
    const uintptr_t animationBodyCall = is66 ? 0x42C3E5 : (is64 ? 0x42F68B : 0x42F5B8);
    if (image.Dispatch(0x1C,0x413E49) && image.Call(0x413E4C) == 0x40B913 &&
        image.Call(0x413E9E) == 0x41E5F6 && image.Dispatch(0x1D,0x413EC7) &&
        image.Call(0x413EDF) == 0x426A83 && image.Call(0x413EE4) == 0x426C3B &&
        image.Dispatch(0x1F,camera) && image.Call(is66 ? 0x42B3CA : 0x402388) == 0x45503F &&
        Add(image,layout,0x413E49,8,SPresentationSharedStatus,3) &&
        Add(image,layout,0x413EC7,8,SPresentationSharedStatus,3) &&
        Add(image,layout,camera,is66 ? 5 : 6,SPresentationSharedMovement,0) &&
        image.Dispatch(0x22,sceneAnimation) && image.Match(sceneAnimation,{0x55,0x8B,0xEC,0x6A,0x1B}) &&
        image.Call(animationBodyCall) == (is66 ? 0x42F5C9u : (is64 ? 0x42F5EDu : 0x42F5C4u)) &&
        Add(image,layout,sceneAnimation,5,SPresentationSharedStatus,3)) paths |= SPathSharedScene;

    const uintptr_t illustration = is66 ? 0x4295FB : 0x404F17;
    const uintptr_t brightness = is66 ? 0x40DC06 : 0x40DC49;
    const uintptr_t animated = is66 ? 0x40DCB3 : 0x40DCAF;
    const uintptr_t casting = is66 ? 0x406D64 : 0x406D66;
    if (image.Absolute(0x47D8DF + 12 * 4) == illustration &&
        image.Absolute(0x47D8DF + 14 * 4) == brightness && image.Absolute(0x47D8DF + 15 * 4) == animated &&
        image.Match(illustration,{0x55,0x8B,0xEC,0x6A,0}) &&
        image.Match(brightness,{0x55,0x8B,0xEC,0x6A,0}) &&
        image.Match(animated,{0x55,0x8B,0xEC,0x6A,0}) &&
        image.Call(is66 ? 0x406E0A : 0x406E0E) == 0x401E5D &&
        image.Call(is66 ? 0x406E17 : 0x406E1B) == 0x4016CD &&
        image.Match(casting,{0x55,0x8B,0xEC,0xB9}) &&
        Add(image,layout,illustration,5,SPresentationSharedStatus,3) &&
        Add(image,layout,brightness,5,SPresentationSharedStatus,3) &&
        Add(image,layout,animated,5,SPresentationSharedStatus,3) &&
        Add(image,layout,casting,8,SPresentationSharedEffectRouter,2)) paths |= SPathExtensions;

    // The historical "effect request" is actually an interactive character
    // dialog and a business query aggregation. Neither is presentation-only.
    const uintptr_t request = is66 ? 0x41FF21 : 0x44CE96;
    const uintptr_t aggregate = is66 ? 0x41E34E : 0x41E348;
    bool requestBusiness = image.Dispatch(0x71,0x403936) &&
        image.Call(request + 3) == 0x422676 && image.Match(request + 0x43,{0xFF,0x15}) &&
        image.Match(request + 0x4C,{0xC3}) &&
        image.Call(is66 ? 0x41E367 : 0x41E368) == 0x41A280 &&
        image.Match(is66 ? 0x41E3B2 : 0x41E3B0,{0xC3});
    for (unsigned index = 0; index < 6; ++index)
        requestBusiness = requestBusiness && image.Absolute(0x409693 + index * 4) == (index == 2 ? aggregate : request);
    if (requestBusiness) paths |= SPathRequestBusiness;

    if (is66) {
        // Item use and phase recovery/buffs share business with animation.
        // Verify the complete native owner prefixes, all item/status sources,
        // the precise temporary pixels, final neutral frame and cleanup ABI.
        bool verified = image.Match(0x41C74D,{0x55,0x8B,0xEC,0x51,0x83,0xEC,0x3C}) &&
            image.Match(0x41C963,{0xC6,0x40,0x1C,1}) && image.Match(0x41C96A,{0xC2,4,0}) &&
            image.Match(0x427CBC,{0x55,0x8B,0xEC,0xB9,0xB0,0x17,0x49,0}) &&
            image.Match(0x427CFC,{0xC2,8,0}) && image.Match(0x43E6AA,{0x55,0x8B,0xEC,0x6A,0}) &&
            image.Match(0x43E75B,{0xC2,0x0C,0}) &&
            image.Match(0x41C5C7,{0x55,0x8B,0xEC,0xFF,0x75,0x14}) &&
            image.Match(0x41C60C,{0xC2,0x10,0}) &&
            image.Match(0x401C61,{0x55,0x8B,0xEC,0x6A,0,0x6A,0,0x8B,0x45,0x10,0x0F,0xAF,0x45,0x14,0x50,0x50}) &&
            image.Match(0x401D30,{0xC2,0x24,0}) &&
            image.Match(0x451913,{0x55,0x8B,0xEC,0x6A,0x28,0xB9,0x50,0x42,0x4B,0}) &&
            image.Match(0x429D4C,{0x55,0x8B,0xEC,0x8B,1,0x5D,0xC3}) &&
            image.Match(0x47D309,{0xC2,0x10,0}) && image.Match(0x4296C7,{0xC2,4,0});
        const uintptr_t itemSources[] = {0x41C690,0x41C70D,0x41CD25,0x41CD9D,0x41D211,
            0x41D4C1,0x422401,0x422541,0x432AC9,0x432D5F};
        for (uintptr_t call : itemSources) verified = verified && image.Call(call) == 0x41C74D;
        const uintptr_t statusSources[] = {0x422599,0x42FC73,0x432AD6,0x432D6C,0x43CA0D,0x43CA53,0x43D0CD};
        for (uintptr_t call : statusSources) verified = verified && image.Call(call) == 0x427CBC;
        const uintptr_t edges[][2] = {
            {0x41C81D,0x41C5C7},{0x41C84C,0x41C5C7},{0x41C879,0x451347},
            {0x41C912,0x401C61},{0x41C5DA,0x451347},{0x41C5FE,0x451913},
            {0x41C606,0x41E5F6},{0x451955,0x47E09B},{0x45195C,0x4296B4},
            {0x45191D,0x429D4C},{0x451939,0x47D281},{0x451941,0x4296B4},
            {0x401CFF,0x47E09B},{0x401D1F,0x423971},{0x41C951,0x401E5D},{0x41C95B,0x4016CD},
            {0x427CD3,0x452F9D},{0x427CE3,0x401E5D},{0x427CE9,0x4016CD},{0x427CF6,0x4158DC},
            {0x412DB0,0x43E6AA},{0x45AAAC,0x43E6AA},{0x43E741,0x401E5D},{0x43E74B,0x4016CD}
        };
        for (const auto& edge : edges) verified = verified && image.Call(edge[0]) == edge[1];
        itemStatus = verified && Add(image,layout,0x41C74D,7,SPresentationItemOwner,3,4) &&
            Add(image,layout,0x427CBC,8,SPresentationStatusEffectOwner,3,8) &&
            Add(image,layout,0x43E6AA,5,SPresentationStatusValueOwner,3,12);

        // These mixed owners retain every native state/resource/experience
        // update. Entry ECX is the affected unit; only verified pixel leaves
        // below their complete native lifetime may be omitted.
        bool phaseVerified = image.Match(0x43C940,{0x55,0x8B,0xEC,0x83,0xEC,8}) &&
            image.Match(0x43CA68,{0xC3}) && image.Match(0x43CDDD,{0x55,0x8B,0xEC,0x33,0xC0}) &&
            image.Match(0x43CFCC,{0xC3}) && image.Match(0x43D038,{0x55,0x8B,0xEC,0x6A,0}) &&
            image.Match(0x43D0D5,{0xC3}) &&
            image.Match(0x43C9D6,{0x88,0x44,0x11,0x18}) &&
            image.Match(0x43CA15,{0xC6,1,0}) &&
            image.Match(0x43D07C,{0x80,0x7C,8,0x18,3}) &&
            image.Match(0x43D0BD,{0xC6,0x44,8,0x18,6}) &&
            image.Match(0x43CF7F,{0x8B,0x45,0xE4,0xFF,0x70,4}) &&
            image.Match(0x43CF90,{1,0x51,0x10}) && image.Match(0x43CF96,{1,0x51,0x14}) &&
            image.Match(0x414361,{0x55,0x8B,0xEC,3,0x4D,0x0C}) &&
            image.Match(0x41439C,{0xC2,0x10,0}) &&
            image.Match(0x45AAC3,{0x88,0x54,8,0x18}) &&
            image.Match(0x421AB6,{0x55,0x8B,0xEC,0x6A,0xFF}) &&
            image.Match(0x421C3E,{0x6A,0x30,0x6A,0x30}) &&
            image.Match(0x421C7B,{0xC6,0x40,3,0}) && image.Match(0x421CD3,{0xC2,0x0C,0}) &&
            image.Match(0x44FB73,{0x55,0x8B,0xEC,0x8B,0x4D,8}) &&
            image.Match(0x44FC41,{0xC2,0x10,0}) &&
            image.Match(0x4036F9,{0x55,0x8B,0xEC,0xE8}) && image.Match(0x403763,{0xC2,8,0}) &&
            image.Match(0x473202,{0x55,0x8B,0xEC,0x6A,0}) && image.Match(0x47338B,{0xC2,0x18,0}) &&
            image.Match(0x42C646,{0x55,0x8B,0xEC,0x83,0xEC,0x18}) && image.Match(0x42C6C4,{0xC3}) &&
            image.Match(0x4086B3,{0x83,0xC4,4}) &&
            image.Match(0x40FAA0,{0x55,0x8B,0xEC,0xE8}) && image.Match(0x40FAB7,{0xC3});
        const uintptr_t phaseEdges[][2] = {
            {0x44EE63,0x43C940},{0x44EE9A,0x43CDDD},{0x43CE0B,0x43D038},
            {0x43C9C8,0x45AA37},{0x43CA0D,0x427CBC},{0x43CA53,0x427CBC},{0x43CA60,0x4073BC},
            {0x43D04F,0x43D025},{0x43D031,0x4101D9},{0x43D089,0x414361},
            {0x414396,0x45AA37},{0x43D0CD,0x427CBC},{0x43CF85,0x450986},
            {0x43CFAA,0x4088CD},{0x43CFB7,0x4082DD},{0x43CFC4,0x4082DD},
            {0x43CE03,0x42F8D9},{0x421C33,0x4036F9},{0x421C48,0x451347},{0x421CAA,0x44FB73},{0x421CAF,0x41E5F6},
            {0x4036FC,0x41E5DB},{0x403758,0x423BD0},{0x40375D,0x41E5F6},
            {0x4129DA,0x473202},{0x408574,0x47E09B},{0x4086AE,0x42C646},{0x4086BB,0x473613},
            {0x40FAA3,0x41E5DB},{0x40FAAC,0x40E4BC},{0x40FAB1,0x41E5F6}
        };
        for (const auto& edge : phaseEdges) phaseVerified = phaseVerified && image.Call(edge[0]) == edge[1];
        const uintptr_t levelActions[] = {0x4074FA,0x408455,0x40846B,0x40847B,0x408499,0x4084A9,0x4084C0,0x4084D0,0x4084E0};
        for (uintptr_t caller : levelActions) phaseVerified = phaseVerified && image.Call(caller-5) == 0x45778F;
        const uintptr_t levelTexts[] = {0x4085C5,0x408601,0x408641,0x408686,0x40874A,0x408781,0x4087C0};
        for (uintptr_t caller : levelTexts) phaseVerified = phaseVerified && image.Call(caller-5) == 0x40FAA0;
        phaseStatus = phaseVerified && Add(image,layout,0x43C940,6,SPresentationAutomaticStatusOwner,3) &&
            Add(image,layout,0x43CDDD,5,SPresentationAutomaticStatusOwner,3) &&
            Add(image,layout,0x43D038,5,SPresentationAutomaticStatusOwner,3) &&
            Add(image,layout,0x44FB73,6,SPresentationAutomaticStatusNumbers,4,16) &&
            Add(image,layout,0x4036F9,8,SPresentationAutomaticStatusTile,3,8) &&
            Add(image,layout,0x473202,5,SPresentationAutomaticStatusGearPanel,3,24) &&
            Add(image,layout,0x42C646,6,SPresentationAutomaticStatusGearWait,3) &&
            Add(image,layout,0x40FAA0,8,SPresentationAutomaticStatusTextOwner,3);
    }

    // Existing ordinary/counterattack/strategy/status/text owners remain
    // necessary. New script owners cannot stand in for their automatic paths.
    const bool automatic = Has(layout, 0x4526E7, 1) && Has(layout, is66 ? 0x4370F3 : 0x4370F6, 1) &&
        Has(layout, 0x4536C7, 1) && Has(layout, 0x425627, 2) && Has(layout, 0x43E6AA, 3) &&
        Has(layout, is66 ? 0x442000 : 0x44201B, 0) && Has(layout, 0x44AC28, 5);
    layout.sPresentationPathMask = paths;
    layout.sPresentationVerified = automatic && paths == SPathRequired && terminalPixels && itemStatus && phaseStatus;
    return layout.sPresentationVerified;
}
