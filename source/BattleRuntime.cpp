#include "BattleRuntime.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <utility>
#include <vector>

namespace {

bool DecodeWithCodePage(UINT codePage, DWORD flags, const BYTE* bytes, size_t length,
    std::wstring& result) {
    if (!bytes || !length || length > static_cast<size_t>(INT_MAX)) return false;
    const int count = MultiByteToWideChar(codePage, flags,
        reinterpret_cast<const char*>(bytes), static_cast<int>(length), nullptr, 0);
    if (count <= 0) return false;
    result.assign(static_cast<size_t>(count), L'\0');
    return MultiByteToWideChar(codePage, flags, reinterpret_cast<const char*>(bytes),
        static_cast<int>(length), result.data(), count) == count;
}

// Game data is normally stored as GBK, but a few 6.6x tables were exported
// as UTF-8.  Do not use CP_ACP here: the proxy must render the same names on
// a machine whose system locale is not Chinese.  UTF-8 is accepted only when
// it is explicitly marked or validates completely; otherwise CP936 is the
// stable game-data fallback.
std::wstring DecodeGameName(const BYTE* bytes, size_t length) {
    if (!bytes || !length) return {};
    const BYTE* terminator = static_cast<const BYTE*>(memchr(bytes, 0, length));
    if (terminator) length = static_cast<size_t>(terminator - bytes);
    while (length && (bytes[length - 1] == 0 || bytes[length - 1] == ' ')) --length;
    while (length && (*bytes == 0 || *bytes == ' ')) {
        ++bytes;
        --length;
    }
    if (!bytes || !length) return {};

    bool utf8Marked = length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
    if (utf8Marked) {
        bytes += 3;
        length -= 3;
    }
    std::wstring result;
    if ((utf8Marked || (length >= 2 && std::any_of(bytes, bytes + length,
            [](BYTE value) { return value >= 0x80; }))) &&
        DecodeWithCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, length, result)) {
        return result;
    }
    if (!DecodeWithCodePage(936, MB_ERR_INVALID_CHARS, bytes, length, result) &&
        !DecodeWithCodePage(936, 0, bytes, length, result)) return {};
    return result;
}

struct PeImage {
    const BYTE* bytes = nullptr;
    uintptr_t runtimeBase = 0;
    const IMAGE_NT_HEADERS32* nt = nullptr;
    const IMAGE_SECTION_HEADER* sections = nullptr;
    unsigned sectionCount = 0;

    bool Open(const BYTE* image, uintptr_t virtualBase) {
        bytes = image;
        if (!bytes) return false;
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
            return false;
        nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            return false;
        runtimeBase = virtualBase ? virtualBase : reinterpret_cast<uintptr_t>(image);
        sections = IMAGE_FIRST_SECTION(nt);
        sectionCount = nt->FileHeader.NumberOfSections;
        return sectionCount > 0 && sectionCount < 96;
    }

    bool Range(const char* name, const BYTE*& begin, size_t& size) const {
        for (unsigned i = 0; i < sectionCount; ++i) {
            char sectionName[9] = {};
            memcpy(sectionName, sections[i].Name, 8);
            if (strcmp(sectionName, name)) continue;
            begin = bytes + sections[i].VirtualAddress;
            size = std::max(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData);
            if (sections[i].VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
                size > nt->OptionalHeader.SizeOfImage - sections[i].VirtualAddress) return false;
            return true;
        }
        return false;
    }

    std::vector<std::pair<const BYTE*, size_t>> ExecutableRanges() const {
        std::vector<std::pair<const BYTE*, size_t>> ranges;
        // When resolving the live image, trust committed page attributes over
        // the packed file's section flags. VProtect may publish decoded code
        // in pages whose on-disk section is read-only or whose raw size is a
        // tiny stub. The flat runtime-RVA tests use section metadata instead.
        if (runtimeBase == reinterpret_cast<uintptr_t>(bytes)) {
            const uintptr_t imageEnd = runtimeBase + nt->OptionalHeader.SizeOfImage;
            uintptr_t cursor = runtimeBase;
            while (cursor < imageEnd) {
                MEMORY_BASIC_INFORMATION information = {};
                if (VirtualQuery(reinterpret_cast<const void*>(cursor), &information,
                        sizeof(information)) != sizeof(information)) break;
                const uintptr_t region = reinterpret_cast<uintptr_t>(information.BaseAddress);
                const uintptr_t rawEnd = region + information.RegionSize;
                if (rawEnd <= cursor) break;
                const uintptr_t regionEnd = std::min(rawEnd, imageEnd);
                const DWORD protection = information.Protect & 0xFFu;
                const bool executable = information.State == MEM_COMMIT &&
                    !(information.Protect & PAGE_GUARD) &&
                    (protection == PAGE_EXECUTE_READ ||
                     protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY);
                if (executable) {
                    if (regionEnd > cursor) {
                        const BYTE* begin = bytes + (cursor - runtimeBase);
                        if (!ranges.empty() && ranges.back().first + ranges.back().second == begin)
                            ranges.back().second += static_cast<size_t>(regionEnd - cursor);
                        else ranges.push_back({begin, static_cast<size_t>(regionEnd - cursor)});
                    }
                }
                cursor = regionEnd;
            }
        }
        if (!ranges.empty()) return ranges;
        for (unsigned i = 0; i < sectionCount; ++i) {
            if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const size_t candidate = std::max(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData);
            if (!candidate || sections[i].VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
                candidate > nt->OptionalHeader.SizeOfImage - sections[i].VirtualAddress) continue;
            ranges.push_back({bytes + sections[i].VirtualAddress, candidate});
        }
        return ranges;
    }

