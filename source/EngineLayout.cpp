#include "EngineLayout.h"
#include <initializer_list>
#include <string.h>

namespace {
constexpr uintptr_t kOrigin = 0x400000;

bool CopyMemoryChecked(void* to, const void* from, size_t length) {
    __try {
        memcpy(to, from, length);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Match(const BYTE* at, const int* pattern, size_t length) {
    __try {
        for (size_t i = 0; i < length; ++i)
            if (pattern[i] >= 0 && at[i] != pattern[i]) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Image {
    const BYTE* data;
    DWORD size;
    uintptr_t virtualBase;
    IMAGE_SECTION_HEADER sections[96];
    WORD count;

    bool Contains(uintptr_t address, size_t length) const {
        return address >= kOrigin && address - kOrigin < size && length <= size - (address - kOrigin);
    }

    bool Executable(uintptr_t address, size_t length) const {
        if (!Contains(address, length)) return false;
        const uintptr_t rva = address - kOrigin;
        for (WORD i = 0; i < count; ++i) {
            const auto& section = sections[i];
            const DWORD bytes = section.Misc.VirtualSize > section.SizeOfRawData ?
                section.Misc.VirtualSize : section.SizeOfRawData;
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= section.VirtualAddress &&
                rva - section.VirtualAddress < bytes && length <= bytes - (rva - section.VirtualAddress)) return true;
        }
        return false;
    }

    const BYTE* At(uintptr_t address) const { return data + address - kOrigin; }

    bool Matches(uintptr_t address, std::initializer_list<int> pattern) const {
        return Contains(address, pattern.size()) && Match(At(address), pattern.begin(), pattern.size());
    }

    uintptr_t Call(uintptr_t instruction) const {
        BYTE bytes[5];
        if (!Executable(instruction, sizeof(bytes)) || !CopyMemoryChecked(bytes, At(instruction), sizeof(bytes)) ||
            bytes[0] != 0xE8) return 0;
        int32_t displacement;
        memcpy(&displacement, bytes + 1, sizeof(displacement));
        const uintptr_t target = static_cast<uint32_t>(instruction + 5 + displacement);
        return Executable(target, 3) ? target : 0;
    }

    uintptr_t Absolute(uintptr_t operand, size_t length) const {
        uint32_t address;
        if (!Contains(operand, sizeof(address)) || !CopyMemoryChecked(&address, At(operand), sizeof(address)) ||
            address < virtualBase || address - virtualBase >= size) return 0;
        const uintptr_t normalized = kOrigin + address - virtualBase;
        return Contains(normalized, length) ? normalized : 0;
    }

    uintptr_t Find(std::initializer_list<int> pattern, uintptr_t begin = 0, size_t span = 0) const {
        uintptr_t result = 0;
        for (WORD i = 0; i < count; ++i) {
            const auto& section = sections[i];
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uintptr_t start = kOrigin + section.VirtualAddress;
            size_t length = section.Misc.VirtualSize > section.SizeOfRawData ?
                section.Misc.VirtualSize : section.SizeOfRawData;
            if (!Contains(start, length) || length < pattern.size()) continue;
            uintptr_t end = start + length;
            if (begin) {
                if (start < begin) start = begin;
                if (end > begin + span) end = begin + span;
            }
            if (end <= start || end - start < pattern.size()) continue;
            for (uintptr_t at = start; at <= end - pattern.size(); ++at) {
                if (!Match(At(at), pattern.begin(), pattern.size())) continue;
                if (result) return 0;
                result = at;
            }
        }
        return result;
    }

    size_t FindCalls(uintptr_t target, uintptr_t* callers, size_t capacity) const {
        size_t found = 0;
        if (!target || !callers || !capacity) return 0;
        for (WORD i = 0; i < this->count; ++i) {
            const auto& section = sections[i];
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uintptr_t start = kOrigin + section.VirtualAddress;
            size_t length = section.Misc.VirtualSize > section.SizeOfRawData ?
                section.Misc.VirtualSize : section.SizeOfRawData;
            if (!Contains(start, length) || length < 5) continue;
            uintptr_t end = start + length - 5;
            for (uintptr_t at = start; at <= end; ++at) {
                BYTE bytes[5] = {};
                if (!CopyMemoryChecked(bytes, At(at), sizeof(bytes)) || bytes[0] != 0xE8) continue;
                int32_t displacement = 0;
                memcpy(&displacement, bytes + 1, sizeof(displacement));
                const uintptr_t callTarget = static_cast<uint32_t>(at + 5 + displacement);
                if (callTarget != target || found >= capacity) continue;
                callers[found++] = at + 5;
            }
        }
        return found;
    }
};

bool ParseImage(const BYTE* bytes, uintptr_t virtualBase, Image& image) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS32 nt;
    if (!bytes || !CopyMemoryChecked(&dos, bytes, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 0x100000 ||
        !CopyMemoryChecked(&nt, bytes + dos.e_lfanew, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.OptionalHeader.SizeOfImage < 4096 || nt.OptionalHeader.SizeOfImage > 64 * 1024 * 1024 ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96) return false;
    const size_t sectionOffset = dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        nt.FileHeader.SizeOfOptionalHeader;
    const size_t sectionBytes = nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (sectionOffset >= nt.OptionalHeader.SizeOfHeaders ||
        sectionBytes > nt.OptionalHeader.SizeOfHeaders - sectionOffset ||
        !CopyMemoryChecked(image.sections, bytes + sectionOffset, sectionBytes)) return false;
    image.data = bytes;
    image.size = nt.OptionalHeader.SizeOfImage;
    image.count = nt.FileHeader.NumberOfSections;
    image.virtualBase = virtualBase ? virtualBase : reinterpret_cast<uintptr_t>(bytes);
    return true;
}
}

static bool Resolve66(const BYTE* bytes, EngineLayout& output, uintptr_t virtualBase) {
    Image image = {};
    EngineLayout layout;
    layout.profile = EngineProfileId::Ccz66;
    if (!ParseImage(bytes, virtualBase, image)) return false;

    // The native 6.6 toggle is also the authoritative map for the extra
    // acceleration sites introduced after the old v0.7 patch.  Resolving the
    // operands keeps layout discovery working even when the native toggle was
    // left in its enabled byte state before this DLL initialized.
    const uintptr_t nativeSuper = image.Find({
        0x55,0x8B,0xEC,0xB9,-1,-1,-1,-1,0xB8,-1,-1,-1,-1,0x80,0x38,0x00,
        0x0F,0x85,-1,-1,-1,-1,0xC6,0x00,0x01,0x80,0x31,0x01,0x80,0x39,0x01,0x75,0x44});
    if (nativeSuper) {
        layout.nativeSuper = nativeSuper;
        layout.nativeSuperState = image.Absolute(nativeSuper + 4, 1);
        layout.nativeSuperBusy = image.Absolute(nativeSuper + 9, 1);
        auto pairedSite = [&](uintptr_t enabledOffset, uintptr_t disabledOffset,
            std::initializer_list<int> enabled, std::initializer_list<int> disabled,
            size_t operandOffset, size_t length) {
            if (!image.Matches(nativeSuper + enabledOffset, enabled) ||
                !image.Matches(nativeSuper + disabledOffset, disabled)) return uintptr_t(0);
            const uintptr_t first = image.Absolute(nativeSuper + enabledOffset + operandOffset, length);
            const uintptr_t second = image.Absolute(nativeSuper + disabledOffset + operandOffset, length);
            return first && first == second ? first : uintptr_t(0);
        };
        layout.sites[SiteDialog3] = pairedSite(33,101,
            {0xC7,0x05,-1,-1,-1,-1,0xC2,0x04,0x00,0x6A},
            {0xC7,0x05,-1,-1,-1,-1,0x55,0x8B,0xEC,0x6A},2,4);
        layout.sites[SiteExtraDelay] = pairedSite(43,111,
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xEB,0x06},
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xD1,0xE0},3,2);
        layout.sites[SiteWaitBranch] = pairedSite(52,120,
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xEB,0x07},
            {0x66,0xC7,0x05,-1,-1,-1,-1,0x75,0x07},3,2);
        layout.sites[SiteDialogDisplay] = pairedSite(61,129,
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xEB,0x1D},
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xE8,0x29},3,2);
        layout.sites[SiteDialog] = pairedSite(70,138,
            {0xC7,0x05,-1,-1,-1,-1,0xC2,0x0C,0x00,0xE8},
            {0xC7,0x05,-1,-1,-1,-1,0x55,0x8B,0xEC,0xE8},2,4);
        layout.sites[SiteMapText] = pairedSite(80,148,
            {0xC7,0x05,-1,-1,-1,-1,0xC2,0x10,0x00,0x6A},
            {0xC7,0x05,-1,-1,-1,-1,0x55,0x8B,0xEC,0x6A},2,4);
        layout.sites[SiteExtendedDisplay] = pairedSite(90,158,
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xEB,0xF5},
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xE8,0x56},3,2);
        const uintptr_t z = image.Find({0x83,0xF8,0x5A,0x75,0x07,0xE8,-1,-1,-1,-1,0xEB,0x36});
        if (z && image.Call(z + 5) == nativeSuper) layout.nativeSuperCall = z + 5;
        if (layout.nativeSuperState && layout.nativeSuperBusy && layout.nativeSuperCall &&
            layout.sites[SiteDialog3] && layout.sites[SiteExtraDelay] &&
            layout.sites[SiteWaitBranch] && layout.sites[SiteDialogDisplay] &&
            layout.sites[SiteDialog] && layout.sites[SiteMapText] &&
            layout.sites[SiteExtendedDisplay])
            layout.capabilities |= CapabilityNativeSuper;
    }

    uintptr_t wait = image.Find({0x55,0x8B,0xEC,0xE8,-1,-1,-1,-1,0x85,0xC0,0x75,0x07,
        0xE8,-1,-1,-1,-1,0xEB,0xF0,0x5D,0xC3});
    if (!wait) wait = image.Find({0x55,0x8B,0xEC,0xE8,-1,-1,-1,-1,0x85,0xC0,0xEB,0x07,
        0xE8,-1,-1,-1,-1,0xEB,0xF0,0x5D,0xC3});
    layout.waitWrapper = wait;
    if (wait) {
        const BYTE branch = image.At(wait)[10];
        if (branch != 0x75 && branch != 0xEB) return false;
        layout.sites[SiteWaitCall] = wait + 3;
        if (!layout.sites[SiteWaitBranch]) layout.sites[SiteWaitBranch] = wait + 10;
        if (layout.sites[SiteWaitBranch] != wait + 10) return false;
        layout.nativeStage = image.Call(wait + 3);
        layout.pump = image.Call(wait + 12);
        // Validate the known presentation callers directly. A whole-image call
        // enumeration can fill its capacity with unrelated waits before reaching
        // later movement/strategy routines.
        struct SafeCaller { uintptr_t legacyReturn; unsigned char kind; };
        static const SafeCaller safeCallers[] = {
            {0x401F6F,1},{0x401FAE,1},{0x403096,2},{0x4030CC,2},
            {0x40D43F,4},{0x41DB77,0},{0x421A20,1},{0x423981,3},
            {0x423C63,3},{0x4249F7,4},{0x42C537,4},{0x42C6BC,5},
            {0x4371B1,1},{0x43721C,1},{0x453777,0},{0x45380B,0},{0x4549DA,2}
        };
        for (const SafeCaller& safe : safeCallers) {
            if (image.Call(safe.legacyReturn - 5) != wait) continue;
            layout.waitCallers[layout.waitCallerCount] = safe.legacyReturn;
            layout.waitKinds[layout.waitCallerCount++] = safe.kind;
        }
    }

    const uintptr_t stage = layout.nativeStage ? image.Find({0x55,0x8B,0xEC,0x0F,0xB7,0x05,-1,-1,-1,-1,
        0x0F,0xB6,0xC8,0x85,0xC9,0x75,0x04,0x8B,0xC1,0xEB,0x04,0xC1,0xE8,0x08,0x40}, layout.nativeStage, 128) : 0;
    if (stage == layout.nativeStage && stage) {
        layout.speed = image.Absolute(stage + 6, 2);
        const uintptr_t block = image.Find({0xBA,-1,-1,-1,-1,0xA0,-1,-1,-1,-1,
            0x8A,0x80,-1,-1,-1,-1,0x3C,0x32,0x7E,0x04,0x2C,0x32,0xEB,0x14,
            0xFE,0x02,0x38,0x02,0x7C,0x04,0x33,0xC0,0xEB,0x02,0xB0,0x01}, stage, 128);
        const uintptr_t pacing = image.Find({0x50,0xE8,-1,-1,-1,-1,0x83,0x7D,0xFC,0x01,0x73,0x0C,
            0xE8,-1,-1,-1,-1,0xF7,0xD8,0x1B,0xC0,0x40,0xEB,0x02,0xB0,0x01,0x8B,0xE5,0x5D,0xC3}, stage, 128);
        if (block && pacing) {
            layout.phaseCounter = image.Absolute(block + 1, 1);
            layout.phase = image.Absolute(block + 6, 1);
            layout.phaseTable = image.Absolute(block + 12, 3);
            layout.sleep = image.Call(pacing + 1);
            layout.deadline = image.Call(pacing + 12);
            if (layout.speed && layout.phase && layout.phaseTable && layout.phaseCounter &&
                layout.sleep && layout.deadline && layout.phaseTable + 3 == layout.phaseCounter &&
                layout.nativeStage) {
                layout.capabilities |= CapabilityPhase;
                if (layout.pump && layout.waitCallerCount) layout.capabilities |= CapabilitySuperS;
            }
        }
    }

    if (!layout.sites[SiteDialog]) layout.sites[SiteDialog] = image.Find({0x55,0x8B,0xEC,0xE8,-1,-1,-1,-1,
        0x85,0xC0,0x74,0x28,0x8B,0x4D,0x0C,0xE8,-1,-1,-1,-1,0x85,0xC0,0x75,0x09,
        0x81,0x7D,0x0C,0,0x04,0,0,0x73,0x13,0xFF,0x75,0x10,0xFF,0x75,0x0C,
        0xFF,0x75,0x08,0xE8,-1,-1,-1,-1});
    if (!layout.sites[SiteMapText]) layout.sites[SiteMapText] = image.Find({0x55,0x8B,0xEC,0x6A,0,0x51,
        0xB8,-1,-1,-1,-1,0xE8,-1,-1,-1,-1,0x50,0x8B,0x4D,0xF8,0x83,0x79,0x68,0,
        0x74,0x3C,0x6A,0,0xE8,-1,-1,-1,-1,0x6A,0x01,0x6A,0x32,0x6A,0x50});
    if (layout.sites[SiteDialog]) layout.capabilities |= CapabilitySuperDialog;
    if (layout.sites[SiteMapText]) layout.capabilities |= CapabilitySuperMapText;
    layout.sites[SiteDelay] = image.Find({0x55,0x8B,0xEC,0xFF,0x75,0x08,0xE8,-1,-1,-1,-1,
        0xE8,-1,-1,-1,-1,0xF7,0xD8,0x1B,0xC0,0x40,0x85,0xC0,0x75,0x07,
        0xE8,-1,-1,-1,-1,0xEB,0xEB,0x5D,0xC2,0x04,0});
    if (layout.sites[SiteDelay]) layout.capabilities |= CapabilitySuperDelay;
    const uintptr_t r = image.Find({0x68,0xFF,0,0,0,0x6A,0,0xE8,-1,-1,-1,-1,
        0xE8,-1,-1,-1,-1,0xA1,-1,-1,-1,-1,0x83,0xE0,0x6E,0x85,0xC0,0x74,0x9E});
    layout.sites[SiteRWait] = r ? r + 12 : 0;
    if (layout.sites[SiteRWait] && layout.sites[SiteWaitCall] && image.Call(layout.sites[SiteRWait]) == wait)
        layout.capabilities |= CapabilitySuperR;
    const uintptr_t key = image.Find({0x83,0xF8,-1,0x75,0x0F,0x8B,0x4D,0xFC,0x80,0x39,0,0x75,0x68,
        0xE8,-1,-1,-1,-1,0xEB,0x61,0x83,0xF8,0x30,0x72,0x5C,0x83,0xF8,0x35,0x73,0x22});
    layout.sites[SiteInputModifier] = key ? key + 2 : 0;
    if (layout.sites[SiteInputModifier]) layout.capabilities |= CapabilityInput;
    if (layout.sites[SiteDelay] && layout.deadline && layout.pump &&
        image.Call(layout.sites[SiteDelay] + 11) != layout.deadline ||
        layout.sites[SiteDelay] && layout.pump && image.Call(layout.sites[SiteDelay] + 25) != layout.pump)
        layout.capabilities &= ~CapabilitySuperDelay;

    const uintptr_t dialog3Handler = image.Find({0x55,0x8B,0xEC,0x6A,0x05,0x8B,0x4D,0x08,
        0xE8,-1,-1,-1,-1,0x85,0xC0,0x7D,0x07,0xB8,0x05,0,0,0,0xEB,0x09,
        0x50,0xE8,-1,-1,-1,-1,0x6A,0x01,0x58,0x5D,0xC3});
    const uintptr_t dialog3Target = dialog3Handler ? image.Call(dialog3Handler + 25) : 0;
    if (!layout.sites[SiteDialog3]) layout.sites[SiteDialog3] = dialog3Target;
    if (dialog3Target && dialog3Target == layout.sites[SiteDialog3])
        layout.capabilities |= CapabilityDialog3;
    const uintptr_t extraDelay = image.Find({0x55,0x8B,0xEC,0x6A,0x04,0x8B,0x4D,0x08,
        0xE8,-1,-1,-1,-1,0x3D,0,0,0,0x80,0x75,0x07,0xB8,0x05,0,0,0,0xEB,0x11,
        0x85,0xC0,0x74,0x08,0xD1,0xE0,0x50,0xE8,-1,-1,-1,-1,
        0xB8,0x01,0,0,0,0x5D,0xC3});
    if (!layout.sites[SiteExtraDelay] && extraDelay) layout.sites[SiteExtraDelay] = extraDelay + 31;
    if (extraDelay && layout.sites[SiteExtraDelay] == extraDelay + 31)
        layout.capabilities |= CapabilityExtraDelay;
    const uintptr_t display = image.Find({0xE8,-1,-1,-1,-1,0x85,0xC0,0x74,0x16,
        0x83,0x75,0xFC,0x01,0xFF,0x75,0xFC,0xE8,-1,-1,-1,-1,0x83,0xC4,0x04,
        0x6A,0x03,0xE8,-1,-1,-1,-1});
    if (!layout.sites[SiteDialogDisplay]) layout.sites[SiteDialogDisplay] = display;
    if (display && display == layout.sites[SiteDialogDisplay])
        layout.capabilities |= CapabilityDialogDisplay;
    const uintptr_t extended = image.Find({0xE8,-1,-1,-1,-1,0x85,0xC0,0x74,0xEE,
        0x83,0x75,0xFC,0x01,0xFF,0x75,0xFC,0xE8,-1,-1,-1,-1,0x83,0xC4,0x04});
    if (!layout.sites[SiteExtendedDisplay]) layout.sites[SiteExtendedDisplay] = extended;
    if (extended && extended == layout.sites[SiteExtendedDisplay])
        layout.capabilities |= CapabilityExtendedDisplay;
    if (!layout.capabilities) return false;
    output = layout;
    return true;
}

