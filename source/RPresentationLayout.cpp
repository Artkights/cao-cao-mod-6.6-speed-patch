#include "RPresentation.h"
#include <cstring>
#include <initializer_list>

namespace {
constexpr uintptr_t kOrigin = 0x400000;
struct Image {
    const BYTE* data = nullptr;
    uintptr_t base = 0;
    DWORD size = 0;
    IMAGE_SECTION_HEADER sections[96] = {};
    WORD count = 0;
    bool Has(uintptr_t at,size_t length) const noexcept {
        return at >= kOrigin && at - kOrigin < size && length <= size - (at - kOrigin);
    }
    const BYTE* At(uintptr_t at) const noexcept { return data + at - kOrigin; }
    bool Code(uintptr_t at,size_t length) const noexcept {
        if (!Has(at,length)) return false;
        for (WORD i = 0; i < count; ++i) {
            const auto& section = sections[i];
            const size_t span = section.Misc.VirtualSize > section.SizeOfRawData ? section.Misc.VirtualSize : section.SizeOfRawData;
            const uintptr_t rva = at - kOrigin;
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= section.VirtualAddress &&
                rva - section.VirtualAddress < span && length <= span - (rva - section.VirtualAddress)) return true;
        }
        return false;
    }
    bool Match(uintptr_t at,std::initializer_list<int> bytes) const noexcept {
        if (!Has(at,bytes.size())) return false;
        __try {
            size_t index = 0;
            for (int value : bytes) { if (value >= 0 && At(at)[index] != value) return false; ++index; }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    uintptr_t Absolute(uintptr_t at,size_t length) const noexcept {
        if (!Has(at,sizeof(DWORD))) return 0;
        DWORD value = 0;
        __try { memcpy(&value,At(at),sizeof(value)); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        if (value < base || value - base >= size) return 0;
        const uintptr_t result = kOrigin + value - base;
        return Has(result,length) ? result : 0;
    }
    uintptr_t Call(uintptr_t at) const noexcept {
        if (!Code(at,5) || !Match(at,{0xE8,-1,-1,-1,-1})) return 0;
        int32_t displacement = 0; memcpy(&displacement,At(at) + 1,4);
        const uintptr_t result = static_cast<uint32_t>(at + 5 + displacement);
        return Code(result,3) ? result : 0;
    }
};
bool Parse(const BYTE* bytes,uintptr_t virtualBase,Image& image) noexcept {
    __try {
        if (!bytes) return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 0x100000) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || !nt->FileHeader.NumberOfSections ||
            nt->FileHeader.NumberOfSections > 96 || nt->OptionalHeader.SizeOfImage < 4096 ||
            nt->OptionalHeader.SizeOfImage > 64 * 1024 * 1024) return false;
        const size_t sections = dos->e_lfanew + 24 + nt->FileHeader.SizeOfOptionalHeader;
        const size_t sectionBytes = nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
        if (sections > nt->OptionalHeader.SizeOfHeaders || sectionBytes > nt->OptionalHeader.SizeOfHeaders - sections) return false;
        image.data = bytes; image.size = nt->OptionalHeader.SizeOfImage;
        image.base = virtualBase ? virtualBase : reinterpret_cast<uintptr_t>(bytes);
        image.count = nt->FileHeader.NumberOfSections;
        memcpy(image.sections,bytes + sections,sectionBytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Add(const Image& image,EngineLayout& layout,uintptr_t address,unsigned char role,bool rScene = true) noexcept {
    size_t found = layout.animationEntryCount;
    for (size_t i = 0; i < layout.animationEntryCount; ++i)
        if (layout.animationEntries[i].address == address) { found = i; break; }
    if (found < layout.animationEntryCount) {
        auto& entry = layout.animationEntries[found];
        if (!entry.rScene || entry.rPresentationRole) return false;
        entry.rPresentationRole = role; return true;
    }
    unsigned char length = 0, relative = 0xFF;
    if (image.Match(address,{0x55,0x8B,0xEC,0x83,0xEC,-1})) length = 6;
    else if (image.Match(address,{0x55,0x8B,0xEC,0x6A,-1})) length = 5;
    else if (image.Match(address,{0x55,0x8B,0xEC,0x50,0x6A,-1})) length = 6;
    else if (image.Match(address,{0x55,0x8B,0xEC,0x8B,0x45,0x08})) length = 6;
    else if (image.Match(address,{0x55,0x8B,0xEC,0xFF,0x75,0x14})) length = 6;
    else if (image.Match(address,{0x55,0x8B,0xEC,0x50,0x51}) ||
        image.Match(address,{0x55,0x8B,0xEC,0x52,0x51})) length = 5;
    else if (image.Match(address,{0x55,0x8B,0xEC,0xE8,-1,-1,-1,-1})) { length = 8; relative = 3; }
    else if (image.Match(address,{0x55,0x8B,0xEC,0x68,-1,-1,-1,-1})) length = 8;
    else if (image.Match(address,{0x55,0x8B,0xEC,0x66,0xC7,0x41,0x28,0,0})) length = 9;
    if (!length || !image.Code(address,length) || layout.animationEntryCount == _countof(layout.animationEntries)) return false;
    auto& entry = layout.animationEntries[layout.animationEntryCount++];
    entry.address = address; entry.length = length; entry.relativeCallOffset = relative;
    entry.rScene = rScene; entry.rPresentationRole = role;
    memcpy(entry.original,image.At(address),length);
    return true;
}
}
bool AppendRPresentationLayout(const BYTE* bytes,EngineLayout& layout,uintptr_t virtualBase) noexcept {
    layout.rPresentationVerified = false;
    layout.rObjectBase = layout.rAsyncWaitReturn = layout.rFinalRefresh = layout.rComposeAdvance = 0;
    layout.rScriptContext = layout.rCommandReturn = 0;
    const bool is64 = layout.profile == EngineProfileId::Ccz64;
    const bool is66 = layout.profile == EngineProfileId::Ccz66;
    Image image;
    if (!Parse(bytes,virtualBase,image) || !layout.animationPresent ||
        !image.Match(0x410A17,{0xFF,0x14,0x85,-1,-1,-1,-1}) ||
        image.Absolute(0x410A1A,0x7A * 4) != 0x410A38 ||
        (is64 ? !image.Match(0x4109EF,{0x0F,0xB7,0xC0,0x50,0x8B,0x55,0xFC}) :
            !image.Match(0x4109F3,{0x88,0x42,0x2E})) ||
        !image.Match(0x410A13,{0x52,0x83,0xE8,2}) ||
        !image.Match(0x410A1E,{0x83,0xC4,4}) ||
        !image.Match(0x417F8F,{0xFF,0x75,0xD4,0xFF,0x75,0xCC,0xB9}) ||
        image.Absolute(0x417F96,0x40) != 0x492F90 ||
        image.Call(0x417F9A) != 0x40FE3D ||
        !image.Match(0x426AA8,{0x55,0x8B,0xEC,0x0F,0xB7,0x45,0x08,0xC1,0xE0,0x06,0x05,-1,-1,-1,-1,0x5D,0xC2,4,0}) ||
        !image.Match(0x4061B7,{0x55,0x8B,0xEC,0xE8,-1,-1,-1,-1,0xE8,-1,-1,-1,-1,0xE8,-1,-1,-1,-1,0xE8}) ||
        image.Call(0x4061BA) != 0x429C2A || image.Call(0x4061C4) != 0x41E5DB ||
        image.Call(0x4061DD) != 0x41E5F6 || !image.Match(0x4061E2,{0x5D,0xC3}) ||
        !image.Match(layout.profile == EngineProfileId::Ccz66 ? 0x424223 : 0x4051BB,{0x55,0x8B,0xEC,0x66,0xC7,0x41,0x28,0,0,0x5D,0xC3})) return false;
    if (!image.Match(0x418E59,{0x55,0x8B,0xEC,0x0F,0xB7,0x41,0x30,0x5D,0xC3}) ||
        image.Call(is64 ? 0x429C89 : 0x429C7D) != 0x418E59 ||
        image.Call(is64 ? 0x429C97 : 0x429C8B) != 0x42A361 ||
        image.Call(is64 ? 0x429CA5 : 0x429C99) != 0x40B1C9 ||
        !image.Match(0x40B1C9,{0x55,0x8B,0xEC,0x6A,4,0x8B,0x4D,8,0xE8}) ||
        !image.Match(0x40B1D6,{0x8B,0x4D,8,0x0F,0xB7,0x41,0x3C,0x50,0xE8}) ||
        !image.Match(0x40B1E3,{0x68,0xFF,0xFF,0,0,0x8B,0x4D,8,0xE8})) return false;
    if (is64 ? !image.Match(0x42A361,{0x55,0x8B,0xEC,0x83,0xEC,0x28,0x89,0x4D,0xDC}) :
        !image.Match(0x42A361,{0x55,0x8B,0xEC,0x51,0x6A,0,0x83,0xEC,0x14,0x0F,0xB7,1})) return false;
    const uintptr_t objects = image.Absolute(0x426AB3,128 * 0x40);
    const uintptr_t compose = image.Call(0x4061C9);
    const uintptr_t expectedCompose = is64 ? 0x427E1D : 0x427E21;
    if (objects != 0x49CF68 || compose != expectedCompose ||
        !image.Match(compose,{0x55,0x8B,0xEC,0x68,-1,-1,-1,-1,0xFF,0x35,-1,-1,-1,-1,0x6A,0x28}) ||
        image.Call(is64 ? 0x427E52 : 0x427E56) != 0x47D281 ||
        image.Call(is66 ? 0x427F08 : 0x427EF1) != 0x47D281 ||
        !image.Match(is66 ? 0x427F0D : 0x427EF6,{0x8B,0xE5,0x5D,0xC3})) return false;
    const uintptr_t mainCall = is66 ? 0x429953 : 0x429954;
    const uintptr_t mainWait = is66 ? 0x429964 : 0x429965;
    if (image.Call(mainCall) != 0x4061B7 || image.Call(mainCall + 12) != 0x4298B8 ||
        image.Call(mainWait) != layout.waitWrapper ||
        !image.Match(mainWait + 5,{0xA1,-1,-1,-1,-1,0x83,0xE0,0x6E})) return false;
    // Exact dispatch identities, independently scoped from dialogue/map text.
    // 1E is a verified no-op returning 2, so it needs no executable hook.
    struct Command { unsigned opcode; uintptr_t address; };
    const Command commands[] = {
        {0x1C,0x413E49},{0x1D,0x413EC7},{0x1F,is66 ? 0x42B391u : 0x402348u},
        {0x20,is66 ? 0x416FD6u : 0x416FD8u},{0x21,is66 ? 0x413EFCu : 0x413EF5u},
        {0x22,is66 ? 0x42C3C7u : (is64 ? 0x42F66Du : 0x42F59Au)},
        {0x27,0x413D13},{0x29,0x417143},{0x2A,0x4171BC},{0x2B,0x4172EC},
        {0x30,0x411DAE},{0x31,0x413522},{0x32,0x4133B2},{0x33,0x411E79},
        {0x34,0x411EDE},{0x35,0x411F2D}
    };
    const size_t originalCount = layout.animationEntryCount;
    for (const auto& command : commands) {
        const bool portrait = command.opcode >= 0x29 && command.opcode <= 0x2B;
        if (image.Absolute(0x410A38 + (command.opcode - 2) * 4,8) != command.address ||
            !Add(image,layout,command.address,portrait ? RPresentationMapPortraitCommand : RPresentationCommand)) goto failed;
    }
    {
        const uintptr_t repaint = image.Absolute(0x410A38 + (0x1E - 2) * 4,4);
        if (!(is66 ? image.Match(repaint,{0x6A,2,0x58,0xC3}) : image.Match(repaint,{0x55,0x8B,0xEC,0x6A,2,0x58,0x5D,0xC3}))) goto failed;
        if (!image.Match(0x47D8C8,{0xFF,0x14,0x85,-1,-1,-1,-1}) ||
            image.Absolute(0x47D8CB,26 * 4) != 0x47D8DF) goto failed;
        const unsigned extensions[] = {9,14,15,25,28};
        for (unsigned sub : extensions) {
            if (is64 && sub == 28) continue; // Native 6.4's table ends at 25.
            const uintptr_t handler = image.Absolute(0x47D8DF + sub * 4,8);
            const uintptr_t expected = sub == 9 ? (is64 ? 0x41456Du : 0x414571u) :
                (sub == 14 ? (is66 ? 0x40DC06u : 0x40DC49u) :
                (sub == 15 ? (is66 ? 0x40DCB3u : 0x40DCAFu) :
                (sub == 25 ? 0x425B66u : 0x425BF2u)));
            if (handler != expected || !Add(image,layout,handler,RPresentationCommand)) goto failed;
        }
        // Reset runs native and invalidates object generations; individual
        // clear/reuse does the same without waiting for a scene change.
        const uintptr_t reset = is66 ? 0x426BB2 : 0x426BB0;
        if (!image.Match(reset,{0x55,0x8B,0xEC,0x68,-1,-1,-1,-1,0x6A,0}) ||
            image.Absolute(reset + 4,128 * 0x40) != objects ||
            !Add(image,layout,reset,RPresentationReset,false) ||
            !Add(image,layout,is66 ? 0x424223 : 0x4051BB,RPresentationObjectReset,false) ||
            !Add(image,layout,compose,RPresentationCompose)) goto failed;
    }
    {
        const uintptr_t finalRefresh = is66 ? 0x41C39D : 0x41C977;
        if (!image.Match(finalRefresh,{0x55,0x8B,0xEC,0xE8,-1,-1,-1,-1,0xE8}) ||
            image.Call(finalRefresh + 3) != 0x41E5DB || image.Call(finalRefresh + 8) != 0x40F59F ||
            image.Call(finalRefresh + 28) != 0x41E5F6 || !image.Match(finalRefresh + 33,{0x5D,0xC3}) ||
            image.Call(is66 ? 0x40F5B8 : 0x40F5C5) != compose) goto failed;
        layout.rFinalRefresh = finalRefresh;
        if (is66) {
            if (image.Call(0x427EF5) != 0x40505E ||
                !image.Match(0x40505E,{0x55,0x8B,0xEC,0xB8,-1,-1,-1,-1,0x50,0x80,0x38,0}) ||
                !image.Match(0x4050F0,{0x8B,0xE5,0x5D,0xC3})) goto failed;
            layout.rComposeAdvance = 0x40505E;
        }
    }
    {
        const uintptr_t move = is66 ? 0x432ADF : 0x432AD6;
        const uintptr_t save = is66 ? 0x4359B7 : 0x432CD6;
        const uintptr_t draw = is66 ? 0x432DD6 : 0x432DC8;
        const uintptr_t show = is66 ? 0x42C4A5 : 0x432D55;
        const uintptr_t hide = is66 ? 0x403CBA : 0x403CE1;
        const uintptr_t fade = is66 ? 0x402F98 : 0x402F90;
        if (image.Call(0x417227) != move || image.Call(is66 ? 0x432AF0 : 0x432AED) != 0x40A33F ||
            image.Call(is66 ? 0x432C40 : 0x432C44) != 0x41E5DB ||
            image.Call(is66 ? 0x432C4D : 0x432C51) != save ||
            image.Call(is66 ? 0x432C7C : 0x432C80) != save ||
            image.Call(is66 ? 0x432C89 : 0x432C8D) != draw ||
            image.Call(is66 ? 0x432C8E : 0x432C92) != 0x41E5F6 ||
            image.Call(is66 ? 0x432CA8 : 0x432CAC) != (is64 ? 0x435BD6u : 0x423971u) ||
            !image.Match(is66 ? 0x432CB5 : 0x432CB9,{0xC2,0x0C,0}) ||
            image.Call(is66 ? 0x4359E5 : 0x432D1B) != 0x47E239 ||
            image.Call(is66 ? 0x4359EC : 0x432D22) != 0x47E09B ||
            image.Call(0x4171AE) != show || image.Call(is66 ? 0x42C4DA : 0x432D92) != draw ||
            image.Call(0x41731A) != hide || image.Call(is66 ? 0x403CF8 : 0x403D1F) != save ||
            image.Call(0x432E2E) != fade || image.Call(is66 ? 0x403CE6 : 0x403D0D) != fade ||
            image.Call(0x402FE8) != (is66 ? 0x401C61u : 0x401C72u) ||
            !image.Match(0x402FED,{0x8B,0xE5,0x5D,0xC2,8,0}) ||
            image.Call(0x432E4A) != 0x47E09B || !image.Match(0x432E59,{0xC2,8,0}) ||
            !image.Match(0x47E09B,{0x55,0x8B,0xEC,0x83,0xEC,0x20}) ||
            !image.Match(0x47E1DB,{0x8B,0xE5,0x5D,0xC2,0x14,0}) ||
            !Add(image,layout,move,RPresentationPortraitMove)) goto failed;
        layout.animationEntries[layout.animationEntryCount - 1].argumentBytes = 12;
        if (!Add(image,layout,0x47E09B,RPresentationPortraitBlit,false)) goto failed;
        layout.animationEntries[layout.animationEntryCount - 1].argumentBytes = 20;
        if (!Add(image,layout,fade,RPresentationPortraitFade,false)) goto failed;
        layout.animationEntries[layout.animationEntryCount - 1].argumentBytes = 8;
    }
    {
        // The mixed ending sequence is entered at this one audited native
        // call. Do not replace its text parsing/cursor, resources or audio.
        // The shared text leaf is only gated inside this exact live owner.
        const uintptr_t ending=is66 ? 0x4317ABu : 0x430470u;
        const uintptr_t text=is66 ? 0x41E997u : 0x41E9B4u;
        if (image.Call(is66 ? 0x42BF06u : 0x426E50u)!=ending ||
            !image.Match(ending,{0x55,0x8B,0xEC,0x6A,0xFF,0x68}) ||
            image.Call(is66 ? 0x43185Au : 0x430554u)!=(is66 ? 0x431543u : 0x431504u) ||
            image.Call(is66 ? 0x43187Du : 0x430577u)!=(is66 ? 0x431967u : 0x432546u) ||
            image.Call(is66 ? 0x43188Bu : 0x430588u)!=(is66 ? 0x431A0Au : 0x431A3Eu) ||
            !(is66 ? image.Match(text,{0x55,0x8B,0xEC,0xFF,0x75,0x14}) :
                image.Match(text,{0x55,0x8B,0xEC,0x83,0xEC,-1})) ||
            !image.Match(is66 ? 0x41EA63u : 0x41EC13u,{0xC2,0x1C,0}) ||
            image.Call(is66 ? 0x4317A2u : 0x431863u)!=text ||
            image.Call(is66 ? 0x431B2Cu : 0x431BEFu)!=0x47E09B ||
            image.Call(is66 ? 0x431B31u : 0x431BF4u)!=0x40BAAA ||
            !Add(image,layout,ending,RPresentationEndingOwner)) goto failed;
        layout.animationEntries[layout.animationEntryCount-1].argumentBytes=4;
        if (!Add(image,layout,text,RPresentationEndingText)) goto failed;
        layout.animationEntries[layout.animationEntryCount-1].argumentBytes=28;
    }
    {
        // 72-9 writes a deferred ending-person list. Its actual renderer has
        // no scenario/business write or input: scratch graphics, statistics
        // text, waits and balanced frame transactions only. The mixed outer
        // ending controller, list preparation and sentinel remain native.
        const uintptr_t person=is66 ? 0x431C19 : 0x431E11;
        const uintptr_t caller=is66 ? 0x431868 : 0x430562;
        if (image.Call(caller)!=person ||
            !image.Match(person,{0x55,0x8B,0xEC,0x68,0xA0,0,0,0,0x68}) ||
            !image.Match(person+0x11,{0x8B,0x45,8,0x8B,4,0x85}) ||
            image.Absolute(person+0x17,128*4)!=0x4B2B30 ||
            !image.Match(person+0x1B,{0x83,0xF8,0xFF,0x0F,0x84}) ||
            !image.Match(is66 ? 0x431FB6 : 0x4321F6,{0x33,0xC0,0x8B,0xE5,0x5D,0xC2,4,0}) ||
            image.Call(is66 ? 0x431C98 : 0x431E90)!=0x4789D6 ||
            image.Call(is66 ? 0x431FB1 : 0x4321F1)!=(is66 ? 0x401C61u : 0x401C72u) ||
            !Add(image,layout,person,RPresentationEndingPerson)) goto failed;
        layout.animationEntries[layout.animationEntryCount-1].argumentBytes=4;
    }
    {
        const uintptr_t numbers = is64 ? 0x425C7D : 0x425C84;
        const uintptr_t numberCall = is64 ? 0x427B6B : 0x427BB3;
        if (image.Call(numberCall) != numbers ||
            !image.Match(is64 ? 0x425D34 : 0x425D39,{0x8B,0xE5,0x5D,0xC2,8,0}) ||
            image.Call(is64 ? 0x425D2D : 0x425D2F) != 0x47E09B ||
            !Add(image,layout,numbers,RPresentationStaticNumbers)) goto failed;
        layout.animationEntries[layout.animationEntryCount - 1].argumentBytes = 8;
        if (!is64) {
            if (image.Call(0x427B94) != 0x426BEB || image.Call(0x426BEE) != 0x40154A ||
                image.Call(0x426C01) != 0x47E239 || image.Call(0x426C1B) != 0x40690C ||
                image.Call(0x426C2F) != 0x47E09B || !image.Match(0x426C35,{0x8B,0xE5,0x5D,0xC2,0x18,0}) ||
                !Add(image,layout,0x426BEB,RPresentationStaticIllustration)) goto failed;
            layout.animationEntries[layout.animationEntryCount - 1].argumentBytes = 24;
        }
    }
    layout.rObjectBase = objects;
    // 6.4 has no verified current-opcode store at script+2E. Retain its
    // established R/S paths without granting this additional state-zero scope.
    layout.rScriptContext = is64 ? 0 : 0x492F90;
    layout.rCommandReturn = is64 ? 0 : 0x410A1E;
    layout.rAsyncWaitReturn = mainWait + 5;
    layout.rPresentationVerified = true;
    return true;
failed:
    layout.rScriptContext = layout.rCommandReturn = 0;
    layout.rObjectBase = layout.rAsyncWaitReturn = layout.rFinalRefresh = layout.rComposeAdvance = 0;
    layout.animationEntryCount = originalCount;
    for (size_t i = 0; i < originalCount; ++i) layout.animationEntries[i].rPresentationRole = 0;
    return false;
}