    bool ExecutableRange(const BYTE*& begin, size_t& size) const {
        const auto ranges = ExecutableRanges();
        if (ranges.empty()) { begin = nullptr; size = 0; return false; }
        begin = ranges.front().first;
        size = ranges.front().second;
        return true;
    }

    bool InImage(uintptr_t address, size_t size = 1) const {
        return address >= runtimeBase && size <= nt->OptionalHeader.SizeOfImage &&
            address - runtimeBase <= nt->OptionalHeader.SizeOfImage - size;
    }
};

struct RuntimeCodeSpan {
    uintptr_t address = 0;
    std::vector<BYTE> bytes;
    bool operator==(const RuntimeCodeSpan& other) const { return address == other.address && bytes == other.bytes; }
};
struct RuntimeLayout {
    uintptr_t unitBase = 0;
    uintptr_t personPointerSlot = 0;
    uintptr_t itemBase = 0;
    unsigned capabilities = 0;
    std::vector<RuntimeCodeSpan> resourceCode;
};

RuntimeLayout gLayout;
std::array<std::wstring, 255> gItemNames;
SRWLOCK gRuntimeLock = SRWLOCK_INIT;
bool gRuntimeResourceNames = false;
std::mutex gResourceSnapshotLock;
uint64_t gProcessGeneration = 1, gResourceGeneration = 1;
uintptr_t gObservedPersonTable = 0;
BattleResourceSnapshot gLastResourceSnapshot;

void InvalidateResourceSnapshots() {
    std::lock_guard<std::mutex> lock(gResourceSnapshotLock);
    ++gProcessGeneration;
    ++gResourceGeneration;
    gObservedPersonTable = 0;
    gLastResourceSnapshot = BattleResourceSnapshot();
}

bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    const bool validSize = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
        size.QuadPart <= 16 * 1024 * 1024;
    if (!validSize) {
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool okay = bytes.empty() ||
        (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
            read == bytes.size());
    CloseHandle(file);
    if (!okay) bytes.clear();
    return okay;
}

bool LoadItemTable(const std::wstring& path, size_t offset, size_t count, int firstId,
    std::array<std::wstring, 255>& names) {
    constexpr size_t kRowSize = 25;
    constexpr size_t kNameBytes = 17;
    std::vector<BYTE> bytes;
    if (!ReadFileBytes(path, bytes) || offset > bytes.size() ||
        count > (bytes.size() - offset) / kRowSize || firstId < 0 ||
        static_cast<size_t>(firstId) + count > names.size()) return false;
    size_t readable = 0;
    for (size_t index = 0; index < count; ++index) {
        const BYTE* raw = bytes.data() + offset + index * kRowSize;
        const std::wstring name = DecodeGameName(raw, kNameBytes);
        if (name.empty() || name.size() > 32) continue;
        bool valid = true;
        for (wchar_t ch : name) if (ch < 0x20) { valid = false; break; }
        if (!valid) continue;
        names[static_cast<size_t>(firstId) + index] = name;
        ++readable;
    }
    return readable >= count / 5;
}

bool LoadItemNames(const wchar_t* gameRoot, std::array<std::wstring, 255>& names) {
    names = {};
    if (!gameRoot || !*gameRoot) return false;
    const std::wstring root(gameRoot);
    const bool low = LoadItemTable(root + L"\\Data.e5", 33164, 104, 0, names);
    const bool high = LoadItemTable(root + L"\\Star.e5", 0, 151, 104, names);
    return low || high;
}

bool ReadMemory(uintptr_t address, void* output, size_t size) {
    if (!address || !output || !size) return false;
    bool ok = false;
    __try {
        memcpy(output, reinterpret_cast<const void*>(address), size);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

bool IsReadable(uintptr_t address, size_t size) {
    if (!address || !size) return false;
    MEMORY_BASIC_INFORMATION information = {};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &information, sizeof(information)) ||
        information.State != MEM_COMMIT || (information.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (information.Protect & readable) != 0 && address >= reinterpret_cast<uintptr_t>(information.BaseAddress) &&
        size <= information.RegionSize - (address - reinterpret_cast<uintptr_t>(information.BaseAddress));
}

bool IsReadableSpan(uintptr_t address, size_t size) {
    if (!address || !size || size > UINTPTR_MAX - address) return false;
    const uintptr_t end = address + size;
    while (address < end) {
        MEMORY_BASIC_INFORMATION information = {};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &information,
                sizeof(information)) != sizeof(information)) return false;
        const uintptr_t region = reinterpret_cast<uintptr_t>(information.BaseAddress);
        if (region > address || information.RegionSize > UINTPTR_MAX - region) return false;
        const uintptr_t next = region + information.RegionSize;
        if (next <= address || !IsReadable(address,
                static_cast<size_t>(std::min(next, end) - address))) return false;
        address = std::min(next, end);
    }
    return true;
}

