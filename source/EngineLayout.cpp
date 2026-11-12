#include "EngineLayout.h"
#include "PeImage.h"
#include "RPresentation.h"
#include "SPresentation.h"
#include <initializer_list>
#include <string.h>
#include <sstream>
#include <iomanip>

namespace {
constexpr uintptr_t kOrigin = 0x400000;
thread_local EngineAnalysisSource gAnalysisSource = EngineAnalysisSource::Disk;
thread_local uintptr_t gRuntimeMemoryBase = 0;
struct AnalysisScope {
    EngineAnalysisSource previous;
    uintptr_t previousBase;
    explicit AnalysisScope(EngineAnalysisSource source, uintptr_t memoryBase = 0)
        : previous(gAnalysisSource), previousBase(gRuntimeMemoryBase) {
        gAnalysisSource = source;
        if (memoryBase) gRuntimeMemoryBase = memoryBase;
    }
    ~AnalysisScope() { gAnalysisSource = previous; gRuntimeMemoryBase = previousBase; }
};

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
    DWORD headers;
    uintptr_t virtualBase;
    IMAGE_SECTION_HEADER sections[96];
    WORD count;
    bool runtime = false;
    uintptr_t runtimeMemoryBase = 0;
    mutable unsigned uniqueSignatures = 0;
    mutable unsigned missingSignatures = 0;
    mutable unsigned ambiguousSignatures = 0;

    bool Contains(uintptr_t address, size_t length) const {
        return address >= kOrigin && address - kOrigin < size && length <= size - (address - kOrigin);
    }