static bool Resolve65(const BYTE* bytes, EngineLayout& output, uintptr_t virtualBase) {
    Image image = {};
    EngineLayout layout;
    layout.profile = EngineProfileId::Ccz65;
    if (!ParseImage(bytes, virtualBase, image)) return false;

    // The six 6.5 toggle sites and its ECX timer ABI are independent of 6.6.
    // Decode both enabled/disabled stores so native-on images stay discoverable.
    const uintptr_t nativeSuper = image.Find({
        0x55,0x8B,0xEC,0xB9,-1,-1,-1,-1,0xB8,-1,-1,-1,-1,0x80,0x38,0x00,
        0x0F,0x85,-1,-1,-1,-1,0xC6,0x00,0x01,0x80,0x31,0x01,0x80,0x39,0x01,0x75,0x3A});
    if (nativeSuper) {
        layout.nativeSuper = nativeSuper;
        layout.nativeSuperState = image.Absolute(nativeSuper + 4, 1);
        layout.nativeSuperBusy = image.Absolute(nativeSuper + 9, 1);
        auto pairedSite = [&](uintptr_t enabledOffset, uintptr_t disabledOffset,
            std::initializer_list<int> enabled, std::initializer_list<int> disabled,
            size_t operandOffset, size_t length) {
            if (!image.Matches(nativeSuper + enabledOffset, enabled) ||
                !image.Matches(nativeSuper + disabledOffset, disabled)) return uintptr_t(0);
            const uintptr_t first = image.Absolute(nativeSuper + enabledOffset + operandOffset, length);
            const uintptr_t second = image.Absolute(nativeSuper + disabledOffset + operandOffset, length);
            return first && first == second ? first : uintptr_t(0);
        };
        layout.sites[SiteExtraDelay] = pairedSite(33,91,
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xEB,0x06},
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xD1,0xE0},3,2);
        layout.sites[SiteWaitBranch] = pairedSite(42,100,
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xEB,0x07},
            {0x66,0xC7,0x05,-1,-1,-1,-1,0x75,0x07},3,2);
        layout.sites[SiteDialogDisplay] = pairedSite(51,109,
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xEB,0x1D},
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xE8,0x1C},3,2);
        layout.sites[SiteDialog] = pairedSite(60,118,
            {0xC7,0x05,-1,-1,-1,-1,0xC2,0x0C,0x00,0xE8},
            {0xC7,0x05,-1,-1,-1,-1,0x55,0x8B,0xEC,0xE8},2,4);
        layout.sites[SiteMapText] = pairedSite(70,128,
            {0xC7,0x05,-1,-1,-1,-1,0xC2,0x10,0x00,0x6A},
            {0xC7,0x05,-1,-1,-1,-1,0x55,0x8B,0xEC,0x6A},2,4);
        layout.sites[SiteExtendedDisplay] = pairedSite(80,138,
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xEB,0xF0},
            {0x66,0xC7,0x05,-1,-1,-1,-1,0xE8,0x49},3,2);
        const uintptr_t z = image.Find({0x83,0xF8,0x5A,0x75,0x07,0xE8,-1,-1,-1,-1,0xEB,0x37});
        if (z && image.Call(z + 5) == nativeSuper) layout.nativeSuperCall = z + 5;
        if (layout.nativeSuperState && layout.nativeSuperBusy && layout.nativeSuperCall &&
            layout.sites[SiteExtraDelay] &&
            layout.sites[SiteWaitBranch] && layout.sites[SiteDialogDisplay] &&
            layout.sites[SiteDialog] && layout.sites[SiteMapText] &&
            layout.sites[SiteExtendedDisplay])
            layout.capabilities |= CapabilityNativeSuper;
    }

    uintptr_t wait = image.Find({0x55,0x8B,0xEC,0xB9,-1,-1,-1,-1,
        0xE8,-1,-1,-1,-1,0x85,0xC0,0x75,0x07,
        0xE8,-1,-1,-1,-1,0xEB,0xEB,0x5D,0xC3});
    if (!wait) wait = image.Find({0x55,0x8B,0xEC,0xB9,-1,-1,-1,-1,
        0xE8,-1,-1,-1,-1,0x85,0xC0,0xEB,0x07,
        0xE8,-1,-1,-1,-1,0xEB,0xEB,0x5D,0xC3});
    layout.waitWrapper = wait;
    layout.stageUsesThis = true;
    // Script dispatch slot 0x410C18 is the common unimplemented handler in
    // standard 6.5. Dialog3 was added in 6.6; it has no 6.5 execution path.
    layout.dialog3Applicable = false;
    const uintptr_t unusedCommand = image.Absolute(0x410C18, 16);
    if (unusedCommand != 0x4D0000 || image.Absolute(0x410C1C, 16) != unusedCommand ||
        !image.Matches(unusedCommand, {0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90})) return false;
    if (wait) {
        layout.timerContext = image.Absolute(wait + 4, 4);
        layout.sites[SiteWaitCall] = wait + 8;
        if (!layout.sites[SiteWaitBranch]) layout.sites[SiteWaitBranch] = wait + 15;
        if (layout.sites[SiteWaitBranch] != wait + 15) return false;
        layout.nativeStage = image.Call(wait + 8);
        layout.pump = image.Call(wait + 17);
        // Validate the known presentation callers directly. A whole-image call
        // enumeration can fill its capacity with unrelated waits before reaching
        // later movement/strategy routines.
        struct SafeCaller { uintptr_t legacyReturn; unsigned char kind; };
        static const SafeCaller safeCallers[] = {
            {0x401F8D,1},{0x401FD0,1},{0x4030AF,2},{0x4030DF,2},
            {0x40D449,4},{0x41DB77,0},{0x421A2A,1},{0x423981,3},
            {0x424A0E,4},{0x42C53C,4},{0x42C6D1,5},
            {0x4371BF,1},{0x437231,1},{0x453777,0},{0x45380B,0},{0x454B76,2}
        };
        for (const SafeCaller& safe : safeCallers) {
            if (image.Call(safe.legacyReturn - 5) != wait) continue;
            layout.waitCallers[layout.waitCallerCount] = safe.legacyReturn;
            layout.waitKinds[layout.waitCallerCount++] = safe.kind;
        }
    }

    const uintptr_t stage = layout.nativeStage ? image.Find({0x55,0x8B,0xEC,0x51,0x0F,0xB7,0x05,-1,-1,-1,-1,
        0x0F,0xB6,0xC8,0x85,0xC9,0x75,0x04,0x8B,0xC1,0xEB,0x04,0xC1,0xE8,0x08,0x40}, layout.nativeStage, 128) : 0;
    if (stage == layout.nativeStage && stage) {
        layout.speed = image.Absolute(stage + 7, 2);
        const uintptr_t block = image.Find({0xBA,-1,-1,-1,-1,0xA0,-1,-1,-1,-1,
            0x8A,0x80,-1,-1,-1,-1,0x3C,0x32,0x7E,0x04,0x2C,0x32,0xEB,0x14,
            0xFE,0x02,0x38,0x02,0x7C,0x04,0x33,0xC0,0xEB,0x02,0xB0,0x01}, stage, 128);
        const uintptr_t pacing = image.Find({0x50,0xE8,-1,-1,-1,-1,0x83,0x7D,0xF8,0x01,0x73,0x0F,
            0x8B,0x4D,0xFC,0xE8,-1,-1,-1,-1,0xF7,0xD8,0x1B,0xC0,0x40,0xEB,0x02,0xB0,0x01,0x8B,0xE5,0x5D,0xC3}, stage, 128);
        if (block && pacing) {
            layout.phaseCounter = image.Absolute(block + 1, 1);
            layout.phase = image.Absolute(block + 6, 1);
            layout.phaseTable = image.Absolute(block + 12, 3);
            layout.sleep = image.Call(pacing + 1);
            layout.deadline = image.Call(pacing + 15);
            if (layout.speed && layout.phase && layout.phaseTable && layout.phaseCounter &&
                layout.sleep && layout.deadline && layout.phaseTable + 3 == layout.phaseCounter &&
                layout.nativeStage && layout.timerContext &&
                image.Matches(layout.deadline, {0x55,0x8B,0xEC,0x51,0xE8,-1,-1,-1,-1,
                    0x8B,0x4D,0xFC,0x3B,0x01})) {
                layout.capabilities |= CapabilityPhase;
                if (layout.pump && layout.waitCallerCount) layout.capabilities |= CapabilitySuperS;
            }
        }
    }

    if (!layout.sites[SiteDialog]) layout.sites[SiteDialog] = image.Find({0x55,0x8B,0xEC,0xE8,-1,-1,-1,-1,
        0x85,0xC0,0x74,0x28,0x8B,0x4D,0x0C,0xE8,-1,-1,-1,-1,0x85,0xC0,0x75,0x09,
        0x81,0x7D,0x0C,0,0x04,0,0,0x73,0x13,0xFF,0x75,0x10,0xFF,0x75,0x0C,
        0xFF,0x75,0x08,0xE8,-1,-1,-1,-1});
    if (!layout.sites[SiteMapText]) layout.sites[SiteMapText] = image.Find({0x55,0x8B,0xEC,0x6A,0,0x51,
        0xB8,-1,-1,-1,-1,0xE8,-1,-1,-1,-1,0x50,0x8B,0x4D,0xF8,0x83,0x79,0x68,0,
        0x74,0x3C,0x6A,0,0xE8,-1,-1,-1,-1,0x6A,0x01,0x6A,0x32,0x6A,0x50});
    if (layout.sites[SiteDialog]) layout.capabilities |= CapabilitySuperDialog;
    if (layout.sites[SiteMapText]) layout.capabilities |= CapabilitySuperMapText;
    layout.sites[SiteDelay] = image.Find({0x55,0x8B,0xEC,0xB9,-1,-1,-1,-1,0x51,
        0xFF,0x75,0x08,0xE8,-1,-1,-1,-1,0x8B,0x4D,0xFC,0xE8,-1,-1,-1,-1,
        0xF7,0xD8,0x1B,0xC0,0x40,0x85,0xC0,0x75,0x07,
        0xE8,-1,-1,-1,-1,0xEB,0xE8,0x8B,0xE5,0x5D,0xC2,0x04,0});
    if (layout.sites[SiteDelay]) layout.capabilities |= CapabilitySuperDelay;
    const uintptr_t r = image.Find({0x68,0xFF,0,0,0,0x6A,0,0xE8,-1,-1,-1,-1,
        0xE8,-1,-1,-1,-1,0xA1,-1,-1,-1,-1,0x83,0xE0,0x6E,0x85,0xC0,0x74,0x98});
    layout.sites[SiteRWait] = r ? r + 12 : 0;
    if (layout.sites[SiteRWait] && layout.sites[SiteWaitCall] && image.Call(layout.sites[SiteRWait]) == wait)
        layout.capabilities |= CapabilitySuperR;
    const uintptr_t key = image.Find({0x83,0xF8,-1,0x75,0x0F,0x8B,0x4D,0xFC,0x80,0x39,0,0x75,0x68,
        0xE8,-1,-1,-1,-1,0xEB,0x61,0x83,0xF8,0x30,0x72,0x5C,0x83,0xF8,0x35,0x73,0x22});
    layout.sites[SiteInputModifier] = key ? key + 2 : 0;
    if (layout.sites[SiteInputModifier]) layout.capabilities |= CapabilityInput;
    if (layout.sites[SiteDelay] && layout.deadline && layout.pump &&
        image.Call(layout.sites[SiteDelay] + 20) != layout.deadline ||
        layout.sites[SiteDelay] && layout.pump && image.Call(layout.sites[SiteDelay] + 34) != layout.pump)
        layout.capabilities &= ~CapabilitySuperDelay;

    if (layout.sites[SiteDelay] && image.Absolute(layout.sites[SiteDelay] + 4, 4) != layout.timerContext)
        layout.capabilities &= ~CapabilitySuperDelay;
    const uintptr_t extraDelay = image.Find({0x55,0x8B,0xEC,0x6A,0x04,0x8B,0x4D,0x08,
        0xE8,-1,-1,-1,-1,0x3D,0,0,0,0x80,0x75,0x07,0xB8,0x05,0,0,0,0xEB,0x11,
        0x85,0xC0,0x74,0x08,0xD1,0xE0,0x50,0xE8,-1,-1,-1,-1,
        0xB8,0x01,0,0,0,0x5D,0xC3});
    if (!layout.sites[SiteExtraDelay] && extraDelay) layout.sites[SiteExtraDelay] = extraDelay + 31;
    if (extraDelay && layout.sites[SiteExtraDelay] == extraDelay + 31)
        layout.capabilities |= CapabilityExtraDelay;
    const uintptr_t display = image.Find({0xE8,-1,-1,-1,-1,0x85,0xC0,0x74,0x16,
        0x83,0x75,0xFC,0x01,0xFF,0x75,0xFC,0xE8,-1,-1,-1,-1,0x83,0xC4,0x04,
        0x6A,0x03,0xE8,-1,-1,-1,-1});
    if (!layout.sites[SiteDialogDisplay]) layout.sites[SiteDialogDisplay] = display;
    if (display && display == layout.sites[SiteDialogDisplay])
        layout.capabilities |= CapabilityDialogDisplay;
    const uintptr_t extended = image.Find({0xE8,-1,-1,-1,-1,0x85,0xC0,0x74,0xE9,
        0x83,0x75,0xFC,0x01,0xFF,0x75,0xFC,0xE8,-1,-1,-1,-1,0x83,0xC4,0x04});
    if (!layout.sites[SiteExtendedDisplay]) layout.sites[SiteExtendedDisplay] = extended;
    if (extended && extended == layout.sites[SiteExtendedDisplay])
        layout.capabilities |= CapabilityExtendedDisplay;
    if (!layout.capabilities) return false;
    output = layout;
    return true;
}