bool ImageMatches(const PeImage& pe, uintptr_t address, const BYTE* expected,
    size_t length) {
    return pe.InImage(address, length) &&
        !memcmp(pe.bytes + (address - pe.runtimeBase), expected, length);
}

void AddResourceCode(const PeImage& pe, const BYTE* code, size_t size,
    std::vector<RuntimeCodeSpan>& evidence) {
    RuntimeCodeSpan span;
    span.address = pe.runtimeBase + static_cast<uintptr_t>(code - pe.bytes);
    span.bytes.assign(code, code + size);
    evidence.push_back(std::move(span));
}

bool ResolveItemTable(const PeImage& pe, const BYTE* text, size_t textSize,
    uintptr_t& itemBase, std::vector<RuntimeCodeSpan>* evidence = nullptr) {
    itemBase = 0;
    size_t getters = 0;
    uintptr_t candidate = 0;
    std::vector<RuntimeCodeSpan> proof;
    for (size_t i = 0; i + 14 <= textSize; ++i) {
        const BYTE* p = text + i;
        // Native accessor: ECX = id * 25 + table. ID 255 is a sentinel.
        static const BYTE prefix[] = {0x55,0x8B,0xEC,0x6B,0xC9,0x19,0x81,0xC1};
        if (memcmp(p, prefix, sizeof(prefix)) || p[12] != 0x5D || p[13] != 0xC3)
            continue;
        uint32_t value = 0;
        memcpy(&value, p + 8, sizeof(value));
        if (pe.InImage(value, 256u * 25u)) {
            candidate = value;
            ++getters;
            AddResourceCode(pe, p, 14, proof);
        }
    }
    if (getters != 1) return false;
    size_t loaders = 0;
    for (size_t i = 0; i + 35 <= textSize; ++i) {
        const BYTE* p = text + i;
        if (p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC || p[3] != 0x68 ||
            p[8] != 0x6A || p[9] != 1 || p[10] != 0x68 || p[15] != 0xE8 ||
            p[20] != 0xB9 || p[25] != 0xBA || p[30] != 0xE8) continue;
        uint32_t low = 0, high = 0, dataName = 0, starName = 0;
        memcpy(&low, p + 4, 4); memcpy(&dataName, p + 11, 4);
        memcpy(&high, p + 21, 4); memcpy(&starName, p + 26, 4);
        static const BYTE data[] = {'D','A','T','A','.','E','5',0};
        static const BYTE star[] = {'s','t','a','r','.','e','5',0};
        if (low != candidate || high != candidate + 104u * 25u ||
            !ImageMatches(pe, dataName, data, sizeof(data)) ||
            !ImageMatches(pe, starName, star, sizeof(star))) continue;
        int32_t dataRelative = 0, starRelative = 0;
        memcpy(&dataRelative, p + 16, 4); memcpy(&starRelative, p + 31, 4);
        const uintptr_t loader = pe.runtimeBase + static_cast<uintptr_t>(p - pe.bytes);
        const uintptr_t dataTarget = loader + 20 + dataRelative;
        const uintptr_t starTarget = loader + 35 + starRelative;
        static const BYTE dataOpen[] = {0x55,0x8B,0xEC,0x6A,0xFF};
        static const BYTE starOpen[] = {0x55,0x8B,0xEC,0x51,0xB9};
        static const BYTE starLength[] = {0x68,0xD8,0x0E,0,0,0xFF,0x75,0xFC};
        if (!ImageMatches(pe, dataTarget, dataOpen, sizeof(dataOpen)) ||
            !ImageMatches(pe, starTarget, starOpen, sizeof(starOpen)) ||
            !ImageMatches(pe, starTarget + 20, starLength, sizeof(starLength))) continue;
        ++loaders;
        AddResourceCode(pe, p, 35, proof);
        AddResourceCode(pe, pe.bytes + (dataTarget - pe.runtimeBase), 5, proof);
        AddResourceCode(pe, pe.bytes + (starTarget - pe.runtimeBase), 28, proof);
    }
    if (loaders != 1) return false;
    itemBase = candidate;
    if (evidence) *evidence = std::move(proof);
    return true;
}