    bool Executable(uintptr_t address, size_t length) const {
        if (!Contains(address, length)) return false;
        const uintptr_t rva = address - kOrigin;
        if (runtime) {
            std::wstring error;
            return ValidateRuntimeMemory(reinterpret_cast<const void*>(runtimeMemoryBase + rva), length, true, error);
        }
        for (WORD i = 0; i < count; ++i) {
            const auto& section = sections[i];
            const DWORD bytes = PeLogicalSectionSize(section);
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= section.VirtualAddress &&
                rva - section.VirtualAddress < bytes && length <= bytes - (rva - section.VirtualAddress)) return true;
        }
        return false;
    }

    const BYTE* At(uintptr_t address) const { return data + address - kOrigin; }

    bool RawOffset(uintptr_t address, size_t length, size_t& output) const {
        if (!Contains(address, length)) return false;
        const DWORD rva = static_cast<DWORD>(address - kOrigin);
        if (rva < headers) {
            if (static_cast<uint64_t>(rva) + length > headers) return false;
            output = rva;
            return true;
        }
        for (WORD i = 0; i < count; ++i) {
            const auto& section = sections[i];
            const DWORD virtualSize = PeLogicalSectionSize(section);
            if (rva < section.VirtualAddress ||
                static_cast<uint64_t>(rva - section.VirtualAddress) + length > section.SizeOfRawData ||
                static_cast<uint64_t>(rva - section.VirtualAddress) + length > virtualSize ||
                !section.SizeOfRawData) continue;
            const uint64_t raw = static_cast<uint64_t>(section.PointerToRawData) +
                (rva - section.VirtualAddress);
            if (raw > SIZE_MAX || raw + length > SIZE_MAX) return false;
            output = static_cast<size_t>(raw);
            return true;
        }
        return false;
    }

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
        size_t anchor = pattern.size();
        int anchorByte = -1;
        size_t patternIndex = 0;
        for (int value : pattern) {
            if (value >= 0) {
                anchor = patternIndex;
                anchorByte = value;
                break;
            }
            ++patternIndex;
        }
        for (WORD i = 0; i < count; ++i) {
            const auto& section = sections[i];
            if (!runtime && !(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uintptr_t start = kOrigin + section.VirtualAddress;
            size_t length = PeLogicalSectionSize(section);
            if (!Contains(start, length) || length < pattern.size()) continue;
            uintptr_t end = start + length;
            if (begin) {
                if (start < begin) start = begin;
                if (end > begin + span) end = begin + span;
            }
            if (end <= start || end - start < pattern.size()) continue;
            const size_t candidateCount = end - start - pattern.size() + 1;
            const BYTE* sectionData = At(start);
            if (anchorByte >= 0) {
                // Most signatures have a fixed first byte.  Jump directly
                // between those anchor bytes instead of invoking the guarded
                // wildcard matcher at every offset in the executable section.
                size_t searchOffset = anchor;
                size_t remaining = candidateCount;
                while (remaining) {
                    const BYTE* hit = static_cast<const BYTE*>(memchr(
                        sectionData + searchOffset, anchorByte, remaining));
                    if (!hit) break;
                    const size_t anchorPosition =
                        static_cast<size_t>(hit - sectionData);
                    const size_t offset = anchorPosition - anchor;
                    if (Match(sectionData + offset, pattern.begin(), pattern.size())) {
                        const uintptr_t at = start + offset;
                        if (runtime && !Executable(at, pattern.size())) {
                            const size_t consumed = anchorPosition - searchOffset + 1;
                            searchOffset = anchorPosition + 1;
                            remaining -= consumed;
                            continue;
                        }
                        if (result) { ++ambiguousSignatures; return 0; }
                        result = at;
                    }
                    const size_t consumed = anchorPosition - searchOffset + 1;
                    searchOffset = anchorPosition + 1;
                    remaining -= consumed;
                }
            } else {
                for (size_t offset = 0; offset < candidateCount; ++offset) {
                    if (!Match(sectionData + offset, pattern.begin(), pattern.size())) continue;
                    const uintptr_t at = start + offset;
                    if (runtime && !Executable(at, pattern.size())) continue;
                    if (result) { ++ambiguousSignatures; return 0; }
                    result = at;
                }
            }
        }
        if (result) ++uniqueSignatures;
        else ++missingSignatures;
        return result;
    }

    size_t FindCalls(uintptr_t target, uintptr_t* callers, size_t capacity) const {
        size_t found = 0;
        if (!target || !callers || !capacity) return 0;
        for (WORD i = 0; i < this->count; ++i) {
            const auto& section = sections[i];
            if (!runtime && !(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uintptr_t start = kOrigin + section.VirtualAddress;
            size_t length = PeLogicalSectionSize(section);
            if (!Contains(start, length) || length < 5) continue;
            const BYTE* sectionData = At(start);
            const size_t candidateCount = length - 4;
            size_t searchOffset = 0;
            size_t remaining = candidateCount;
            while (remaining) {
                const BYTE* hit = static_cast<const BYTE*>(memchr(
                    sectionData + searchOffset, 0xE8, remaining));
                if (!hit) break;
                const size_t offset = static_cast<size_t>(hit - sectionData);
                const uintptr_t at = start + offset;
                BYTE bytes[5] = {};
                if (!CopyMemoryChecked(bytes, hit, sizeof(bytes))) break;
                int32_t displacement = 0;
                memcpy(&displacement, bytes + 1, sizeof(displacement));
                const uintptr_t callTarget = static_cast<uint32_t>(at + 5 + displacement);
                if (callTarget == target && found < capacity && (!runtime || Executable(at, 5)))
                    callers[found++] = at + 5;
                const size_t consumed = offset - searchOffset + 1;
                searchOffset = offset + 1;
                remaining -= consumed;
            }
        }
        return found;
    }
};

bool ParseImage(const BYTE* bytes, uintptr_t virtualBase, Image& image) {
    IMAGE_DOS_HEADER dos = {};
    if (!bytes || !CopyMemoryChecked(&dos, bytes, sizeof(dos)) || dos.e_lfanew < sizeof(dos) ||
        dos.e_lfanew > 0x100000) return false;
    IMAGE_NT_HEADERS32 nt = {};
    if (!CopyMemoryChecked(&nt, bytes + dos.e_lfanew, sizeof(nt)) ||
        nt.OptionalHeader.SizeOfHeaders > nt.OptionalHeader.SizeOfImage ||
        !nt.OptionalHeader.SizeOfHeaders || nt.OptionalHeader.SizeOfHeaders > 0x100000) return false;
    std::vector<BYTE> headerCopy(nt.OptionalHeader.SizeOfHeaders);
    if (!CopyMemoryChecked(headerCopy.data(), bytes, headerCopy.size())) return false;
    PeImageMetadata metadata;
    std::wstring error;
    if (!ParsePeImageMetadata(headerCopy.data(), headerCopy.size(), 0, false, metadata, error)) return false;
    if (!CopyMemoryChecked(image.sections, metadata.sections.data(),
        metadata.sections.size() * sizeof(IMAGE_SECTION_HEADER))) return false;
    image.data = bytes;
    image.size = metadata.nt.OptionalHeader.SizeOfImage;
    image.headers = metadata.nt.OptionalHeader.SizeOfHeaders;
    image.count = metadata.nt.FileHeader.NumberOfSections;
    image.virtualBase = virtualBase ? virtualBase : reinterpret_cast<uintptr_t>(bytes);
    image.runtime = gAnalysisSource == EngineAnalysisSource::Runtime;
    image.runtimeMemoryBase = gRuntimeMemoryBase ? gRuntimeMemoryBase : image.virtualBase;
    return true;
}

struct WaitCallerSignature {
    EngineProfileId profile;
    unsigned char kind;
    const char* context;
};

// Context starts 21 bytes before the CALL's return address. Relative CALL/JMP
// operands and absolute data operands are masked; local control-flow shape is
// retained to distinguish presentation loops with different timing semantics.
// None of these signatures depends on the caller's original image address.
const WaitCallerSignature kWaitCallerSignatures[] = {
    {EngineProfileId::Ccz65,1,"00 85 C0 74 05 33 C0 89 45 EC 83 7D EC 00 74 05 E8 ?? ?? ?? ?? FF 75 E4 8B 4D DC E8 ?? ?? ?? ?? 85 C0 74 08 8B 4D DC E8 ?? ?? ?? ?? E8 ?? ?? ??"},
    {EngineProfileId::Ccz65,1,"FF FF FF C7 45 FC FF FF FF FF 83 7D 0C 00 74 11 E8 ?? ?? ?? ?? 6A 00 6A 00 8B 4D DC E8 ?? ?? ?? ?? 8B 4D F4 64 89 0D ?? ?? ?? ?? 5F 5E 5B 8B E5"},
    {EngineProfileId::Ccz65,2,"75 EC FF 75 0C FF 75 08 8B 4D E8 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 4D E8 E8 ?? ?? ?? ?? 85 C0 74 08 8B 4D E8 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 45"},
    {EngineProfileId::Ccz65,2,"E8 FF 70 20 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? EB 9E E8 ?? ?? ?? ?? 8B 55 E8 FF 72 50 FF 72 28 FF 72 24 FF 72 60 FF 72 5C E8 ?? ?? ?? ?? FF 4D FC 6A"},
    {EngineProfileId::Ccz65,4,"E0 01 50 FF 75 0C B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? EB B1 8B 55 F4 8A 45 08 83 7D 0C 00 74 05 28 42 12 EB 03 00 42 12 0F B6 42 12 50"},
    {EngineProfileId::Ccz65,0,"00 E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 8B E5 5D C2 08 00 55 8B EC 81 EC 04 01 00 00 8B CC 68 ?? ?? ?? ??"},
    {EngineProfileId::Ccz65,1,"FF 75 E8 FF 75 E4 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 89 45 EC 6A 01 6A 27 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 80 3D ?? ?? ??"},
    {EngineProfileId::Ccz65,3,"C2 5D C2 08 00 55 8B EC FF 75 08 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 5D C2 04 00 55 8B EC B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 5A 5D C3 55"},
    {EngineProfileId::Ccz65,4,"03 45 F4 80 B8 01 04 00 00 00 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 01 6A 7C B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 02 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B"},
    {EngineProfileId::Ccz65,4,"08 00 74 11 8B 45 08 50 E8 ?? ?? ?? ?? 90 90 90 E8 ?? ?? ?? ?? 5D C2 04 00 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90"},
    {EngineProfileId::Ccz65,5,"8B 55 FC 3B 55 F0 7D 02 B1 0C 51 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? FF 4D 08 EB 80 8B E5 5D C3 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90"},
    {EngineProfileId::Ccz65,1,"F8 E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 6A 00 FF 75 08 8B 4D EC E8 ?? ?? ?? ?? 83 7D 08 02 75 73 8B 4D EC"},
    {EngineProfileId::Ccz65,1,"F8 E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 E8 ?? ?? ?? ?? 6A 01 6A 06 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 04 E8 ?? ?? ?? ?? EB AD EB 9C 8B E5"},
    {EngineProfileId::Ccz65,0,"04 E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 6A 00 FF 75 08 8B 4D EC E8 ?? ?? ?? ?? 6A 00 FF 75 08 8B 4D F8 E8"},
    {EngineProfileId::Ccz65,0,"F8 E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 E8 ?? ?? ?? ?? 6A 01 6A 06 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 04 E8 ?? ?? ?? ?? EB A5 EB 94 8B E5"},
    {EngineProfileId::Ccz65,2,"8B 45 D8 50 8B 4D E0 51 8B 4D CC E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 45 E4 5E 8B E5 5D C3 55 8B EC 83 EC 14 89 4D EC 8B 45 08 89 45 F4 8B 4D 0C 89"},
    {EngineProfileId::Ccz66,1,"00 85 C0 74 05 33 C0 89 45 EC 83 7D EC 00 74 05 E8 ?? ?? ?? ?? FF 75 E4 8B 4D F0 E8 ?? ?? ?? ?? 85 C0 74 08 8B 4D F0 E8 ?? ?? ?? ?? E8 ?? ?? ??"},
    {EngineProfileId::Ccz66,1,"45 E4 E9 ?? ?? ?? ?? FF 4D FC 83 7D 0C 00 74 11 E8 ?? ?? ?? ?? 6A 00 6A 00 8B 4D F0 E8 ?? ?? ?? ?? 8B 4D F4 64 89 0D ?? ?? ?? ?? 8B E5 5D C2 08"},
    {EngineProfileId::Ccz66,2,"75 EC FF 75 0C FF 75 08 8B 4D F0 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 4D F0 E8 ?? ?? ?? ?? 85 C0 74 10 8B 4D F0 0F B6 49 2C 83 C1 64 51 E8 ?? ?? ??"},
    {EngineProfileId::Ccz66,2,"8B 4D F0 FF 71 20 E8 ?? ?? ?? ?? FF 45 EC EB 9D E8 ?? ?? ?? ?? 8B 4D F0 FF 71 50 FF 71 28 FF 71 24 FF 71 60 FF 71 5C E8 ?? ?? ?? ?? FF 4D FC 6A"},
    {EngineProfileId::Ccz66,4,"E0 01 50 FF 75 0C B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? FF 45 F4 EB B6 8B 55 FC 8A 45 08 83 7D 0C 00 74 05 28 42 12 EB 03 00 42 12 FF 72"},
    {EngineProfileId::Ccz66,0,"00 E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 8B E5 5D C2 08 00 55 8B EC 81 EC 04 01 00 00 8B CC 68 ?? ?? ?? ??"},
    {EngineProfileId::Ccz66,1,"FF 75 E8 FF 75 E4 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 50 6A 27 E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 75 1B 8B 4D FC 0F B6"},
    {EngineProfileId::Ccz66,3,"C2 5D C2 08 00 55 8B EC FF 75 08 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 5D C2 04 00 55 8B EC B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 5A 5D C3 55"},
    {EngineProfileId::Ccz66,3,"00 E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 E8 ?? ?? ?? ?? 6A 06 E8 ?? ?? ?? ?? 5D C2 04 00 55 8B EC 83 EC 10 8D 55 F0 52 FF 35 ?? ?? ?? ??"},
    {EngineProfileId::Ccz66,4,"03 45 F4 80 B8 01 04 00 00 00 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 7C E8 ?? ?? ?? ?? 6A 02 E8 ?? ?? ?? ?? 8B 4D EC E8 ?? ?? ?? ?? 89 45 E8 50 8B"},
    {EngineProfileId::Ccz66,4,"55 8B EC 8B 45 08 85 C0 74 0B 50 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 5D C2 04 00 55 8B EC B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ??"},
    {EngineProfileId::Ccz66,5,"C0 28 99 39 45 F8 7F 02 B2 0C 52 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? FF 4D 08 EB 95 8B E5 5D C3 55 8B EC 6A 00 8B 45 0C 39 45 FC 73 1C FF 75 08 E8 ??"},
    {EngineProfileId::Ccz66,1,"F8 E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 E8 ?? ?? ?? ?? FE 45 FC E9 ?? ?? ?? ?? 6A 00 FF 75 08 8B 4D EC E8 ?? ?? ?? ?? 83 7D 08 02 75 68"},
    {EngineProfileId::Ccz66,1,"F8 E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 E8 ?? ?? ?? ?? 6A 06 E8 ?? ?? ?? ?? 6A 04 E8 ?? ?? ?? ?? FE 45 F4 EB B4 FE 45 FC EB A5 8B E5 5D"},
    {EngineProfileId::Ccz66,0,"04 E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 6A 00 FF 75 08 8B 4D EC E8 ?? ?? ?? ?? 6A 00 FF 75 08 8B 4D F8 E8"},
    {EngineProfileId::Ccz66,0,"F8 E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 E8 ?? ?? ?? ?? 90 90 6A 06 90 90 90 90 90 E8 ?? ?? ?? ?? 6A 04 E8 ?? ?? ?? ?? EB A5 EB 94 8B E5"},
    {EngineProfileId::Ccz66,2,"F1 FF FF 6A 00 FF 75 F4 FF 75 F8 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 45 F0 8B E5 5D C3 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90"}
};

int HexNibble(char value) {
    return value <= '9' ? value - '0' : value - 'A' + 10;
}

bool MatchWaitCaller(const Image& image, uintptr_t returned, const char* signature) {
    constexpr size_t kContextSize = 48;
    if (returned < kOrigin + 21 || !image.Executable(returned - 21, kContextSize)) return false;
    int pattern[kContextSize] = {};
    size_t count = 0;
    while (*signature && count < kContextSize) {
        if (*signature == ' ') { ++signature; continue; }
        pattern[count++] = *signature == '?' ? -1 :
            (HexNibble(signature[0]) << 4) | HexNibble(signature[1]);
        signature += 2;
    }
    return count == kContextSize && !*signature &&
        Match(image.At(returned - 21), pattern, kContextSize);
}

void ResolveWaitCallers(const Image& image, EngineLayout& layout) {
    layout.waitCallerCount = 0;
    // Filter every real call to the resolved wait wrapper before collecting
    // recognized contexts, so unrelated waits cannot exhaust a candidate cap.
    for (WORD sectionIndex = 0; sectionIndex < image.count; ++sectionIndex) {
        const auto& section = image.sections[sectionIndex];
        if (!image.runtime && !(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t start = kOrigin + section.VirtualAddress;
        const size_t length = PeLogicalSectionSize(section);
        if (!image.Contains(start, length) || length < 5) continue;
        const BYTE* sectionData = image.At(start);
        const size_t candidateCount = length - 4;
        size_t searchOffset = 0;
        size_t remaining = candidateCount;
        while (remaining) {
            const BYTE* hit = static_cast<const BYTE*>(memchr(
                sectionData + searchOffset, 0xE8, remaining));
            if (!hit) break;
            const size_t offset = static_cast<size_t>(hit - sectionData);
            const uintptr_t at = start + offset;
            const auto advance = [&]() {
                const size_t consumed = offset - searchOffset + 1;
                searchOffset = offset + 1;
                remaining -= consumed;
            };
            if (image.Call(at) != layout.waitWrapper) {
                advance();
                continue;
            }
            int kind = -1;
            bool ambiguous = false;
            for (const auto& signature : kWaitCallerSignatures) {
                if ((signature.profile != layout.profile &&
                    !(layout.profile == EngineProfileId::Ccz64 &&
                        signature.profile == EngineProfileId::Ccz65)) ||
                    !MatchWaitCaller(image, at + 5, signature.context)) continue;
                if (kind >= 0 && kind != signature.kind) ambiguous = true;
                kind = signature.kind;
            }
            if (ambiguous) ++layout.ambiguousWaitCallers;
            if (kind < 0 || ambiguous) {
                advance();
                continue;
            }
            bool duplicate = false;
            for (size_t i = 0; i < layout.waitCallerCount; ++i)
                if (layout.waitCallers[i] == at + 5) duplicate = true;
            if (duplicate) {
                advance();
                continue;
            }
            if (layout.waitCallerCount == ARRAYSIZE(layout.waitCallers)) {
                // An unexpected multiplication of known contexts is ambiguous.
                layout.waitCallerCount = 0;
                return;
            }
            layout.waitCallers[layout.waitCallerCount] = at + 5;
            layout.waitKinds[layout.waitCallerCount++] = static_cast<unsigned char>(kind);
            advance();
        }
    }
}
}

static bool Resolve66(const BYTE* bytes, EngineLayout& output, uintptr_t virtualBase) {
    Image image = {};
    EngineLayout layout = {};
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
            layout.capabilities |= CapabilityNativeSuper|CapabilityDialog3|CapabilityExtraDelay|
                CapabilityDialogDisplay|CapabilityExtendedDisplay;
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
        ResolveWaitCallers(image, layout);
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
    layout.uniqueSignatureAttempts = image.uniqueSignatures;
    layout.missingSignatureAttempts = image.missingSignatures;
    layout.ambiguousSignatureAttempts = image.ambiguousSignatures;
    output = layout;
    return true;
}

// 6.4 has the original ECX timer but no native Z toggle or engine-owned
// phase table.  Keep its discovery independent from the 6.5/6.6 native
// route: the proxy owns the phase state, while the resolver verifies the
// wait/stage chain and the same presentation sites used by the patch.
static bool Resolve64(const BYTE* bytes, EngineLayout& output, uintptr_t virtualBase) {
    Image image = {};
    EngineLayout layout = {};
    // Preserve the partial candidate on every rejection so the installer can
    // report the exact missing or ambiguous signature that blocked automatic
    // injection (especially when a second 6.4 wait wrapper is found).
    struct PreserveCandidate {
        EngineLayout* destination;
        EngineLayout* candidate;
        Image* image;
        ~PreserveCandidate() {
            candidate->uniqueSignatureAttempts = image->uniqueSignatures;
            candidate->missingSignatureAttempts = image->missingSignatures;
            candidate->ambiguousSignatureAttempts = image->ambiguousSignatures;
            *destination = *candidate;
        }
    } preserve{&output, &layout, &image};
    layout.profile = EngineProfileId::Ccz64;
    layout.stageUsesThis = true;
    layout.phaseOwnedByPatch = true;
    layout.dialog3Applicable = false;
    if (!ParseImage(bytes, virtualBase, image)) return false;

    const uintptr_t wait = image.Find({0x55,0x8B,0xEC,0xB9,-1,-1,-1,-1,
        0xE8,-1,-1,-1,-1,0x85,0xC0,0x75,0x07,0xE8,-1,-1,-1,-1,
        0xEB,0xEB,0x5D,0xC3});
    if (!wait) return false;
    layout.waitWrapper = wait;
    layout.timerContext = image.Absolute(wait + 4, 4);
    layout.sites[SiteWaitCall] = wait + 8;
    layout.sites[SiteWaitBranch] = wait + 15;
    layout.nativeStage = image.Call(wait + 8);
    layout.pump = image.Call(wait + 17);
    const uintptr_t stage = layout.nativeStage;
    if (!stage || !layout.timerContext || !layout.pump ||
        !image.Matches(stage, {0x55,0x8B,0xEC,0x51,0x0F,0xB7,0x0D,-1,-1,-1,-1,
            0x0F,0xB6,0xC1,0x50,0x85,0xC0,0x75,0x04,0x8B,0xC8,0xEB,0x06,
            0xC1,0xE9,0x08,0x6B,0xC9,0x04,0x83,0xC1,0x01,0x51,0xE8,-1,-1,-1,-1,
            0x83,0x7D,0xF8,0x00,0x75,0x08,0x8B,0x4D,0xFC,0xE8,-1,-1,-1,-1,
            0xF7,0xD8,0x1B,0xC0,0x40,0x8B,0xE5,0x5D,0xC3})) return false;
    layout.speed = image.Absolute(stage + 7, 2);
    layout.sleep = image.Call(stage + 33);
    layout.deadline = image.Call(stage + 47);
    if (!layout.speed || !layout.sleep || !layout.deadline ||
        !image.Matches(layout.sleep, {0x55,0x8B,0xEC,0xFF,0x75,0x08,0xFF,0x15,-1,-1,-1,-1,0x5D,0xC2,0x04,0}) ||
        !image.Matches(layout.deadline, {0x55,0x8B,0xEC,0x51,0xE8,-1,-1,-1,-1,
            0x8B,0x4D,0xFC,0x3B,0x01})) return false;

    // 6.4's phase byte is shared by the player, ally and combat-side tests.
    const uintptr_t player = image.Find({0x80,0x3D,-1,-1,-1,-1,0,0x75,0x0B,
        0x8B,0x4D,0x08,0x80,0x79,0x05,0x02,0x75,0x02,0xB0,0x01});
    const uintptr_t ally = image.Find({0x80,0x3D,-1,-1,-1,-1,0x02,0x75,0x0B,
        0x8B,0x4D,0x08,0x80,0x79,0x05,0x02,0x74,0x02,0xB0,0});
    const uintptr_t combat = image.Find({0x80,0x3D,-1,-1,-1,-1,0x02,0x73,0x08,
        0x3C,0x01,0x74,0x10,0xB0,0x02,0xEB,0x0C});
    layout.phase = player ? image.Absolute(player + 2, 1) : 0;
    if (!layout.phase || !ally || !combat || image.Absolute(ally + 2, 1) != layout.phase ||
        image.Absolute(combat + 2, 1) != layout.phase) return false;
    // The 6.4 handler occupies the reserved command slots.  Confirm the
    // aliasing but never treat it as a Dialog3 site.
    const uintptr_t reserved = image.Absolute(0x410C18, 1);
    if (!reserved || !image.Executable(reserved, 1)) return false;
    for (uintptr_t slot = 0x410C1C; slot <= 0x410C2C; slot += 4)
        if (image.Absolute(slot, 1) != reserved) return false;

    layout.sites[SiteDialog] = image.Find({0x55,0x8B,0xEC,0x81,0xEC,0x68,0x09,0,0,
        0xE8,-1,-1,-1,-1,0x85,0xC0,0x74,0x3F,0x8B,0x4D,0x0C,
        0xE8,-1,-1,-1,-1,0x85,0xC0,0x75,0x20});
    layout.sites[SiteMapText] = image.Find({0x55,0x8B,0xEC,0x6A,0,0x51,
        0xB8,-1,-1,-1,-1,0xE8,-1,-1,-1,-1,0x50,0x8B,0x4D,0xF8,0x83,0x79,0x68,0,
        0x74,0x3C,0x6A,0,0xE8,-1,-1,-1,-1,0x6A,0x01,0x6A,0x32,0x6A,0x50});
    layout.sites[SiteDelay] = image.Find({0x55,0x8B,0xEC,0xB9,-1,-1,-1,-1,0x51,
        0xFF,0x75,0x08,0xE8,-1,-1,-1,-1,0x8B,0x4D,0xFC,0xE8,-1,-1,-1,-1,
        0xF7,0xD8,0x1B,0xC0,0x40,0x85,0xC0,0x75,0x07,0xE8,-1,-1,-1,-1,
        0xEB,0xE8,0x8B,0xE5,0x5D,0xC2,0x04,0});
    const uintptr_t delay = layout.sites[SiteDelay];
    if (!delay || image.Absolute(delay + 4, 4) != layout.timerContext ||
        image.Call(delay + 20) != layout.deadline || image.Call(delay + 34) != layout.pump) return false;
    const uintptr_t extra = image.Find({0x55,0x8B,0xEC,0x6A,0x04,0x8B,0x4D,0x08,
        0xE8,-1,-1,-1,-1,0x3D,0,0,0,0x80,0x75,0x07,0xB8,0x05,0,0,0,0xEB,0x11,
        0x85,0xC0,0x74,0x08,0xD1,0xE0,0x50,0xE8,-1,-1,-1,-1,0xB8,0x01,0,0,0,0x5D,0xC3});
    if (!extra || image.Call(extra + 34) != delay) return false;
    layout.sites[SiteExtraDelay] = extra + 31;
    const uintptr_t r = image.Find({0x68,0xFF,0,0,0,0x6A,0,0xE8,-1,-1,-1,-1,
        0xE8,-1,-1,-1,-1,0xA1,-1,-1,-1,-1,0x83,0xE0,0x6E,0x85,0xC0,0x74,0x98});
    if (!r || image.Call(r + 12) != wait) return false;
    layout.sites[SiteRWait] = r + 12;
    const uintptr_t key = image.Find({0x83,0xF8,-1,0x75,0x0F,0x8B,0x4D,0xFC,0x80,0x39,0,0x75,0x68,
        0xE8,-1,-1,-1,-1,0xEB,0x61,0x83,0xF8,0x30,0x72,0x5C,0x83,0xF8,0x35,0x73,0x22});
    layout.sites[SiteInputModifier] = key ? key + 2 : 0;
    const uintptr_t display = image.Find({0xB9,-1,-1,-1,-1,0xE8,-1,-1,-1,-1,
        0x85,0xC0,0x74,0x25,0x8A,0x4D,0xFC,0x80,0xF1,0x01,0x88,0x4D,0xFC,
        0x8B,0x55,0xFC,0x81,0xE2,0xFF,0,0,0,0x52,0xE8,-1,-1,-1,-1,
        0x83,0xC4,0x04,0x6A,0x03,0xE8,-1,-1,-1,-1,0x83,0xC4,0x04,0xE9});
    if (!display || image.Absolute(display + 1, 4) != layout.timerContext || image.Call(display + 5) != stage) return false;
    layout.sites[SiteDialogDisplay] = display + 5;
    const uintptr_t extended = image.Find({0xB9,-1,-1,-1,-1,0xE8,-1,-1,-1,-1,
        0x85,0xC0,0x74,0xE9,0x83,0x75,0xFC,0x01,0xFF,0x75,0xFC,
        0xE8,-1,-1,-1,-1,0x83,0xC4,0x04});
    if (!extended || image.Absolute(extended + 1, 4) != layout.timerContext || image.Call(extended + 5) != stage) return false;
    layout.sites[SiteExtendedDisplay] = extended + 5;
    if (!layout.sites[SiteDialog] || !layout.sites[SiteMapText] || !key) return false;

    // 6.4 uses the same presentation-context ABI as the 6.5 family. Scan
    // executable sections instead of relying on fixed return addresses so a
    // relocated caller remains discoverable after its original bytes move.
    ResolveWaitCallers(image, layout);
    layout.capabilities = CapabilitySuperDialog | CapabilitySuperMapText | CapabilitySuperDelay |
        CapabilitySuperR | CapabilitySuperS | CapabilityPhase | CapabilityInput |
        CapabilityExtraDelay | CapabilityDialogDisplay | CapabilityExtendedDisplay;
    layout.uniqueSignatureAttempts = image.uniqueSignatures;
    layout.missingSignatureAttempts = image.missingSignatures;
    layout.ambiguousSignatureAttempts = image.ambiguousSignatures;
    output = layout;
    return true;
}

static bool Resolve65(const BYTE* bytes, EngineLayout& output, uintptr_t virtualBase) {
    Image image = {};
    EngineLayout layout = {};
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
            layout.capabilities |= CapabilityNativeSuper|CapabilityExtraDelay|
                CapabilityDialogDisplay|CapabilityExtendedDisplay;
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
        ResolveWaitCallers(image, layout);
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
    layout.uniqueSignatureAttempts = image.uniqueSignatures;
    layout.missingSignatureAttempts = image.missingSignatures;
    layout.ambiguousSignatureAttempts = image.ambiguousSignatures;
    output = layout;
    return true;
}

namespace {
void ResolveAnimationPresentation(const BYTE* bytes, EngineLayout& layout, uintptr_t virtualBase) {
    Image image = {};
    if (!ParseImage(bytes, virtualBase, image)) return;
    // This value is shared by the native R/S branches, not a remembered scene.
    if (image.Matches(0x40B922, {0x55,0x8B,0xEC,0xE8,-1,-1,-1,-1,0x33,0xC9,0x3C,0x03}) &&
        image.Call(0x40B925) == 0x40B913 &&
        image.Matches(0x40B913, {0x55,0x8B,0xEC,0xB9,-1,-1,-1,-1,0xE8,-1,-1,-1,-1,0x5D,0xC3}) &&
        image.Call(0x40B91B) == 0x41EDEF &&
        image.Matches(0x41EDEF, {0x55,0x8B,0xEC,0x0F,0xB6,0x01,0x5D,0xC3}))
        layout.sceneState = image.Absolute(0x40B917, 1);
    const uintptr_t unitGetter = image.Find({0x55,0x8B,0xEC,0x0F,0xB6,0xC9,0x6B,0xC9,0x30,
        0x81,0xC1,-1,-1,-1,-1,0x5D,0xC3});
    layout.animationUnitBase = unitGetter ? image.Absolute(unitGetter + 11, 0x30u * 250u) : 0;
    const uintptr_t present = image.Call(0x47EBE1);
    if (!layout.sceneState || !layout.animationUnitBase ||
        !image.Matches(0x47EBA5, {0x83,0x7C,0x24,0x04,0x01,0x75,0x6D}) ||
        image.Call(0x47EBBD) != 0x420A38 ||
        !image.Matches(present, {0x55,0x8B,0xEC,0x6A,0x00,0x83,0xEC,-1}) ||
        image.Call(present + 23) != 0x47C966) return;
    // Gate the final screen blitter as well as normal frame commits: native
    // WM_PAINT and palette refresh call it directly while message pumping.
    if (!image.Matches(0x47C966, {0x55,0x8B,0xEC,0x81,0xEC,0x20,0x08,0,0}) ||
        !image.Matches(0x47CA80, {0xC2,0x14,0})) return;
    layout.animationPresent = 0x47C966;
    layout.animationPresentLength = 9;
    memcpy(layout.animationPresentOriginal, image.At(0x47C966), 9);
    auto entry = [&](uintptr_t address, unsigned char length, unsigned char kind,
        unsigned char actorSource, bool rScene) {
        if (layout.animationEntryCount >= _countof(layout.animationEntries) ||
            !image.Executable(address, length)) return;
        AnimationEntryLayout& item = layout.animationEntries[layout.animationEntryCount++];
        item.address = address; item.length = length; item.kind = kind;
        item.actorSource = actorSource; item.rScene = rScene;
        memcpy(item.original, image.At(address), length);
    };
    const bool is66 = layout.profile == EngineProfileId::Ccz66;
    const bool is64 = layout.profile == EngineProfileId::Ccz64;
    const bool mapPhysical = image.Matches(0x4526E7, {0x55,0x8B,0xEC,0x51,0x8A,0x51,0x10,
        0x88,0x51,0x01,0x8B,0x49,0x0C,0x51,0x81,0xEC,0xD4,0,0,0}) &&
        image.Call(0x4526FB) == 0x403B96 && image.Call(0x452706) == unitGetter &&
        image.Call(0x45272B) == 0x403D7F &&
        image.Call(0x452777) == (is66 ? 0x45506Au : 0x455123u) &&
        image.Call(is66 ? 0x4527E2 : (is64 ? 0x4527F5 : 0x4527F0)) == 0x404388 &&
        image.Matches(is66 ? 0x4527E7 : (is64 ? 0x4527FA : 0x4527F5), {0x8B,0xE5,0x5D,0xC3});
    if (mapPhysical) {
        // The outer wrapper's result CALL belongs to BattleStats. Keep this
        // presentation scope after the roll, without overlapping that hook.
        entry(0x4526E7, 7, 1, 2, false);
        layout.animationOrdinaryPhysical = 0x4526E7;
    }
    if (is66 && image.Matches(0x452907, {0x55,0x8B,0xEC,0x51,0x51,0x6A,0,0x8B,0x49,0x0C}) &&
        image.Call(0x45292B) == 0x4526E7 && image.Call(0x452933) == 0x405966 &&
        image.Matches(0x452954, {0x8B,0xE5,0x5D,0xC3}))
        entry(0x452907, 5, 1, 2, false);
    const uintptr_t movement = is66 ? 0x442000 : 0x44201B;
    const uintptr_t movementWait = is66 ? 0x442256 : (is64 ? 0x4425C2 : 0x442436);
    if ((is64 ? image.Matches(movement, {0x55,0x8B,0xEC,0x83,0xEC,0x70,0x53,0x89,0x4D,0xA0}) :
            image.Matches(movement, {0x55,0x8B,0xEC,0x68,0x30,0x0C,0x06,0})) &&
        image.Call(movementWait) == (is64 ? layout.waitWrapper : 0x423971u) &&
        (is64 || image.Call(0x42397C) == layout.waitWrapper) &&
        image.Call(is66 ? 0x44225B : (is64 ? 0x442422 : 0x44243B)) == 0x41E5DB &&
        image.Call(is66 ? 0x442327 : (is64 ? 0x44250F : 0x4424E5)) == 0x451347 &&
        image.Call(is66 ? 0x44236B : (is64 ? 0x4425BD : 0x442522)) == 0x41E5F6 &&
        image.Matches(is66 ? 0x44238F : (is64 ? 0x442600 : 0x442546), {0xC2,0x04,0}))
        entry(movement, is64 ? 6 : 8, 0, 1, false);
    const uintptr_t single = is66 ? 0x4370F3 : 0x4370F6;
    const uintptr_t hit = is66 ? 0x4234B8 : 0x4234A8;
    if ((is66 ? image.Matches(single, {0x55,0x8B,0xEC,0x6A,0,0x51,0x6A,0}) :
            image.Matches(single, {0x55,0x8B,0xEC,0x83,0xEC,0x20,0x89,0x4D,0xF8})) &&
        image.Call(is66 ? 0x437149 : 0x437157) == hit &&
        image.Call(is66 ? 0x4371A4 : 0x4371B2) == 0x47EBA5 &&
        image.Call(is66 ? 0x4371AC : 0x4371BA) == layout.waitWrapper)
        entry(single, is66 ? 5 : 6, 1, 1, false);
    if (image.Matches(0x4536C7, {0x55,0x8B,0xEC,0x83,0xEC,0x20,0x89,0x4D,0xF8}) &&
        image.Call(0x453718) == hit && image.Call(0x45376A) == 0x47EBA5 &&
        image.Call(0x453772) == layout.waitWrapper)
        entry(0x4536C7, 6, 1, 1, false);
    if (image.Matches(0x411224, {0x55,0x8B,0xEC,0x51,0x33,0xC0,0x80,0xF9,0x20,0x74,0x01,
            0x40,0x69,0xC0,0,0xE9,0x01,0,0x05,0,0x75,0x03,0,0x03,0x45,0x08}) &&
        image.Call(0x41123E) == 0x40155F && image.Call(0x411258) == 0x47E09B &&
        image.Matches(0x41125D, {0x5D,0xC2,0x04,0}) &&
        image.Matches(0x41CC9D, {0x55,0x8B,0xEC,0x51,0x33,0xC0,0x80,0xF9,0x20,0x74,0x01,
            0x40,0x69,0xC0,0,0xE9,0x01,0,0x05,0,0x4C,0x02,0,0x03,0x45,0x08}) &&
        image.Call(0x41CCB7) == 0x40155F && image.Call(0x41CCCB) == 0x47E09B &&
        image.Matches(0x41CCD0, {0x8B,0xE5,0x5D,0xC2,0x08,0}) &&
        image.Matches(0x425B33, {0x55,0x8B,0xEC,0x6A,0,0x51,0x6A,0x14,0xE8,-1,-1,-1,-1,
            0x85,0xC0,0x75,0x06,0xC6,0x45,0xFC,0x09,0xEB,0x04,0xC6,0x45,0xFC,0x04}) &&
        image.Call(0x425B3B) == 0x4221DD && image.Call(0x425B5D) == 0x41CC9D &&
        image.Matches(0x425B62, {0x8B,0xE5,0x5D,0xC3})) {
        layout.animationNeutralSprite = 0x425B33;
        entry(0x411224, 6, 1, 1, false);
        layout.animationEntries[layout.animationEntryCount - 1].physicalSprite = true;
        layout.animationEntries[layout.animationEntryCount - 1].argumentBytes = 4;
        entry(0x41CC9D, 6, 1, 1, false);
        layout.animationEntries[layout.animationEntryCount - 1].physicalSprite = true;
        layout.animationEntries[layout.animationEntryCount - 1].argumentBytes = 8;
        const uintptr_t spriteCalls[] = {is66 ? 0x437175u : 0x437183u,
            is66 ? 0x437193u : 0x4371A1u,is66 ? 0x437200u : 0x437215u,
            0x45373F,0x45374A,0x4537EA,0x4537F7};
        for (uintptr_t call : spriteCalls) {
            const uintptr_t target = image.Call(call);
            if (target == 0x411224 || target == 0x41CC9D)
                layout.animationPhysicalSpriteCallers[layout.animationPhysicalSpriteCallerCount++] = call + 5;
        }
    }
    if (image.Matches(0x425627, {0x55,0x8B,0xEC,0x51,0xFF,0x75,0x0C,0xFF,0x75,0x08}) &&
        image.Call(0x425634) == 0x4251E1 &&
        image.Matches(0x4251F3, {0x8A,0x55,0x0C,0x89,0x51,0x01,0xC6,0x41,0x0C,0xFF,0x8A,0xCA}) &&
        image.Call(0x4251FF) == unitGetter)
        entry(0x425627, 7, 2, 6, false);
    const bool idleMapSprite = (is64 ?
        image.Matches(0x43E572, {0x55,0x8B,0xEC,0x83,0xEC,0x0C,0x33,0xC0}) &&
            image.Call(0x43E583) == unitGetter && image.Call(0x43E58D) == 0x425AF0 &&
            image.Call(0x43E5FC) == 0x4134DA && image.Call(0x43E61D) == 0x4134DA &&
            image.Call(0x43E656) == 0x401655 && image.Matches(0x43E678, {0x8B,0xE5,0x5D,0xC3}) :
        image.Matches(0x43E572, {0x55,0x8B,0xEC,0x6A,0,0x6A,0,0x8B,0x4D,0x08}) &&
            image.Call(0x43E57C) == unitGetter && image.Call(0x43E584) == 0x425AF0 &&
            image.Call(0x43E5D0) == 0x4134DA && image.Call(0x43E60E) == 0x401655 &&
            image.Matches(0x43E627, {0x8B,0xE5,0x5D,0xC2,0x04,0}));
    if (idleMapSprite && image.Matches(0x403240, {0x55,0x8B,0xEC,0x50,0x51,0x6A,0x40,0x6A,0x40,
            0x8B,0x55,0x0C,0x83,0xEA,0x08,0x52,0x8B,0x45,0x08,0x83,0xE8,0x08,0x50}) &&
        image.Call(0x403257) == 0x451347 && image.Matches(0x40325C, {0x8B,0xE5,0x5D,0xC2,0x08,0}) &&
        (is66 ? image.Matches(0x451347, {0x55,0x8B,0xEC,0xFF,0x75,0x18}) &&
            image.Matches(0x45153E, {0x8B,0xE5,0x5D,0xC2,0x18,0}) :
            (is64 ? image.Matches(0x451347, {0x55,0x8B,0xEC,0x83,0xEC,0x14}) &&
                image.Matches(0x45155F, {0x5E,0x8B,0xE5,0x5D,0xC2,0x18,0}) :
                image.Matches(0x451347, {0x55,0x8B,0xEC,0xB8,0,0x1E,0x06,0}) &&
                image.Matches(0x45151C, {0x8B,0xE5,0x5D,0xC2,0x18,0}))) &&
        image.Matches(0x40F18B, {0xB8,0,0xC0,0,0,0xE8,-1,-1,-1,-1,0x50,0x8B,0x4D,-1,
            0xFF,0x71,0x01,0x6A,0x30,0x6A,0x30,0xFF,0x75,0xD0,0xFF,0x75,0xD4}) &&
        image.Call(0x40F190) == 0x4015B1 && image.Call(0x40F1A6) == 0x451347 &&
        image.Matches(0x40F1AB, {0xC3}) &&
        image.Call(is66 ? 0x4041DE : 0x4041F4) == 0x451347) {
        const uintptr_t spriteCalls66[] = {0x4044FA,0x4045B1,0x4045E1,0x404625,0x4047A2,
            0x404806,0x404956,0x4049B8,0x404A13,0x404A34};
        const uintptr_t spriteCalls65[] = {0x404523,0x4045B3,0x4045D4,0x404625,0x404643,0x40467D,
            0x40481B,0x40487F,0x4049D5,0x404A3A,0x404A9B,0x404ABC};
        const uintptr_t* calls = is66 ? spriteCalls66 : spriteCalls65;
        const size_t count = is66 ? _countof(spriteCalls66) : _countof(spriteCalls65);
        for (size_t i = 0; i < count; ++i)
            if (image.Call(calls[i]) == 0x403240)
                layout.animationMapSpriteCallers[layout.animationMapSpriteCallerCount++] = calls[i] + 5;
        if (layout.animationMapSpriteCallerCount == count) {
            layout.animationNeutralMapSprite = 0x43E572;
            layout.animationMapDraw = 0x451347;
            entry(0x403240, 5, 1, 0, false);
            auto& sprite = layout.animationEntries[layout.animationEntryCount - 1];
            sprite.mapPhysicalSprite = true;
            sprite.argumentBytes = 8;
            layout.animationMapSpriteCallers[layout.animationMapSpriteCallerCount++] = 0x40F1AB;
            layout.animationMapSpriteCallers[layout.animationMapSpriteCallerCount++] = is66 ? 0x4041E3 : 0x4041F9;
            entry(0x451347, is66 || is64 ? 6 : 8, 1, 0, false);
            auto& hitSprite = layout.animationEntries[layout.animationEntryCount - 1];
            hitSprite.mapPhysicalSprite = true;
            hitSprite.argumentBytes = 24;
        }
    }
    const bool numberActor = image.Matches(0x45005C, {0x8A,0x4D,0x08}) ?
        image.Call(0x45005F) == unitGetter :
        image.Matches(0x450059, {0x8A,0x4D,0x08}) && image.Call(0x45005C) == unitGetter;
    if (image.Matches(0x450033, {0x55,0x8B,0xEC,0x83,0xEC,0x50}) && numberActor)
        entry(0x450033, 6, 4, 7, false);
    const uintptr_t mapNumbers = is66 ? 0x44FB73 : 0x44FAF1;
    if ((is66 ? image.Matches(mapNumbers, {0x55,0x8B,0xEC,0x8B,0x4D,0x08}) :
            image.Matches(mapNumbers, {0x55,0x8B,0xEC,0x83,0xEC,0x24,0x56,0x8B,0x4D,0x08})) &&
        image.Call(is66 ? 0x44FB79 : 0x44FAFB) == unitGetter &&
        image.Call(is66 ? 0x404A77 : 0x404AFF) == mapNumbers &&
        image.Call(is66 ? 0x44FC13 : 0x44FC19) == 0x41E5DB &&
        image.Call(is66 ? 0x44FC2D : 0x44FC33) == 0x41E5F6 &&
        image.Call(is66 ? 0x44FC25 : 0x44FC2B) == 0x40FAA0 &&
        image.Call(is66 ? 0x44FC39 : 0x44FC3F) == 0x40E303 &&
        image.Matches(is66 ? 0x44FC41 : 0x44FC48, {0xC2,0x10,0})) {
        entry(mapNumbers, 6, 4, 7, false);
        auto& numbers = layout.animationEntries[layout.animationEntryCount - 1];
        numbers.numberPresentation = true;
        numbers.argumentBytes = 16;
        // Every native caller discards the incidental EAX result. Keep unknown
        // callers native, even when they reach this otherwise pure UI leaf.
        const uintptr_t calls[] = {is66 ? 0x404A77u : 0x404AFFu,
            is66 ? 0x412D31u : 0x412D2Au,is66 ? 0x421CAAu : 0x421CBAu};
        for (size_t i = 0; i < _countof(calls); ++i) {
            const uintptr_t call = calls[i];
            const bool discardsReturn = i == 1 ? (is66 ?
                image.Matches(call + 5, {0x8B,0x45,0x08,0x33,0xC9}) :
                image.Matches(call + 5, {0x6A,0x01,0x8B,0x45,0x08,0x33,0xC9})) :
                image.Call(call + 5) == 0x41E5F6;
            if (image.Call(call) == mapNumbers && discardsReturn)
                layout.animationNumberPresentationCallers[layout.animationNumberPresentationCallerCount++] = call + 5;
        }
    }
    if (image.Matches(0x423971, {0x55,0x8B,0xEC,0xFF,0x75,0x08}) &&
        image.Call(is64 ? 0x423981 : 0x42397C) == layout.waitWrapper &&
        image.Matches(is64 ? 0x423986 : 0x423981, {0x5D,0xC2,0x04,0}) &&
        image.Matches(is66 ? 0x404187 : 0x40419D, {0x6A,0x04}) &&
        image.Call(is66 ? 0x404189 : 0x40419F) == 0x423971) {
        const bool numberWaitLoop = is66 && image.Call(0x404A8D) == 0x40CB10 &&
            image.Matches(0x40CB10, {0x55,0x8B,0xEC,0x6A,0,0x8B,0x45,0xFC,0x3B,0x45,0x08,
                0x73,0x0C,0x6A,0x01}) && image.Call(0x40CB1F) == 0x423971 &&
            image.Matches(0x40CB24, {0xFF,0x45,0xFC,0xEB,0xEC,0x8B,0xE5,0x5D,0xC2,0x04,0});
        if (is66 && image.Matches(0x404A8B, {0x6A,0x08}) &&
            (image.Call(0x404A8D) == 0x423971 || numberWaitLoop)) {
            layout.animationWaitRouterReturn = 0x423981;
            if (numberWaitLoop) layout.animationWaitLoopReturn = 0x40CB24;
            layout.animationNumberWaitCallers[0] = 0x40418E;
            layout.animationNumberWaitCallers[1] = 0x404A92;
        } else if (!is66 && image.Matches(0x404BD9, {0x6A,0x01}) && image.Call(0x404BDB) == 0x423971 &&
            image.Matches(0x40440F, {0xFF,0x45,0xF0,0x8B,0x45,0xF0,0x3C,0x1B}) &&
            image.Matches(0x404425, {0x8A,0x80,-1,-1,-1,-1,0xFF,0x24,0x85,-1,-1,-1,-1}) &&
            image.Absolute(0x404427, 0x1B) == 0x404C12 &&
            image.Absolute(0x40442E, 10 * sizeof(DWORD)) == 0x404BEA &&
            image.Matches(0x404C23, {0x07,0x09,0x09,0x09,0x09,0x09,0x09,0x09,0x08}) &&
            image.Absolute(0x404C06, 1) == 0x404A49 && image.Absolute(0x404C0A, 1) == 0x404B19) {
            layout.animationWaitRouterReturn = is64 ? 0x423986 : 0x423981;
            layout.animationNumberWaitCallers[0] = 0x4041A4;
            layout.animationNumberTickWait = 0x404BE0;
        }
    }
    if (image.Matches(0x43E6AA, {0x55,0x8B,0xEC,0x6A,0,0x8B,0x4D,0x08}) &&
        image.Call(0x43E6B2) == unitGetter)
        entry(0x43E6AA, 5, 3, 7, false);
    const uintptr_t batchStatus = is66 ? 0x42481A : 0x424822;
    if ((is66 ? image.Matches(batchStatus, {0x55,0x8B,0xEC,0x50,0x6A,0,0x51,0x52}) :
            image.Matches(batchStatus, {0x55,0x8B,0xEC,0x6A,0,0x6A,0,0x51})) &&
        image.Call(is66 ? 0x425972 : 0x425971) == batchStatus &&
        image.Call(is66 ? 0x42482D : 0x424840) == unitGetter)
        entry(batchStatus, is66 ? 6 : 5, 3, 0, false);
    const uintptr_t morale = is66 ? 0x40D3BC : 0x40D3B7;
    if ((is66 ? image.Matches(morale, {0x55,0x8B,0xEC,0xB9,-1,-1,-1,-1,0x51,0x0F,0xB6,0x41,0x12}) :
            image.Matches(morale, {0x55,0x8B,0xEC,0x6A,0,0x6A,0,0xB9,-1,-1,-1,-1,0x51})) &&
        image.Absolute(morale + (is66 ? 4 : 8), 0x13) == 0x4B0770 &&
        image.Call(0x4148E7) == morale)
        entry(morale, is66 ? 8 : 5, 4, 5, false);
    if (image.Matches(0x4061E4, {0x55,0x8B,0xEC,0x0F,0xB7,0x4D,0x08,0x6B,0xC9,0x48,0x03,0x0D,
            -1,-1,-1,-1,0x5D,0xC2,0x04,0}) &&
        image.Matches(0x40BE7D, {0x55,0x8B,0xEC,0x8B,0x45,0x08,0x2B,0x05,-1,-1,-1,-1,
            0x99,0x33,0xC9,0xB1,0x48}) &&
        image.Absolute(0x4061F0, 4) == image.Absolute(0x40BE85, 4) &&
        image.Call(0x44E871) == 0x44AC28 && image.Call(0x44ED9A) == 0x44AC28 &&
        (is66 ? image.Matches(0x44AC28, {0x55,0x8B,0xEC,0x51,0x6A,0}) :
            image.Matches(0x44AC28, {0x55,0x8B,0xEC,0x83,0xEC,0x40,0x89,0x4D,0xC0}))) {
        layout.animationPersonTable = image.Absolute(0x4061F0, 4);
        // This entry draws the player/enemy phase banner. ECX is its phase
        // controller, not a person record, and the banner is not a number.
        if (layout.animationPersonTable) entry(0x44AC28, 6, 5, 5, false);
    }
    const uintptr_t text = is66 ? 0x42CD05 : 0x42CB4D;
    const uintptr_t dialogueMap = is66 ? 0x41B882 : 0x41B888;
    const bool dialogueMapValid = is66 ?
        image.Matches(dialogueMap, {0x55,0x8B,0xEC,0x33,0xC0,0xBA,0x04,0x04,0,0,0x3B,0xCA,
            0x7C,0x04,0x2B,0xCA,0xEB,0x0F,0x38,0x05,-1,-1,-1,-1,0x74,0x17,
            0x66,0x81,0xF9,0xFA,0,0x73,0x10,0xE8,-1,-1,-1,-1,0x66,0x83,0x39,0xFF,
            0x75,0x03,0x48,0xEB,0x02,0x8B,0xC1,0x5D,0xC3}) &&
            image.Call(dialogueMap + 33) == unitGetter :
        image.Matches(dialogueMap, {0x55,0x8B,0xEC,0x33,0xC0,0x38,0x05,-1,-1,-1,-1,0x74,0x18,
            0x66,0x81,0xF9,0xFA,0,0x73,0x11,0xE8,-1,-1,-1,-1,0x66,0x81,0x39,0xFF,0xFF,
            0x75,0x03,0x48,0xEB,0x02,0x8B,0xC1,0x5D,0xC3}) &&
            image.Call(dialogueMap + 20) == unitGetter;
    const uintptr_t dialogueSlotFlag = dialogueMapValid ? image.Absolute(dialogueMap + (is66 ? 20 : 7), 1) : 0;
    if (dialogueSlotFlag && (is64 ? image.Matches(text, {0x55,0x8B,0xEC,0x81,0xEC,0x30,0x07,0,0}) :
            image.Matches(text, {0x55,0x8B,0xEC,0x6A,0,0x6A,0})) &&
        image.Call(is66 ? 0x42CD15 : (is64 ? 0x42CB6A : 0x42CB5D)) == (is66 ? 0x41B882u : 0x41B888u) &&
        image.Call(is66 ? 0x42CD54 : (is64 ? 0x42CDA2 : 0x42CD25)) == 0x41E5DB &&
        image.Call(is66 ? 0x42CDEB : (is64 ? 0x42CF62 : 0x42CDD8)) == 0x41E5F6 &&
        image.Matches(is64 ? 0x42D115 : 0x42CEEB, {0xC2,0x0C,0})) {
        layout.animationDialogueSlotFlag = dialogueSlotFlag;
        layout.animationDialogueEncodedSlots = is66;
        entry(text, is64 ? 9 : 5, 5, 8, false);
        // The router also owns actorless battlefield narration (IDs 400..403).
        // It is the sole owner of the independent cancel-dialogue fast return.
        if ((is64 ? image.Matches(0x42D5B0, {0x55,0x8B,0xEC,0x81,0xEC,0x68,0x09,0,0}) :
                image.Matches(0x42D5B0, {0x55,0x8B,0xEC,0xE8,-1,-1,-1,-1})) &&
            image.Call(is64 ? 0x42D5B9 : 0x42D5B3) == 0x40B922 &&
            image.Call(is64 ? 0x42D5C5 : 0x42D5BF) == (is66 ? 0x41B882u : 0x41B888u) &&
            image.Call(is64 ? 0x42D5F7 : 0x42D5DA) == text) {
            entry(0x42D5B0, is64 ? 9 : 8, 5, 8, false);
            auto& router = layout.animationEntries[layout.animationEntryCount - 1];
            router.dialogueRouter = true;
            router.relativeCallOffset = is64 ? 0xFF : 3;
        }
    }
    // Only non-interactive presentation commands are scoped. The native
    // handler still consumes every parameter, applies changes and runs triggers.
    if (image.Matches(0x410A17, {0xFF,0x14,0x85,-1,-1,-1,-1}) &&
        image.Absolute(0x410A1A, 0x40 * 4) == 0x410A38) {
        struct ScriptEntry { unsigned opcode; uintptr_t address; unsigned char length; };
        const ScriptEntry commands[] = {
            {0x27,0x413D13,5},{0x2A,0x4171BC,6},{0x30,0x411DAE,6},
            {0x31,0x413522,6},{0x32,0x4133B2,6},{0x33,0x411E79,6},
            {0x34,0x411EDE,6},{0x35,0x411F2D,6}
        };
        for (const ScriptEntry& command : commands) {
            if (image.Absolute(0x410A38 + (command.opcode - 2) * 4, 8) != command.address) continue;
            const bool prefix = command.length == 5 ?
                image.Matches(command.address, {0x55,0x8B,0xEC,0x6A,0}) :
                image.Matches(command.address, {0x55,0x8B,0xEC,0x83,0xEC,-1}) ||
                image.Matches(command.address, {0x55,0x8B,0xEC,0x50,0x6A,0x02});
            if (prefix) entry(command.address, command.length, 0, 0, true);
        }
    }
}

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
    if (layout.profile == EngineProfileId::Ccz64) {
        layout.nativeSiteMask = 0;
        // 6.4's dialog wrapper has an extra byte-local instruction and its
        // display branch skips the verified blink block rather than using the
        // later native toggle encodings.
        layout.siteOriginal[SiteDialog][3] = 0x81;
        layout.siteEnabled[SiteDialog][3] = 0x81;
        layout.siteOriginal[SiteDialogDisplay][1] = 0x1C;
        layout.siteEnabled[SiteDialogDisplay][1] = 0x2C;
        layout.siteOriginal[SiteExtendedDisplay][1] = 0x49;
        layout.siteEnabled[SiteExtendedDisplay][1] = 0xF5;
    }
}
}

bool ResolveEngineLayout(const BYTE* bytes, EngineLayout& output, uintptr_t virtualBase,
    EngineAnalysisSource source, uintptr_t runtimeMemoryBase) {
    AnalysisScope scope(source, runtimeMemoryBase);
    output = EngineLayout();
    EngineLayout layout = {};
    // Family identity needs the native toggle and wait/stage relationship. A
    // partial match to common dialog code must never misidentify an engine.
    if (Resolve64(bytes, layout, virtualBase) && layout.waitWrapper && layout.nativeStage &&
        layout.phaseOwnedByPatch && layout.speed) {
        SetTemplates(layout);
        ResolveAnimationPresentation(bytes, layout, virtualBase);
        AppendRPresentationLayout(bytes, layout, virtualBase);
        AppendSPresentationLayout(bytes, layout, virtualBase);
        BuildEnginePatchPlan(bytes, layout.plan.imageSize, layout, layout.plan, virtualBase, source);
        output = layout;
        return true;
    }
    const EngineLayout partial64 = layout;
    layout = EngineLayout();
    if (Resolve66(bytes, layout, virtualBase) && layout.nativeSuper && layout.waitWrapper &&
        layout.nativeStage && layout.speed && layout.phaseTable) {
        SetTemplates(layout);
        ResolveAnimationPresentation(bytes, layout, virtualBase);
        AppendRPresentationLayout(bytes, layout, virtualBase);
        AppendSPresentationLayout(bytes, layout, virtualBase);
        BuildEnginePatchPlan(bytes, layout.plan.imageSize, layout, layout.plan, virtualBase, source);
        output = layout;
        return true;
    }
    const EngineLayout partial66 = layout;
    layout = EngineLayout();
    if (Resolve65(bytes, layout, virtualBase) && layout.nativeSuper && layout.waitWrapper &&
        layout.nativeStage && layout.speed && layout.phaseTable && layout.timerContext) {
        SetTemplates(layout);
        ResolveAnimationPresentation(bytes, layout, virtualBase);
        AppendRPresentationLayout(bytes, layout, virtualBase);
        AppendSPresentationLayout(bytes, layout, virtualBase);
        BuildEnginePatchPlan(bytes, layout.plan.imageSize, layout, layout.plan, virtualBase, source);
        output = layout;
        return true;
    }
    // Keep a rejected candidate's diagnostics for the installer report. This
    // never authorizes runtime writes against an unresolved layout.
    if (layout.nativeSuper || partial66.nativeSuper || partial64.waitWrapper ||
        partial64.ambiguousSignatureAttempts || partial64.missingSignatureAttempts) {
        if (!layout.nativeSuper) layout = partial66;
        if (!layout.nativeSuper && (partial64.waitWrapper ||
            partial64.ambiguousSignatureAttempts || partial64.missingSignatureAttempts)) layout = partial64;
        SetTemplates(layout);
        ResolveAnimationPresentation(bytes, layout, virtualBase);
        AppendRPresentationLayout(bytes, layout, virtualBase);
        AppendSPresentationLayout(bytes, layout, virtualBase);
        BuildEnginePatchPlan(bytes, 0, layout, layout.plan, virtualBase, source);
        layout.plan.status = EnginePlanStatus::Rejected;
        output = layout;
    }
    return false;
}

namespace {
constexpr size_t kSiteLengths[EngineSiteCount] = {
    4, 4, 3, 5, 5, 1, 4, 2, 2, 2, 2
};

unsigned SiteCapability(EngineSiteIndex index) {
    static const unsigned values[EngineSiteCount] = {
        CapabilitySuperDialog, CapabilitySuperMapText, CapabilitySuperDelay,
        CapabilitySuperR, CapabilitySuperS | CapabilityPhase, CapabilityInput,
        CapabilityDialog3, CapabilityExtraDelay, CapabilitySuperS,
        CapabilityDialogDisplay, CapabilityExtendedDisplay
    };
    return index < EngineSiteCount ? values[index] : 0;
}

bool SiteRequired(const EngineLayout& layout, EngineSiteIndex index) {
    return index < EngineSiteCount && (index != SiteDialog3 || layout.dialog3Applicable);
}

EngineSitePlan MakePlanSite(const Image& image, const EngineLayout& layout,
    EngineSiteIndex index, uintptr_t address, size_t length, const BYTE* original,
    const BYTE* enabled) {
    EngineSitePlan result;
    result.index = index;
    result.legacyAddress = address;
    result.rva = address >= kOrigin ? address - kOrigin : 0;
    result.length = length;
    result.capability = SiteCapability(index);
    result.required = SiteRequired(layout, index);
    result.present = address != 0 && image.Contains(address, length);
    if (result.present && image.RawOffset(address, length, result.rawFileOffset))
        result.byteSource = EngineSiteByteSource::Disk;
    if (original) memcpy(result.original, original, sizeof(result.original));
    if (enabled) memcpy(result.enabled, enabled, sizeof(result.enabled));
    if (result.present && (length > sizeof(result.original) ||
        !CopyMemoryChecked(result.original, image.At(address), length))) result.present = false;
    return result;
}
}

const wchar_t* EnginePlanStatusName(EnginePlanStatus status) {
    switch (status) {
    case EnginePlanStatus::AutoVerified: return L"自动识别已验证";
    case EnginePlanStatus::VersionSelected: return L"按版本模板注入";
    case EnginePlanStatus::Rejected: return L"已拒绝";
    default: return L"未解析";
    }
}

std::wstring EnginePatchPlanReport(const EnginePatchPlan& plan) {
    std::wstringstream text;
    text << L"EnginePlanVersion=1\r\nProfile=" << static_cast<unsigned>(plan.profile)
        << L"\r\nMode=" << (plan.status == EnginePlanStatus::VersionSelected ? L"VersionSelected" : L"Automatic")
        << L"\r\nStatus=" << static_cast<unsigned>(plan.status) << L"\r\nConfidence=" << plan.confidence
        << L"\r\nConfidenceBasis=RuleBasedLevelNotProbability"
        << L"\r\nImageSize=" << plan.imageSize << L"\r\nCapabilities=" << plan.capabilities
        << L"\r\nRuntimeValidated=" << (plan.runtimeValidated ? 1 : 0)
        << L"\r\nEvidence=" << plan.evidence << L"\r\n";
    auto add = [&](const wchar_t* name, const EngineSitePlan& site) {
        text << L"\r\n[" << name << L"]\r\nPresent=" << (site.present ? 1 : 0)
            << L"\r\nRequired=" << (site.required ? 1 : 0)
            << L"\r\nRVA=0x" << std::hex << std::uppercase << site.rva
            << L"\r\nAddress=0x" << site.legacyAddress << L"\r\nRawFileOffset=";
        if (site.rawFileOffset == SIZE_MAX) text << L"Unavailable";
        else text << L"0x" << site.rawFileOffset;
        text << L"\r\nByteSource=" << (site.byteSource == EngineSiteByteSource::Disk ? L"Disk" : L"RuntimeOnly")
            << std::dec << L"\r\nLength=" << site.length << L"\r\nOriginal=";
        for (size_t i = 0; i < site.length && i < sizeof(site.original); ++i) {
            if (i) text << L' ';
            text << std::hex << std::uppercase << std::setfill(L'0') << std::setw(2)
                << static_cast<unsigned>(site.original[i]);
        }
        text << std::dec << L"\r\n";
    };
    for (size_t i = 0; i < EngineSiteCount; ++i) {
        const std::wstring name = L"Site." + std::to_wstring(i);
        add(name.c_str(), plan.sites[i]);
    }
    add(L"NativeRoute", plan.nativeRoute);
    add(L"NativeState", plan.nativeState);
    add(L"NativeBusy", plan.nativeBusy);
    add(L"PhaseTable", plan.phaseTable);
    add(L"PhaseValue", plan.phaseValue);
    add(L"SpeedValue", plan.speedValue);
    add(L"TimerContext", plan.timerContext);
    return text.str();
}

bool ResolveVersionEngineLayout(const BYTE* bytes, size_t imageSize,
    EngineProfileId profile, EngineLayout& output, uintptr_t virtualBase) {
    output = EngineLayout();
    Image image = {};
    if ((profile != EngineProfileId::Ccz64 && profile != EngineProfileId::Ccz65 &&
         profile != EngineProfileId::Ccz66) || !ParseImage(bytes, virtualBase, image) ||
        (imageSize && image.size > imageSize)) return false;

    // Reuse independently discovered addresses where available, while the
    // selected family supplies the ABI and offsets for unresolved sites.
    EngineLayout resolved;
    if (profile == EngineProfileId::Ccz64) Resolve64(bytes, resolved, virtualBase);
    else if (profile == EngineProfileId::Ccz65) Resolve65(bytes, resolved, virtualBase);
    else Resolve66(bytes, resolved, virtualBase);
    EngineLayout layout;
    layout.profile = profile;
    layout.stageUsesThis = profile != EngineProfileId::Ccz66;
    layout.phaseOwnedByPatch = profile == EngineProfileId::Ccz64;
    layout.dialog3Applicable = profile == EngineProfileId::Ccz66;
    const uintptr_t sites64[EngineSiteCount] = {0x42D5B0,0x4327EF,0x40A985,0x429964,0x42C4E8,
        0x413324,0,0x415B40,0x42C4EF,0x42C812,0x455CE5};
    const uintptr_t sites65[EngineSiteCount] = {0x42D5B0,0x4327EF,0x40A985,0x429965,0x42C4E8,
        0x413324,0,0x415B40,0x42C4EF,0x42C812,0x455CE5};
    const uintptr_t sites66[EngineSiteCount] = {0x42D5B0,0x4332A6,0x422DBA,0x429964,0x42C4E8,
        0x413324,0x42DB2C,0x415B40,0x42C4EF,0x42C812,0x455CE5};
    memcpy(layout.sites, profile == EngineProfileId::Ccz64 ? sites64 :
        profile == EngineProfileId::Ccz65 ? sites65 : sites66, sizeof(layout.sites));
    layout.waitWrapper = profile == EngineProfileId::Ccz66 ? 0x42C4E5 : 0x42C4E0;
    layout.nativeStage = profile == EngineProfileId::Ccz64 ? 0x422B33 : 0x4750E0;
    layout.timerContext = profile == EngineProfileId::Ccz66 ? 0 : 0x4B1B18;
    layout.phase = 0x4B3D18;
    layout.speed = profile == EngineProfileId::Ccz66 ? 0x500AD8 : 0x4CE7BC;
    layout.sleep = profile == EngineProfileId::Ccz66 ? 0x411195 : 0x4298F6;
    layout.deadline = profile == EngineProfileId::Ccz66 ? 0x40C19D : 0x421DF2;
    layout.pump = 0x47EAC4;
    if (!layout.phaseOwnedByPatch) {
        layout.nativeSuper = profile == EngineProfileId::Ccz65 ? 0x4250B3 : 0x4250E6;
        layout.nativeSuperCall = profile == EngineProfileId::Ccz65 ? 0x4132E2 : 0x4132E3;
        layout.nativeSuperState = profile == EngineProfileId::Ccz65 ? 0x4CE7BF : 0x500ADB;
        layout.nativeSuperBusy = 0x500E0C;
        layout.phaseTable = 0x4750DC;
        layout.phaseCounter = 0x4750DF;
    }
    layout.capabilities = CapabilitySuperDialog|CapabilitySuperMapText|CapabilitySuperDelay|
        CapabilitySuperR|CapabilitySuperS|CapabilityPhase|CapabilityInput|CapabilityExtraDelay|
        CapabilityDialogDisplay|CapabilityExtendedDisplay|
        (layout.phaseOwnedByPatch ? 0u : CapabilityNativeSuper)|
        (layout.dialog3Applicable ? CapabilityDialog3 : 0u);
    if (resolved.profile == profile) {
        for (size_t i = 0; i < EngineSiteCount; ++i)
            if (resolved.sites[i]) layout.sites[i] = resolved.sites[i];
        uintptr_t* destinations[] = {&layout.waitWrapper,&layout.nativeStage,&layout.timerContext,
            &layout.phase,&layout.speed,&layout.sleep,&layout.deadline,&layout.pump,
            &layout.nativeSuper,&layout.nativeSuperCall,&layout.nativeSuperState,
            &layout.nativeSuperBusy,&layout.phaseTable,&layout.phaseCounter};
        const uintptr_t values[] = {resolved.waitWrapper,resolved.nativeStage,resolved.timerContext,
            resolved.phase,resolved.speed,resolved.sleep,resolved.deadline,resolved.pump,
            resolved.nativeSuper,resolved.nativeSuperCall,resolved.nativeSuperState,
            resolved.nativeSuperBusy,resolved.phaseTable,resolved.phaseCounter};
        for (size_t i = 0; i < _countof(values); ++i) if (values[i]) *destinations[i] = values[i];
        layout.waitCallerCount = resolved.waitCallerCount;
        memcpy(layout.waitCallers, resolved.waitCallers, sizeof(layout.waitCallers));
        memcpy(layout.waitKinds, resolved.waitKinds, sizeof(layout.waitKinds));
    }
    SetTemplates(layout);
    ResolveAnimationPresentation(bytes, layout, virtualBase);
    AppendRPresentationLayout(bytes, layout, virtualBase);
    AppendSPresentationLayout(bytes, layout, virtualBase);
    for (size_t i = 0; i < EngineSiteCount; ++i) {
        if (!layout.sites[i]) continue;
        if (!image.Contains(layout.sites[i], kSiteLengths[i]) ||
            !CopyMemoryChecked(layout.siteOriginal[i], image.At(layout.sites[i]), kSiteLengths[i])) return false;
    }
    const uintptr_t dependencies[] = {layout.waitWrapper,layout.nativeStage,layout.timerContext,
        layout.phase,layout.speed,layout.sleep,layout.deadline,layout.pump,
        layout.nativeSuperCall,layout.phaseCounter};
    for (uintptr_t address : dependencies) {
        BYTE captured = 0;
        if (address && (!image.Contains(address, 1) ||
            !CopyMemoryChecked(&captured, image.At(address), 1))) return false;
    }
    // The normal plan builder populates the complete audit record even when
    // a protected zero-fill region has no corresponding bytes on disk.
    BuildEnginePatchPlan(bytes, image.size, layout, layout.plan, virtualBase, EngineAnalysisSource::Runtime);
    for (const EngineSitePlan& site : layout.plan.sites)
        if (site.required && !site.present) return false;
    const EngineSitePlan* dataSites[] = {&layout.plan.nativeRoute,&layout.plan.nativeState,
        &layout.plan.nativeBusy,&layout.plan.phaseTable};
    for (const EngineSitePlan* site : dataSites) if (site->required && !site->present) return false;
    layout.plan.status = EnginePlanStatus::VersionSelected;
    layout.plan.confidence = 0;
    layout.plan.evidence = L"用户确认按版本模板注入；实际运行时原始字节已捕获，"
        L"未要求签名、调用图、可执行节标记或磁盘原始偏移一致。";
    output = layout;
    return true;
}

bool BuildEnginePatchPlan(const BYTE* imageBytes, size_t imageSize, const EngineLayout& layout,
    EnginePatchPlan& plan, uintptr_t virtualBase, EngineAnalysisSource source, uintptr_t runtimeMemoryBase) {
    AnalysisScope scope(source, runtimeMemoryBase);
    plan = EnginePatchPlan();
    plan.profile = layout.profile;
    plan.loadedBase = virtualBase;
    plan.imageSize = imageSize;
    plan.capabilities = layout.capabilities;
    plan.runtimeValidated = false;
    if (layout.profile == EngineProfileId::Unknown || !imageBytes) {
        plan.status = EnginePlanStatus::Rejected;
        plan.evidence = L"没有已验证的引擎布局，不能生成可审计的写入计划。";
        return false;
    }
    Image image = {};
    if (!ParseImage(imageBytes, virtualBase, image) || (imageSize && image.size > imageSize)) {
        plan.status = EnginePlanStatus::Rejected;
        plan.evidence = L"映像头或映像大小无效，不能计算文件偏移。";
        return false;
    }
    plan.loadedBase = image.virtualBase;
    plan.imageSize = image.size;
    bool requiredOkay = true;
    for (size_t i = 0; i < EngineSiteCount; ++i) {
        const EngineSiteIndex index = static_cast<EngineSiteIndex>(i);
        plan.sites[i] = MakePlanSite(image, layout, index, layout.sites[i], kSiteLengths[i],
            layout.siteOriginal[i], layout.siteEnabled[i]);
        if (source == EngineAnalysisSource::Runtime) {
            plan.sites[i].rawFileOffset = SIZE_MAX;
            plan.sites[i].byteSource = EngineSiteByteSource::RuntimeOnly;
        }
        const EngineSitePlan& site = plan.sites[i];
        if (site.required && (!site.present || (source == EngineAnalysisSource::Disk && site.rawFileOffset == SIZE_MAX) ||
            !image.Executable(site.legacyAddress, site.length))) requiredOkay = false;
    }
    plan.nativeRoute = MakePlanSite(image, layout, EngineSiteCount, layout.nativeSuper, 5,
        nullptr, nullptr);
    if (source == EngineAnalysisSource::Runtime) {
        plan.nativeRoute.rawFileOffset = SIZE_MAX;
        plan.nativeRoute.byteSource = EngineSiteByteSource::RuntimeOnly;
    }
    plan.nativeRoute.capability = CapabilityNativeSuper;
    plan.nativeRoute.required = !layout.phaseOwnedByPatch;
    plan.nativeState = MakePlanSite(image, layout, EngineSiteCount, layout.nativeSuperState, 1,
        nullptr, nullptr);
    if (source == EngineAnalysisSource::Runtime) {
        plan.nativeState.rawFileOffset = SIZE_MAX;
        plan.nativeState.byteSource = EngineSiteByteSource::RuntimeOnly;
    }
    plan.nativeState.capability = CapabilityNativeSuper;
    plan.nativeState.required = !layout.phaseOwnedByPatch;
    plan.nativeBusy = MakePlanSite(image, layout, EngineSiteCount, layout.nativeSuperBusy, 1,
        nullptr, nullptr);
    if (source == EngineAnalysisSource::Runtime) {
        plan.nativeBusy.rawFileOffset = SIZE_MAX;
        plan.nativeBusy.byteSource = EngineSiteByteSource::RuntimeOnly;
    }
    plan.nativeBusy.capability = CapabilityNativeSuper;
    plan.nativeBusy.required = !layout.phaseOwnedByPatch;
    plan.phaseTable = MakePlanSite(image, layout, EngineSiteCount, layout.phaseTable, 4,
        nullptr, nullptr);
    if (source == EngineAnalysisSource::Runtime) {
        plan.phaseTable.rawFileOffset = SIZE_MAX;
        plan.phaseTable.byteSource = EngineSiteByteSource::RuntimeOnly;
    }
    plan.phaseTable.capability = CapabilityPhase;
    plan.phaseTable.required = !layout.phaseOwnedByPatch && (layout.capabilities & CapabilityPhase) != 0;
    plan.phaseValue = MakePlanSite(image, layout, EngineSiteCount, layout.phase, 1,
        nullptr, nullptr);
    if (source == EngineAnalysisSource::Runtime) {
        plan.phaseValue.rawFileOffset = SIZE_MAX;
        plan.phaseValue.byteSource = EngineSiteByteSource::RuntimeOnly;
    }
    plan.phaseValue.capability = CapabilityPhase;
    // These globals can live in PE zero-fill (.bss) in shipped builds.  Keep
    // their captured bytes in the plan when a raw mapping exists, while the
    // semantic resolver validates their in-memory bounds and relationships.
    plan.phaseValue.required = false;
    plan.speedValue = MakePlanSite(image, layout, EngineSiteCount, layout.speed, 1,
        nullptr, nullptr);
    if (source == EngineAnalysisSource::Runtime) {
        plan.speedValue.rawFileOffset = SIZE_MAX;
        plan.speedValue.byteSource = EngineSiteByteSource::RuntimeOnly;
    }
    plan.speedValue.capability = CapabilitySuperDelay;
    plan.speedValue.required = false;
    plan.timerContext = MakePlanSite(image, layout, EngineSiteCount, layout.timerContext, 1,
        nullptr, nullptr);
    if (source == EngineAnalysisSource::Runtime) {
        plan.timerContext.rawFileOffset = SIZE_MAX;
        plan.timerContext.byteSource = EngineSiteByteSource::RuntimeOnly;
    }
    plan.timerContext.capability = CapabilitySuperDelay;
    plan.timerContext.required = false;
    const EngineSitePlan* dataSites[] = {&plan.nativeRoute, &plan.nativeState,
        &plan.nativeBusy, &plan.phaseTable, &plan.phaseValue, &plan.speedValue,
        &plan.timerContext};
    for (const EngineSitePlan* site : dataSites)
        if (site->required && (!site->present || (source == EngineAnalysisSource::Disk && site->rawFileOffset == SIZE_MAX))) requiredOkay = false;
    if (!layout.phaseOwnedByPatch && !image.Executable(layout.nativeSuper, 5)) requiredOkay = false;
    const unsigned requiredCapabilities = CapabilitySuperDialog|CapabilitySuperMapText|
        CapabilitySuperDelay|CapabilitySuperR|CapabilitySuperS|CapabilityPhase|CapabilityInput|
        CapabilityExtraDelay|CapabilityDialogDisplay|CapabilityExtendedDisplay|
        (layout.phaseOwnedByPatch ? 0u : CapabilityNativeSuper) |
        (layout.dialog3Applicable ? CapabilityDialog3 : 0u);
    const unsigned missingCapabilities = requiredCapabilities & ~layout.capabilities;
    const bool semanticComplete = !missingCapabilities && !layout.ambiguousWaitCallers;
    plan.confidence = requiredOkay && semanticComplete ? 90 : 50;
    std::wstringstream evidence;
    evidence << L"Resolution=" << (requiredOkay && semanticComplete ? L"Unique" :
        layout.ambiguousSignatureAttempts || layout.ambiguousWaitCallers ? L"Ambiguous" : L"Missing")
        << L"; SignatureAttempts.Unique=" << layout.uniqueSignatureAttempts
        << L"; SignatureAttempts.Missing=" << layout.missingSignatureAttempts
        << L"; SignatureAttempts.Ambiguous=" << layout.ambiguousSignatureAttempts
        << L"; WaitCallers.Unique=" << layout.waitCallerCount
        << L"; WaitCallers.Ambiguous=" << layout.ambiguousWaitCallers
        << L"; MissingCapabilities=0x" << std::hex << missingCapabilities << L"; ";
    plan.evidence = evidence.str();
    if (!requiredOkay) {
        plan.status = EnginePlanStatus::Rejected;
        plan.evidence += L"必需站点存在缺失、不可执行或无法映射到磁盘文件的情况，不能接受写入计划。";
        return false;
    }
    if (!semanticComplete) {
        plan.status = EnginePlanStatus::Rejected;
        plan.evidence += L"加速功能调用关系未全部唯一确认；自动安装已停止。";
        return false;
    }
    plan.status = EnginePlanStatus::AutoVerified;
    plan.evidence += source == EngineAnalysisSource::Disk
        ? L"调用图、站点原始字节和 PE 节到文件偏移映射均已验证。"
        : L"运行时调用图和站点原始字节已验证；页面权限由运行时识别入口另行确认。";
    return true;
}