namespace {
void SetTemplates(EngineLayout& layout) {
    const BYTE originals[EngineSiteCount][8] = {
        {0x55,0x8B,0xEC,0xE8},{0x55,0x8B,0xEC,0x6A},{0x55,0x8B,0xEC},
        {},{},{0x10},{0x55,0x8B,0xEC,0x6A},{0xD1,0xE0},{0x75,0x07},
        {0xE8,0x29},{0xE8,0x56}
    };
    const BYTE enabled[EngineSiteCount][8] = {
        {0xC2,0x0C,0,0xE8},{0xC2,0x10,0,0x6A},{0xC2,0x04,0},
        {0x90,0x90,0x90,0x90,0x90},{},{0x10},{0xC2,0x04,0,0x6A},
        {0xEB,0x06},{0xEB,0x07},{0xEB,0x1D},{0xEB,0xF5}
    };
    memcpy(layout.siteOriginal, originals, sizeof(originals));
    memcpy(layout.siteEnabled, enabled, sizeof(enabled));
    const size_t native[] = {SiteDialog3,SiteExtraDelay,SiteWaitBranch,SiteDialogDisplay,
        SiteDialog,SiteMapText,SiteExtendedDisplay};
    for (size_t site : native) layout.nativeSiteMask |= 1u << site;
    if (layout.profile == EngineProfileId::Ccz65) {
        layout.nativeSiteMask &= ~(1u << SiteDialog3);
        layout.siteOriginal[SiteDialogDisplay][1] = 0x1C;
        layout.siteOriginal[SiteExtendedDisplay][1] = 0x49;
        layout.siteEnabled[SiteExtendedDisplay][1] = 0xF0;
    }
}
}

bool ResolveEngineLayout(const BYTE* bytes, EngineLayout& output, uintptr_t virtualBase) {
    EngineLayout layout;
    // Family identity needs the native toggle and wait/stage relationship. A
    // partial match to common dialog code must never misidentify an engine.
    if (Resolve66(bytes, layout, virtualBase) && layout.nativeSuper && layout.waitWrapper &&
        layout.nativeStage && layout.speed && layout.phaseTable) {
        SetTemplates(layout);
        output = layout;
        return true;
    }
    layout = EngineLayout();
    if (Resolve65(bytes, layout, virtualBase) && layout.nativeSuper && layout.waitWrapper &&
        layout.nativeStage && layout.speed && layout.phaseTable && layout.timerContext) {
        SetTemplates(layout);
        output = layout;
        return true;
    }
    return false;
}