bool ResolveDeployment(const PeImage& pe, const BYTE* text, size_t textSize, const BYTE* unitGetter) {
    const auto match = [text,textSize](const BYTE* at,std::initializer_list<int> pattern) {
        if (!at || at < text || static_cast<size_t>(at - text) > textSize ||
            pattern.size() > textSize - static_cast<size_t>(at - text)) return false;
        size_t index = 0;
        for (int value : pattern) {
            if (value >= 0 && at[index] != value) return false;
            ++index;
        }
        return true;
    };
    const auto call = [text,textSize](const BYTE* at) -> const BYTE* {
        if (at < text || static_cast<size_t>(at - text) > textSize ||
            5 > textSize - static_cast<size_t>(at - text) || *at != 0xE8) return nullptr;
        int32_t delta = 0; memcpy(&delta,at + 1,4);
        const int64_t target = static_cast<int64_t>(at - text) + 5 + delta;
        return target >= 0 && static_cast<uint64_t>(target) < textSize ? text + target : nullptr;
    };
    const BYTE* visibility = nullptr;
    unsigned accessors = 0, loops = 0, coordinateConsumers = 0;
    for (size_t offset = 0; offset + 9 <= textSize; ++offset) {
        const BYTE* at = text + offset;
        if (match(at,{0x55,0x8B,0xEC,0x0F,0xB6,0x41,0x0C,0x5D,0xC3})) {
            visibility = at; ++accessors;
        }
    }
    if (accessors != 1 || !unitGetter) return false;
    for (size_t offset = 0; offset + 0x45 <= textSize; ++offset) {
        const BYTE* at = text + offset;
        if (match(at,{0x55,0x8B,0xEC,0x6A,0,0x83,0xEC,8,0xEB,3,0xFF,0x45,0xFC,
                0x33,0xC0,0x8B,0x4D,0xFC,0x80,0xF9,0xFA,0x73,0x68,0xE8}) &&
            call(at + 0x17) == unitGetter && match(at + 0x1C,{0x89,0x4D,0xF8,0xE8}) &&
            call(at + 0x1F) == visibility && match(at + 0x24,{0x3C,2,0x75,0xE2})) ++loops;
        if (call(at) != visibility || !match(at + 5,{0x3C,2,0x75,-1,0x8B,0x4D,0xFC,
                0x8B,0x55,0xF8,0x80,0xFA,0xFA,0x73,0x0B,0x8B,0x45,0xF4,0x88,4,0x11,
                0xFF,0x45,0xF8,0xEB,-1,0x8B,0x55,0xFC,0x8A,0x0A,0x80,0xF9,0xFF,0x74,-1,
                0x83,0x7D,0xF0,0,0x74,-1,0xE8}) || call(at + 0x2F) != unitGetter ||
            !match(at + 0x34,{0xFF,0x71,7,0xFF,0x71,6,0xB9,-1,-1,-1,-1,0xE8}) ||
            !call(at + 0x3F)) continue;
        ++coordinateConsumers;
    }
    (void)pe;
    return loops == 1 && coordinateConsumers == 1;
}

bool ResolveLayout(const BYTE* image, uintptr_t virtualBase, RuntimeLayout& layout) {
    PeImage pe;
    if (!pe.Open(image, virtualBase)) return false;
    const auto ranges = pe.ExecutableRanges();
    if (ranges.empty()) return false;

    uintptr_t unitBase = 0;
    uintptr_t personSlot = 0;
    size_t unitMatches = 0;
    size_t personMatches = 0;
    const BYTE* unitGetter = nullptr;
    const BYTE* personGetter = nullptr;
    std::vector<RuntimeCodeSpan> itemCode;
    bool deployment = false;
    uintptr_t itemBase = 0;
    size_t itemMatches = 0;
    for (const auto& range : ranges) {
        const BYTE* text = range.first;
        const size_t textSize = range.second;
        for (size_t i = 0; i + 20 <= textSize; ++i) {
            const BYTE* p = text + i;
        // push ebp; mov ebp,esp; movzx ecx,cl; imul ecx,ecx,30h; add ecx,imm32; pop ebp; ret
            if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x0F && p[4] == 0xB6 &&
            p[5] == 0xC9 && p[6] == 0x6B && p[7] == 0xC9 && p[8] == 0x30 &&
            p[9] == 0x81 && p[10] == 0xC1 && p[15] == 0x5D && p[16] == 0xC3) {
            uint32_t value = 0;
            memcpy(&value, p + 11, sizeof(value));
            if (pe.InImage(value, 0x30u * 250u)) {
                unitBase = value;
                unitGetter = p;
                ++unitMatches;
            }
        }
        // push ebp; mov ebp,esp; movzx ecx,word [ebp+8]; imul ecx,48h; add ecx,[slot]
            if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x0F && p[4] == 0xB7 &&
            p[5] == 0x4D && p[6] == 0x08 && p[7] == 0x6B && p[8] == 0xC9 && p[9] == 0x48 &&
            p[10] == 0x03 && p[11] == 0x0D && p[16] == 0x5D && p[17] == 0xC2 &&
            p[18] == 0x04 && p[19] == 0x00) {
            uint32_t value = 0;
            memcpy(&value, p + 12, sizeof(value));
            if (pe.InImage(value, sizeof(uintptr_t))) {
                personSlot = value;
                personGetter = p;
                ++personMatches;
            }
            }
        }
        if (ResolveDeployment(pe, text, textSize, unitGetter)) deployment = true;
        uintptr_t candidate = 0;
        std::vector<RuntimeCodeSpan> candidateCode;
        if (ResolveItemTable(pe, text, textSize, candidate, &candidateCode)) {
            if (!itemBase) itemBase = candidate;
            ++itemMatches;
            if (itemBase != candidate) itemBase = 0;
            itemCode = std::move(candidateCode);
        }
    }
    if (unitMatches != 1) return false;
    layout = RuntimeLayout();
    layout.unitBase = unitBase;
    AddResourceCode(pe, unitGetter, 17, layout.resourceCode);
    layout.capabilities = BattleCapabilityUnits | BattleCapabilityStatuses | BattleCapabilityAi;
    if (deployment) layout.capabilities |= BattleCapabilityDeployment;
    if (personMatches == 1) {
        layout.personPointerSlot = personSlot;
        AddResourceCode(pe, personGetter, 20, layout.resourceCode);
        layout.capabilities |= BattleCapabilityPeople | BattleCapabilityEquipment;
    }
    if (itemMatches == 1 && itemBase) {
        layout.itemBase = itemBase;
        layout.resourceCode.insert(layout.resourceCode.end(), itemCode.begin(), itemCode.end());
    }
    return true;
}

std::wstring DecodeName(const BYTE* bytes, size_t length) {
    size_t used = 0;
    while (used < length && bytes[used]) ++used;
    while (used && bytes[used - 1] == ' ') --used;
    if (!used) return {};
    std::wstring result = DecodeGameName(bytes, used);
    if (result.empty() || result.size() > 32) return {};
    for (wchar_t ch : result) if (ch < 0x20) return {};
    return result;
}

std::wstring DecodeRuntimeName(const BYTE* bytes, size_t length) {
    size_t used = 0;
    while (used < length && bytes[used]) ++used;
    while (used && bytes[used - 1] == ' ') --used;
    if (!used) return {};
    std::wstring name;
    if (!DecodeWithCodePage(936, MB_ERR_INVALID_CHARS, bytes, used, name) ||
        name.empty() || name.size() > length) return {};
    for (wchar_t ch : name)
        if (ch < 0x20 || ch == 0x7F || ch == 0xFFFD) return {};
    return name;
}

bool SnapshotNameTable(uintptr_t table, size_t count, size_t stride, size_t offset,
    size_t nameBytes, std::vector<std::wstring>& names) {
    names.assign(count, {});
    if (!table || count > SIZE_MAX / stride ||
        !IsReadableSpan(table, count * stride)) return false;
    const size_t size = count * stride;
    std::vector<BYTE> first(size), second(size);
    if (!ReadMemory(table, first.data(), size) || !ReadMemory(table, second.data(), size))
        return false;
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        const BYTE* a = first.data() + i * stride + offset;
        const BYTE* b = second.data() + i * stride + offset;
        if (memcmp(a, b, nameBytes)) {
            names.assign(count, {});
            return false;
        }
        if (stride == 0x48 && offset == 8) {
            uint16_t beforeTag = 0, afterTag = 0;
            memcpy(&beforeTag, a - offset, sizeof(beforeTag));
            memcpy(&afterTag, b - offset, sizeof(afterTag));
            if (beforeTag != 0x1000u + i || afterTag != beforeTag) continue;
        }
        names[i] = DecodeRuntimeName(a, nameBytes);
        found = found || !names[i].empty();
    }
    return found;
}

bool ReadResourceNames(const RuntimeLayout& layout,
    std::vector<std::wstring>& people, std::vector<std::wstring>& items) {
    people.clear();
    items.clear();
    if (!layout.personPointerSlot && !layout.itemBase) return false;
    uintptr_t table = 0, after = 0;
    bool havePeople = false;
    if (layout.personPointerSlot &&
        IsReadable(layout.personPointerSlot, sizeof(table)) &&
        ReadMemory(layout.personPointerSlot, &table, sizeof(table)) &&
        table >= 0x10000 && (table % alignof(uint32_t)) == 0) {
        havePeople = SnapshotNameTable(table, 1024, 0x48, 8, 8, people) &&
            ReadMemory(layout.personPointerSlot, &after, sizeof(after)) && after == table;
        if (!havePeople) people.clear();
    }
    const bool haveItems = layout.itemBase &&
        SnapshotNameTable(layout.itemBase, 255, 25, 0, 17, items);
    if (!haveItems) items.clear();
    return havePeople || haveItems;
}

bool ReadPerson(uintptr_t table, const std::array<std::wstring, 255>& itemNames,
    BattleUnitSnapshot& unit) {
    if (unit.dataId < 0 || unit.dataId >= 1024) return false;
    BYTE person[0x48] = {};
    const uintptr_t address = table + static_cast<uintptr_t>(unit.dataId) * sizeof(person);
    if (!IsReadable(address, sizeof(person)) || !ReadMemory(address, person, sizeof(person))) return false;
    unit.name = DecodeName(person + 8, 8);
    unit.level = person[0x2C];
    unit.weaponId = person[0x2E];
    unit.hasWeapon = unit.weaponId != 0xFF;
    if (!unit.hasWeapon) unit.weaponId = -1;
    unit.weaponLevel = unit.hasWeapon ? person[0x2F] : -1;
    unit.armorId = person[0x31];
    unit.hasArmor = unit.armorId != 0xFF;
    if (!unit.hasArmor) unit.armorId = -1;
    unit.armorLevel = unit.hasArmor ? person[0x32] : -1;
    unit.accessoryId = person[0x34];
    unit.hasAccessory = unit.accessoryId != 0xFF;
    if (!unit.hasAccessory) unit.accessoryId = -1;
    if (unit.weaponId >= 0 && unit.weaponId < static_cast<int>(itemNames.size()))
        unit.weaponName = itemNames[static_cast<size_t>(unit.weaponId)];
    if (unit.armorId >= 0 && unit.armorId < static_cast<int>(itemNames.size()))
        unit.armorName = itemNames[static_cast<size_t>(unit.armorId)];
    if (unit.accessoryId >= 0 && unit.accessoryId < static_cast<int>(itemNames.size()))
        unit.accessoryName = itemNames[static_cast<size_t>(unit.accessoryId)];
    return !unit.name.empty() || unit.level <= 99;
}
}

bool InitializeBattleRuntime(const wchar_t* gameRoot) {
    ResetBattleRuntime();
    RuntimeLayout layout;
    std::array<std::wstring, 255> itemNames;
    LoadItemNames(gameRoot, itemNames);
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module || !ResolveLayout(reinterpret_cast<const BYTE*>(module),
        reinterpret_cast<uintptr_t>(module), layout)) return false;
    AcquireSRWLockExclusive(&gRuntimeLock);
    gLayout = layout;
    gItemNames = std::move(itemNames);
    ReleaseSRWLockExclusive(&gRuntimeLock);
    return true;
}

bool ReadBattlePersonName(uintptr_t peoplePointerSlot, int personId, std::wstring& name) {
    name.clear();
    uintptr_t people = 0;
    BYTE raw[8] = {};
    if (personId < 0 || personId >= 1024 || !peoplePointerSlot ||
        !ReadMemory(peoplePointerSlot,&people,sizeof(DWORD)) || !people) return false;
    const uintptr_t address = people + static_cast<uintptr_t>(personId) * 0x48 + 8;
    if (!IsReadable(address,sizeof(raw)) || !ReadMemory(address,raw,sizeof(raw))) return false;
    name = DecodeName(raw,sizeof(raw));
    return !name.empty();
}

void ResetBattleRuntime() {
    AcquireSRWLockExclusive(&gRuntimeLock);
    gLayout = RuntimeLayout();
    gItemNames = {};
    gRuntimeResourceNames = false;
    ReleaseSRWLockExclusive(&gRuntimeLock);
    InvalidateResourceSnapshots();
}

void ConfigureBattleRuntimeResourceNames(bool enabled) noexcept {
    AcquireSRWLockExclusive(&gRuntimeLock);
    const bool changed = gRuntimeResourceNames != enabled;
    gRuntimeResourceNames = enabled;
    ReleaseSRWLockExclusive(&gRuntimeLock);
    if (changed) InvalidateResourceSnapshots();
}

bool ReadBattleResourceSnapshot(BattleResourceSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(gResourceSnapshotLock);
    snapshot = BattleResourceSnapshot();
    snapshot.profile = CompiledEngineProfile();
    snapshot.processId = GetCurrentProcessId();
    FILETIME created = {}, exited = {}, kernel = {}, user = {};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        snapshot.processCreatedAt = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    snapshot.moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    snapshot.capturedAt = GetTickCount64();
    AcquireSRWLockShared(&gRuntimeLock);
    const bool enabled = gRuntimeResourceNames;
    RuntimeLayout layout = gLayout;
    ReleaseSRWLockShared(&gRuntimeLock);
    bool codeValid = true;
    for (const auto& span : layout.resourceCode) {
        std::vector<BYTE> actual(span.bytes.size());
        if (!ReadMemory(span.address, actual.data(), actual.size()) || actual != span.bytes) {
            codeValid = false;
            break;
        }
    }
    // A delayed unpacker may publish the accessors after startup. Re-resolve
    // missing/changed code, rather than caching the first failed probe forever.
    if (enabled && (!layout.capabilities || !layout.personPointerSlot || !layout.itemBase || !codeValid)) {
        RuntimeLayout refreshed;
        const bool resolved = ResolveLayout(reinterpret_cast<const BYTE*>(snapshot.moduleBase),
            snapshot.moduleBase, refreshed);
        if ((!codeValid || resolved) && (layout.capabilities != refreshed.capabilities ||
                layout.unitBase != refreshed.unitBase || layout.personPointerSlot != refreshed.personPointerSlot ||
                layout.itemBase != refreshed.itemBase || layout.resourceCode != refreshed.resourceCode)) {
            layout = std::move(refreshed);
            AcquireSRWLockExclusive(&gRuntimeLock);
            gLayout = layout;
            ReleaseSRWLockExclusive(&gRuntimeLock);
            ++gProcessGeneration;
            ++gResourceGeneration;
            gLastResourceSnapshot = BattleResourceSnapshot();
        }
    }
    snapshot.runtimeProfileReady = enabled && layout.capabilities != 0;
    snapshot.peopleLayoutReady = snapshot.runtimeProfileReady && layout.personPointerSlot != 0;
    snapshot.itemsLayoutReady = snapshot.runtimeProfileReady && layout.itemBase != 0;
    uintptr_t personTable = 0;
    if (snapshot.peopleLayoutReady)
        ReadMemory(layout.personPointerSlot, &personTable, sizeof(personTable));
    if (enabled) ReadResourceNames(layout, snapshot.people, snapshot.items);
    snapshot.peopleReady = !snapshot.people.empty();
    snapshot.itemsReady = !snapshot.items.empty();
    snapshot.peopleOrigin = snapshot.peopleReady ? BattleResourceOrigin::Runtime : BattleResourceOrigin::Unavailable;
    snapshot.itemOrigin = snapshot.itemsReady ? BattleResourceOrigin::Runtime : BattleResourceOrigin::Unavailable;
    if (personTable != gObservedPersonTable ||
        snapshot.people != gLastResourceSnapshot.people || snapshot.items != gLastResourceSnapshot.items ||
        snapshot.peopleReady != gLastResourceSnapshot.peopleReady || snapshot.itemsReady != gLastResourceSnapshot.itemsReady ||
        snapshot.peopleLayoutReady != gLastResourceSnapshot.peopleLayoutReady ||
        snapshot.itemsLayoutReady != gLastResourceSnapshot.itemsLayoutReady) {
        ++gResourceGeneration;
        gObservedPersonTable = personTable;
    }
    snapshot.processGeneration = gProcessGeneration;
    snapshot.resourceGeneration = gResourceGeneration;
    if (!enabled) snapshot.diagnostic = L"运行时名称能力未启用。";
    else if (!snapshot.runtimeProfileReady) snapshot.diagnostic = L"运行时资源布局尚未就绪。";
    else if (!snapshot.peopleReady || !snapshot.itemsReady)
        snapshot.diagnostic = L"运行时名称部分未就绪：人物=" + std::to_wstring(snapshot.peopleReady) +
            L"，物品=" + std::to_wstring(snapshot.itemsReady) + L"。";
    gLastResourceSnapshot = snapshot;
    return snapshot.peopleReady || snapshot.itemsReady;
}

bool RefreshBattleResourceSnapshot() {
    BattleResourceSnapshot snapshot;
    return ReadBattleResourceSnapshot(snapshot);
}

bool IsBattleResourceSnapshotCurrent(const BattleResourceSnapshot& snapshot) {
    BattleResourceSnapshot current;
    ReadBattleResourceSnapshot(current);
    return snapshot.processId == current.processId &&
        snapshot.processCreatedAt == current.processCreatedAt && snapshot.moduleBase == current.moduleBase &&
        snapshot.profile == current.profile && snapshot.processGeneration == current.processGeneration &&
        snapshot.resourceGeneration == current.resourceGeneration;
}

bool ReadBattleResourceNames(std::vector<std::wstring>& people,
    std::vector<std::wstring>& items) {
    BattleResourceSnapshot snapshot;
    const bool ready = ReadBattleResourceSnapshot(snapshot);
    people = std::move(snapshot.people);
    items = std::move(snapshot.items);
    return ready;
}

bool BattleRuntimeResourceNamesEnabled() noexcept {
    AcquireSRWLockShared(&gRuntimeLock);
    const bool enabled = gRuntimeResourceNames;
    ReleaseSRWLockShared(&gRuntimeLock);
    return enabled;
}

unsigned BattleRuntimeCapabilities() {
    AcquireSRWLockShared(&gRuntimeLock);
    const unsigned result = gLayout.capabilities;
    ReleaseSRWLockShared(&gRuntimeLock);
    return result;
}

bool ReadBattleSnapshot(BattleSnapshot& snapshot) {
    RuntimeLayout layout;
    std::array<std::wstring, 255> itemNames;
    AcquireSRWLockShared(&gRuntimeLock);
    layout = gLayout;
    itemNames = gItemNames;
    const bool runtimeNames = gRuntimeResourceNames;
    ReleaseSRWLockShared(&gRuntimeLock);
    snapshot = BattleSnapshot();
    if (!(layout.capabilities & BattleCapabilityUnits) ||
        !IsReadable(layout.unitBase, 0x30u * 250u)) return false;

    if (runtimeNames && layout.itemBase) {
        std::vector<std::wstring> nativeItems;
        if (SnapshotNameTable(layout.itemBase, 255, 25, 0, 17, nativeItems)) {
            for (size_t i = 0; i < itemNames.size(); ++i)
                if (!nativeItems[i].empty()) itemNames[i] = std::move(nativeItems[i]);
        }
    }

    uintptr_t people = 0;
    bool peopleReady = layout.personPointerSlot && ReadMemory(layout.personPointerSlot, &people, sizeof(people)) &&
        people && IsReadable(people, 0x48);
    snapshot.capabilities = layout.capabilities;
    if (!peopleReady) snapshot.capabilities &= ~(BattleCapabilityPeople | BattleCapabilityEquipment);
    snapshot.capturedAt = GetTickCount64();
    snapshot.units.reserve(64);
    for (int slot = 0; slot < 250; ++slot) {
        BYTE record[0x30] = {};
        if (!ReadMemory(layout.unitBase + static_cast<uintptr_t>(slot) * sizeof(record), record, sizeof(record)))
            return false;
        uint16_t dataId = 0;
        memcpy(&dataId, record, sizeof(dataId));
        if (dataId == 0xFFFF) continue;
        const int camp = record[5];
        const int visibility = record[0x0C];
        int hp = 0, mp = 0;
        memcpy(&hp, record + 0x10, sizeof(hp));
        memcpy(&mp, record + 0x14, sizeof(mp));
        // The native 6.6 HP setter clamps at 9999.  Larger values here are
        // stale pre-battle bytes that happen to resemble TacticalUnit rows.
        if (camp > 15 || visibility > 7 || hp < -1 || hp > 9999 || mp < -1 || mp > 9999)
            continue;
        BattleUnitSnapshot unit;
        unit.slot = slot;
        unit.dataId = dataId;
        unit.camp = camp;
        unit.x = record[6];
        unit.y = record[7];
        unit.targetSlot = record[8] == 0xFF ? -1 : record[8];
        unit.targetX = record[9];
        unit.targetY = record[0x0A];
        unit.actionOrder = record[0x0B];
        unit.visibility = visibility;
        unit.actionState = record[0x0D];
        unit.aiPolicy = record[0x0E];
        unit.direction = record[0x0F];
        unit.hp = hp;
        unit.mp = mp;
        unit.status = record[0x1E];
        if (peopleReady) ReadPerson(people, itemNames, unit);
        snapshot.units.push_back(std::move(unit));
    }
    return true;
}

std::wstring BattleCampName(int camp) {
    if (camp == 0) return L"我军";
    if (camp == 1) return L"友军";
    return L"敌军";
}

std::wstring BattleAiName(int policy) {
    switch (policy) {
    case 0: return L"指定目标";
    case 1: return L"主动攻击";
    case 2: return L"坚守";
    case 3: return L"被动攻击";
    case 4: return L"跟随或撤离";
    case 7: return L"玩家控制";
    default: return {};
    }
}

std::wstring BattleStatusText(int status) {
    struct State { int shift; const wchar_t* name; } states[] = {
        {0, L"麻痹"}, {2, L"禁咒"}, {4, L"混乱"}, {6, L"中毒"}
    };
    std::wstring result;
    for (const State& state : states) {
        const int turns = (status >> state.shift) & 3;
        if (!turns) continue;
        if (!result.empty()) result += L"、";
        result += state.name;
        result += L"（" + std::to_wstring(turns) + L"）";
    }
    return result;
}

std::wstring BattleUnitLabel(const BattleUnitSnapshot& unit) {
    if (!unit.name.empty()) return unit.name;
    return BattleCampName(unit.camp) + L"单位（初始槽位 " + std::to_wstring(unit.slot + 1) + L"）";
}

bool ResolveBattleRuntimeImageForTest(const BYTE* image, uintptr_t virtualBase,
    uintptr_t& unitBase, uintptr_t& personPointerSlot) {
    RuntimeLayout layout;
    if (!ResolveLayout(image, virtualBase, layout)) return false;
    unitBase = layout.unitBase;
    personPointerSlot = layout.personPointerSlot;
    return true;
}

bool ResolveBattleDeploymentForTest(const BYTE* image, uintptr_t virtualBase) {
    RuntimeLayout layout;
    return ResolveLayout(image,virtualBase,layout) && (layout.capabilities & BattleCapabilityDeployment) != 0;
}

bool ResolveBattleItemTableForTest(const BYTE* image, uintptr_t virtualBase,
    uintptr_t& itemBase) {
    itemBase = 0;
    PeImage pe;
    if (!pe.Open(image, virtualBase)) return false;
    const auto ranges = pe.ExecutableRanges();
    size_t matches = 0;
    for (const auto& range : ranges) {
        uintptr_t candidate = 0;
        if (!ResolveItemTable(pe, range.first, range.second, candidate)) continue;
        if (!itemBase) itemBase = candidate;
        ++matches;
        if (itemBase != candidate) itemBase = 0;
    }
    return matches == 1 && itemBase != 0;
}

bool LoadBattleItemNameForTest(const wchar_t* gameRoot, int itemId, std::wstring& name) {
    name.clear();
    if (itemId < 0 || itemId >= 255) return false;
    std::array<std::wstring, 255> names;
    if (!LoadItemNames(gameRoot, names)) return false;
    name = names[static_cast<size_t>(itemId)];
    return !name.empty();
}

#ifdef XA66_TEST
void ConfigureBattleResourceLayoutForTest(uintptr_t personPointerSlot, uintptr_t itemBase) {
    ResetBattleRuntime();
    AcquireSRWLockExclusive(&gRuntimeLock);
    gLayout.personPointerSlot = personPointerSlot;
    gLayout.itemBase = itemBase;
    gLayout.capabilities = BattleCapabilityUnits;
    gRuntimeResourceNames = true;
    ReleaseSRWLockExclusive(&gRuntimeLock);
}

bool ReadBattleResourceNamesForTest(uintptr_t personPointerSlot, uintptr_t itemBase,
    std::vector<std::wstring>& people, std::vector<std::wstring>& items) {
    RuntimeLayout layout;
    layout.personPointerSlot = personPointerSlot;
    layout.itemBase = itemBase;
    return ReadResourceNames(layout, people, items);
}
#endif
