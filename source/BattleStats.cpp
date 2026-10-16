#include "BattleStats.h"

#include "BattleRuntime.h"
#include "Guide.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr size_t kUnitStride = 0x30;
constexpr size_t kUnitCount = 250;
constexpr size_t kStrategyStride = 0x61;
constexpr size_t kStrategyTypeOffset = 0x0B;
enum SettlementKind : int {
    SettlementPhysicalDamage = 0,
    SettlementStrategyDamage = 1,
    SettlementRecoverySimple = 2,
    SettlementRecoveryAnimated = 3
};

constexpr size_t kSettlementCapabilityCount = 3;
constexpr size_t kSettlementCallCount = 4;
constexpr size_t kRecoveryCapability = 2;

struct PeImage {
    const BYTE* bytes = nullptr;
    uintptr_t virtualBase = 0;
    const IMAGE_NT_HEADERS32* nt = nullptr;
    const IMAGE_SECTION_HEADER* sections = nullptr;
    unsigned sectionCount = 0;

    bool Open(const BYTE* image, uintptr_t base) {
        bytes = image;
        if (!bytes) return false;
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
            return false;
        nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            return false;
        virtualBase = base ? base : reinterpret_cast<uintptr_t>(image);
        sections = IMAGE_FIRST_SECTION(nt);
        sectionCount = nt->FileHeader.NumberOfSections;
        return sectionCount > 0 && sectionCount < 96;
    }

    bool ExecutableRange(const BYTE*& begin, size_t& size) const {
        begin = nullptr;
        size = 0;
        for (unsigned i = 0; i < sectionCount; ++i) {
            if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const size_t candidate = std::max(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData);
            if (!candidate || sections[i].VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
                candidate > nt->OptionalHeader.SizeOfImage - sections[i].VirtualAddress) continue;
            if (begin) continue;
            begin = bytes + sections[i].VirtualAddress;
            size = candidate;
        }
        return begin && size;
    }

    bool InImage(uintptr_t address, size_t size = 1) const {
        return address >= virtualBase && size <= nt->OptionalHeader.SizeOfImage &&
            address - virtualBase <= nt->OptionalHeader.SizeOfImage - size;
    }

    const BYTE* At(uintptr_t address, size_t size) const {
        return InImage(address, size) ? bytes + (address - virtualBase) : nullptr;
    }

    uintptr_t Address(const BYTE* pointer) const {
        return virtualBase + static_cast<uintptr_t>(pointer - bytes);
    }
};

struct ResolvedLayout {
    uintptr_t hpSetter = 0;
    uintptr_t unitBase = 0;
    uintptr_t strategyBase = 0;
    unsigned strategyCount = 0;
    std::array<uintptr_t, kSettlementCallCount> callSites = {};
    uintptr_t physicalResult = 0;
    uintptr_t physicalResultCall = 0;
    uintptr_t criticalCommit = 0;
    uint32_t criticalOffset = 0;
    uint32_t comboOffset = 0;
    bool physicalEvents = false;
};

struct CodePatch {
    BYTE* site = nullptr;
    size_t length = 0;
    std::array<BYTE, 16> original = {};
    std::array<BYTE, 16> patched = {};
    void* thunk = nullptr;
    bool installed = false;
};

struct RuntimeLayout {
    uintptr_t hpSetter = 0;
    uintptr_t unitBase = 0;
    uintptr_t strategyBase = 0;
    unsigned strategyCount = 0;
    std::array<BYTE*, kSettlementCallCount> callSites = {};
    std::array<std::array<BYTE, 5>, kSettlementCallCount> original = {};
    std::array<std::array<BYTE, 5>, kSettlementCallCount> patched = {};
    std::array<void*, kSettlementCallCount> thunks = {};
    std::array<bool, kSettlementCallCount> installed = {};
    CodePatch physicalResult;
    CodePatch criticalCommit;
    uint32_t criticalOffset = 0;
    uint32_t comboOffset = 0;
};

struct PhysicalActionState {
    uintptr_t context = 0;
    unsigned long long serial = 0;
    int actorSlot = -1;
    std::vector<unsigned> criticalKeys;
    std::vector<unsigned> comboOrdinals;
};

std::wstring gRoot;
RuntimeLayout gLayout;
std::map<int, BattleUnitTotals> gTotals;
SRWLOCK gStatsLock = SRWLOCK_INIT;
bool gHookAvailable = false;
std::array<bool, kSettlementCapabilityCount> gSettlementAvailable = {};
bool gCriticalAvailable = false;
bool gComboAvailable = false;
bool gActive = false;
ULONGLONG gStarted = 0;
ULONGLONG gLastPoll = 0;
ULONGLONG gEmptySince = 0;
std::wstring gScenario;
volatile LONG gRecording = FALSE;
volatile LONG gEventRecording = FALSE;
PhysicalActionState gPhysicalAction;
unsigned long long gNextActionSerial = 0;

uintptr_t CallTarget(const PeImage& image, const BYTE* instruction) {
    if (!instruction || instruction[0] != 0xE8) return 0;
    int32_t displacement = 0;
    memcpy(&displacement, instruction + 1, sizeof(displacement));
    return image.Address(instruction + 5) + displacement;
}

bool ValidateHpSetter(const PeImage& image, uintptr_t address) {
    const BYTE* p = image.At(address, 0x3A);
    if (!p) return false;
    const BYTE prefix[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x89, 0x4D, 0xF8, 0xFF, 0x31, 0xE8};
    const BYTE suffix[] = {0x8B, 0xE5, 0x5D, 0xC2, 0x04, 0x00};
    return !memcmp(p, prefix, sizeof(prefix)) && !memcmp(p + 0x34, suffix, sizeof(suffix));
}

const BYTE* FindFunctionStart(const BYTE* text, size_t textSize, const BYTE* location) {
    if (!text || !location || location < text || location >= text + textSize) return nullptr;
    const size_t offset = static_cast<size_t>(location - text);
    const size_t lower = offset > 0x300 ? offset - 0x300 : 0;
    for (size_t cursor = offset; cursor-- > lower;) {
        const BYTE* p = text + cursor;
        if (cursor + 5 <= textSize && p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC &&
            p[3] == 0x83 && p[4] == 0xEC) return p;
    }
    return nullptr;
}

bool ContainsBytes(const BYTE* begin, size_t size, const BYTE* pattern, size_t patternSize) {
    if (!begin || !pattern || !patternSize || patternSize > size) return false;
    for (size_t index = 0; index + patternSize <= size; ++index)
        if (!memcmp(begin + index, pattern, patternSize)) return true;
    return false;
}

bool ResolvePhysicalEvents(const PeImage& image, const BYTE* text, size_t textSize,
    ResolvedLayout& result) {
    struct Candidate {
        uintptr_t resultFunction = 0;
        uintptr_t resultCall = 0;
        uintptr_t criticalCommit = 0;
        uint32_t criticalOffset = 0;
        uint32_t comboOffset = 0;
    };
    std::vector<Candidate> candidates;
    for (size_t index = 0; index + 24 <= textSize; ++index) {
        const BYTE* p = text + index;
        if (p[0] != 0x8B || p[1] != 0x45 || p[3] != 0xC7 || p[4] != 0x80 ||
            p[9] != 0x01 || p[10] || p[11] || p[12] || p[13] != 0x8B ||
            p[14] != 0x4D || p[15] != p[2] || p[16] != 0x80 || p[17] != 0xB9 || p[22])
            continue;
        uint32_t criticalOffset = 0;
        uint32_t comboOffset = 0;
        memcpy(&criticalOffset, p + 5, sizeof(criticalOffset));
        memcpy(&comboOffset, p + 18, sizeof(comboOffset));
        if (criticalOffset < 0x100 || comboOffset != criticalOffset + 4 || comboOffset > 0x2000)
            continue;

        const BYTE* resultFunction = FindFunctionStart(text, textSize, p);
        if (!resultFunction || p - resultFunction > 0x280) continue;
        const BYTE targetStore[] = {0x88, 0x51, 0x01};
        if (!ContainsBytes(resultFunction, static_cast<size_t>(p - resultFunction),
            targetStore, sizeof(targetStore))) continue;

        std::vector<const BYTE*> resultCalls;
        for (size_t callIndex = 4; callIndex + 12 <= textSize; ++callIndex) {
            const BYTE* call = text + callIndex;
            if (call[0] != 0xE8 || CallTarget(image, call) != image.Address(resultFunction)) continue;
            if (call[-4] == 0x55 && call[-3] == 0x8B && call[-2] == 0xEC && call[-1] == 0x51 &&
                call[5] == 0x8B && call[6] == 0x4D && call[7] == 0xFC && call[8] == 0xE8)
                resultCalls.push_back(call);
        }
        if (resultCalls.size() != 1) continue;
        const BYTE* resultCall = resultCalls[0];
        const uintptr_t wrapper = image.Address(resultCall - 4);

        size_t comboMatches = 0;
        for (size_t comboIndex = 0; comboIndex + 80 <= textSize; ++comboIndex) {
            const BYTE* q = text + comboIndex;
            if (q[0] != 0xC6 || q[1] != 0x82 || q[6] != 0x00 || q[7] != 0xEB ||
                q[9] != 0x8B || q[10] != 0x45 || q[12] != 0xFE || q[13] != 0x80)
                continue;
            uint32_t resetOffset = 0;
            uint32_t incrementOffset = 0;
            memcpy(&resetOffset, q + 2, sizeof(resetOffset));
            memcpy(&incrementOffset, q + 14, sizeof(incrementOffset));
            if (resetOffset != comboOffset || incrementOffset != comboOffset) continue;
            bool callsWrapper = false;
            bool loopsToIncrement = false;
            for (size_t tail = 20; tail + 5 <= 80; ++tail) {
                if (q[tail] == 0xE8 && CallTarget(image, q + tail) == wrapper)
                    callsWrapper = true;
                if (q[tail] == 0x85 && q[tail + 1] == 0xC0 && q[tail + 2] == 0x75) {
                    const int8_t displacement = static_cast<int8_t>(q[tail + 3]);
                    if (q + tail + 4 + displacement == q + 9) loopsToIncrement = true;
                }
            }
            if (callsWrapper && loopsToIncrement) ++comboMatches;
        }
        if (comboMatches != 1) continue;
        Candidate candidate;
        candidate.resultFunction = image.Address(resultFunction);
        candidate.resultCall = image.Address(resultCall);
        candidate.criticalCommit = image.Address(p + 3);
        candidate.criticalOffset = criticalOffset;
        candidate.comboOffset = comboOffset;
        candidates.push_back(candidate);
    }
    if (candidates.size() != 1) return false;
    result.physicalResult = candidates[0].resultFunction;
    result.physicalResultCall = candidates[0].resultCall;
    result.criticalCommit = candidates[0].criticalCommit;
    result.criticalOffset = candidates[0].criticalOffset;
    result.comboOffset = candidates[0].comboOffset;
    result.physicalEvents = true;
    return true;
}

bool ResolveStrategyTable(const PeImage& image, const BYTE* text, size_t textSize,
    uintptr_t& strategyBase, unsigned& strategyCount) {
    struct Candidate {
        uintptr_t accessor = 0;
        uintptr_t typeAccessor = 0;
        uintptr_t base = 0;
        unsigned count = 0;
    };
    std::vector<Candidate> candidates;
    for (size_t index = 0; index + 14 <= textSize; ++index) {
        const BYTE* accessor = text + index;
        if (accessor[0] != 0x55 || accessor[1] != 0x8B || accessor[2] != 0xEC ||
            accessor[3] != 0x6B || accessor[4] != 0xC9 || accessor[5] != kStrategyStride ||
            accessor[6] != 0x81 || accessor[7] != 0xC1 || accessor[12] != 0x5D ||
            accessor[13] != 0xC3) continue;
        uint32_t base = 0;
        memcpy(&base, accessor + 8, sizeof(base));
        if (!image.InImage(base, kStrategyStride)) continue;

        std::vector<const BYTE*> typeAccessors;
        for (size_t typeIndex = 0; typeIndex + 9 <= textSize; ++typeIndex) {
            const BYTE* type = text + typeIndex;
            if (type[0] == 0x55 && type[1] == 0x8B && type[2] == 0xEC &&
                type[3] == 0x0F && type[4] == 0xB6 && type[5] == 0x41 &&
                type[6] == kStrategyTypeOffset && type[7] == 0x5D && type[8] == 0xC3)
                typeAccessors.push_back(type);
        }
        if (typeAccessors.size() != 1) continue;

        size_t linkedCalls = 0;
        for (size_t callIndex = 0; callIndex + 10 <= textSize; ++callIndex) {
            const BYTE* call = text + callIndex;
            if (call[0] == 0xE8 && call[5] == 0xE8 &&
                CallTarget(image, call) == image.Address(accessor) &&
                CallTarget(image, call + 5) == image.Address(typeAccessors[0]))
                ++linkedCalls;
        }
        if (!linkedCalls) continue;

        std::vector<unsigned> counts;
        for (size_t dispatchIndex = 0; dispatchIndex + 25 <= textSize; ++dispatchIndex) {
            const BYTE* p = text + dispatchIndex;
            if (p[0] != 0x0F || p[1] != 0xB6 || p[2] != 0x01 || p[3] != 0x90 ||
                p[4] != 0x3C || !p[5] || p[6] != 0x0F || p[7] != 0x83 ||
                p[12] != 0x8A || p[13] != 0x80 || p[18] != 0xFF || p[19] != 0x24 ||
                p[20] != 0x85) continue;
            uint32_t dispatchTable = 0;
            uint32_t jumpTable = 0;
            memcpy(&dispatchTable, p + 14, sizeof(dispatchTable));
            memcpy(&jumpTable, p + 21, sizeof(jumpTable));
            if (image.InImage(dispatchTable, p[5]) &&
                image.InImage(jumpTable, sizeof(uint32_t))) counts.push_back(p[5]);
        }
        std::sort(counts.begin(), counts.end());
        counts.erase(std::unique(counts.begin(), counts.end()), counts.end());
        if (counts.size() != 1 || !image.InImage(base, kStrategyStride * counts[0])) continue;
        candidates.push_back({image.Address(accessor), image.Address(typeAccessors[0]),
            base, counts[0]});
    }
    if (candidates.size() != 1) return false;
    strategyBase = candidates[0].base;
    strategyCount = candidates[0].count;
    return true;
}

bool ResolveLayout(const BYTE* imageBytes, uintptr_t virtualBase, ResolvedLayout& result) {
    PeImage image;
    if (!image.Open(imageBytes, virtualBase)) return false;
    const BYTE* text = nullptr;
    size_t textSize = 0;
    if (!image.ExecutableRange(text, textSize)) return false;

    std::vector<uintptr_t> damageSites;
    std::vector<uintptr_t> recoverySites;
    uintptr_t hpSetter = 0;
    uintptr_t unitBase = 0;
    size_t unitMatches = 0;
    for (size_t i = 0; i + 24 <= textSize; ++i) {
        const BYTE* p = text + i;
        if (p[0] == 0x2B && p[1] == 0x45 && p[2] == 0xEC && p[3] == 0x50 &&
            p[4] == 0x8B && p[5] == 0x4D && p[6] == 0xE0 && p[7] == 0xE8) {
            const uintptr_t target = CallTarget(image, p + 7);
            if (ValidateHpSetter(image, target)) {
                damageSites.push_back(image.Address(p + 7));
                if (!hpSetter) hpSetter = target;
                else if (hpSetter != target) return false;
            }
        }
        if (p[0] == 0x8B && p[1] == 0x4D && p[2] == 0xFC &&
            p[3] == 0x8B && p[4] == 0x55 && p[5] == 0xF4 &&
            p[6] == 0x2B && p[7] == 0x84 && p[8] == 0x8A &&
            p[9] == 0x08 && p[10] == 0x01 && p[11] == 0x00 && p[12] == 0x00 &&
            ((p[13] == 0x50 && p[14] == 0x8B && p[15] == 0x4D && p[16] == 0xF0) ||
             (p[13] == 0x8B && p[14] == 0x4D && p[15] == 0xEC && p[16] == 0x50)) &&
            p[17] == 0xE8) {
            const uintptr_t target = CallTarget(image, p + 17);
            if (ValidateHpSetter(image, target)) {
                recoverySites.push_back(image.Address(p + 17));
                if (!hpSetter) hpSetter = target;
                else if (hpSetter != target) return false;
            }
        }
        if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x0F &&
            p[4] == 0xB6 && p[5] == 0xC9 && p[6] == 0x6B && p[7] == 0xC9 &&
            p[8] == 0x30 && p[9] == 0x81 && p[10] == 0xC1 && p[15] == 0x5D && p[16] == 0xC3) {
            uint32_t candidate = 0;
            memcpy(&candidate, p + 11, sizeof(candidate));
            if (image.InImage(candidate, kUnitStride * kUnitCount)) {
                unitBase = candidate;
                ++unitMatches;
            }
        }
    }
    if (unitMatches != 1 || !hpSetter) return false;
    result = ResolvedLayout();
    result.hpSetter = hpSetter;
    result.unitBase = unitBase;
    if (damageSites.size() == 2) {
        std::sort(damageSites.begin(), damageSites.end());
        result.callSites[0] = damageSites[0];
        result.callSites[1] = damageSites[1];
    }
    // Recovery is based on the final HP delta, so it must not depend on
    // resolving a particular strategy ID or strategy-table type.  Keep the
    // table probe below for diagnostics and existing layout tests, but let
    // the two validated HP-setter result loops stand on their own.
    ResolveStrategyTable(image, text, textSize, result.strategyBase, result.strategyCount);
    if (recoverySites.size() == 2) {
        std::sort(recoverySites.begin(), recoverySites.end());
        result.callSites[2] = recoverySites[0];
        result.callSites[3] = recoverySites[1];
    }
    ResolvePhysicalEvents(image, text, textSize, result);
    return std::any_of(result.callSites.begin(), result.callSites.end(),
        [](uintptr_t site) { return site != 0; }) || result.physicalEvents;
}

bool ReadBytes(uintptr_t address, void* output, size_t size) {
    if (!address || !output || !size) return false;
    bool okay = false;
    __try {
        memcpy(output, reinterpret_cast<const void*>(address), size);
        okay = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        okay = false;
    }
    return okay;
}

bool WriteCode(BYTE* address, const BYTE* bytes, size_t size) {
    DWORD protection = 0;
    if (!address || !bytes || !size ||
        !VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &protection)) return false;
    memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD ignored = 0;
    VirtualProtect(address, size, protection, &ignored);
    return true;
}

void* ReachableTarget(BYTE* instruction, uintptr_t target, void*& allocation) {
    allocation = nullptr;
    const int64_t direct = static_cast<int64_t>(target) - reinterpret_cast<uintptr_t>(instruction + 5);
    if (direct >= INT32_MIN && direct <= INT32_MAX) return reinterpret_cast<void*>(target);
    const uintptr_t center = reinterpret_cast<uintptr_t>(instruction) & ~uintptr_t(0xFFFF);
    for (uintptr_t distance = 0; distance <= 0x7F000000; distance += 0x10000) {
        const uintptr_t candidates[] = {center + distance, center >= distance ? center - distance : 0};
        for (uintptr_t candidate : candidates) {
            if (!candidate) continue;
            BYTE* thunk = static_cast<BYTE*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 16,
                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (!thunk) continue;
            const int64_t relative = reinterpret_cast<uintptr_t>(thunk) -
                reinterpret_cast<uintptr_t>(instruction + 5);
            if (relative < INT32_MIN || relative > INT32_MAX) {
                VirtualFree(thunk, 0, MEM_RELEASE);
                continue;
            }
            thunk[0] = 0xB8;
            const uint32_t absolute = static_cast<uint32_t>(target);
            memcpy(thunk + 1, &absolute, sizeof(absolute));
            thunk[5] = 0xFF;
            thunk[6] = 0xE0;
            FlushInstructionCache(GetCurrentProcess(), thunk, 7);
            allocation = thunk;
            return thunk;
        }
    }
    return nullptr;
}

bool PrepareCallPatch(const PeImage& image, uintptr_t address, uintptr_t expectedTarget,
    uintptr_t bridge, CodePatch& patch) {
    if (!address || !expectedTarget || !bridge) return false;
    BYTE* site = reinterpret_cast<BYTE*>(address);
    if (site[0] != 0xE8 || CallTarget(image, site) != expectedTarget) return false;
    patch = CodePatch();
    patch.site = site;
    patch.length = 5;
    memcpy(patch.original.data(), site, patch.length);
    void* target = ReachableTarget(site, bridge, patch.thunk);
    if (!target) return false;
    patch.patched[0] = 0xE8;
    const int32_t displacement = static_cast<int32_t>(reinterpret_cast<BYTE*>(target) - (site + 5));
    memcpy(patch.patched.data() + 1, &displacement, sizeof(displacement));
    return true;
}

bool PrepareCriticalPatch(uintptr_t address, uint32_t offset, uintptr_t bridge,
    CodePatch& patch) {
    if (!address || !offset || !bridge) return false;
    BYTE expected[10] = {0xC7, 0x80, 0, 0, 0, 0, 1, 0, 0, 0};
    memcpy(expected + 2, &offset, sizeof(offset));
    BYTE* site = reinterpret_cast<BYTE*>(address);
    if (memcmp(site, expected, sizeof(expected))) return false;
    patch = CodePatch();
    patch.site = site;
    patch.length = sizeof(expected);
    memcpy(patch.original.data(), site, patch.length);
    std::fill(patch.patched.begin(), patch.patched.end(), static_cast<BYTE>(0x90));
    void* target = ReachableTarget(site, bridge, patch.thunk);
    if (!target) return false;
    patch.patched[0] = 0xE8;
    const int32_t displacement = static_cast<int32_t>(reinterpret_cast<BYTE*>(target) - (site + 5));
    memcpy(patch.patched.data() + 1, &displacement, sizeof(displacement));
    return true;
}

bool InstallCodePatch(CodePatch& patch) {
    if (!patch.site || !patch.length || patch.length > patch.original.size() ||
        memcmp(patch.site, patch.original.data(), patch.length) ||
        !WriteCode(patch.site, patch.patched.data(), patch.length)) return false;
    patch.installed = true;
    return true;
}

bool RestoreCodePatch(CodePatch& patch) {
    if (!patch.site) return true;
    bool restored = true;
    if (patch.installed) {
        if (!memcmp(patch.site, patch.patched.data(), patch.length))
            restored = WriteCode(patch.site, patch.original.data(), patch.length);
        else if (memcmp(patch.site, patch.original.data(), patch.length))
            restored = false;
    }
    if (patch.thunk && restored) {
        VirtualFree(patch.thunk, 0, MEM_RELEASE);
        patch.thunk = nullptr;
    }
    return restored;
}

bool UnitSlotFromPointer(uintptr_t pointer, int& slot) {
    const uintptr_t base = gLayout.unitBase;
    if (!base || pointer < base) return false;
    const uintptr_t delta = pointer - base;
    if (delta % kUnitStride || delta / kUnitStride >= kUnitCount) return false;
    slot = static_cast<int>(delta / kUnitStride);
    return true;
}

bool UnitCamp(int slot, int& camp) {
    if (slot < 0 || slot >= static_cast<int>(kUnitCount)) return false;
    BYTE value = 0;
    if (!ReadBytes(gLayout.unitBase + static_cast<uintptr_t>(slot) * kUnitStride + 5,
        &value, sizeof(value)) || value > 15) return false;
    camp = value;
    return true;
}

bool ResolvePhysicalSettlement(uintptr_t callerFrame, int targetSlot, int& actorSlot) {
    uintptr_t context = 0;
    uintptr_t actor = 0;
    BYTE contextActor = 0xFF;
    BYTE contextTarget = 0xFF;
    if (!ReadBytes(callerFrame - 0x0C, &context, sizeof(context)) || !context ||
        !ReadBytes(context + 0x0C, &actor, sizeof(actor)) ||
        !ReadBytes(context, &contextActor, sizeof(contextActor)) ||
        !ReadBytes(context + 1, &contextTarget, sizeof(contextTarget))) return false;
    if (!UnitSlotFromPointer(actor, actorSlot) || contextActor != actorSlot ||
        contextTarget != targetSlot) return false;
    return true;
}

bool ResolveStrategyDamageSettlement(uintptr_t callerFrame, int targetSlot, int& actorSlot) {
    uintptr_t context = 0;
    uintptr_t actor = 0;
    BYTE contextActor = 0xFF;
    BYTE contextTarget = 0xFF;
    if (!ReadBytes(callerFrame - 0x04, &context, sizeof(context)) || !context ||
        !ReadBytes(context + 0x08, &actor, sizeof(actor)) ||
        !ReadBytes(context + 1, &contextActor, sizeof(contextActor)) ||
        !ReadBytes(context + 0x0C, &contextTarget, sizeof(contextTarget))) return false;
    return UnitSlotFromPointer(actor, actorSlot) && contextActor == actorSlot &&
        contextTarget == targetSlot;
}

bool ResolveSettlementActor(int kind, uintptr_t callerFrame,
    int targetSlot, int& actorSlot) {
    switch (kind) {
    case SettlementPhysicalDamage:
        return ResolvePhysicalSettlement(callerFrame, targetSlot, actorSlot);
    case SettlementStrategyDamage:
        return ResolveStrategyDamageSettlement(callerFrame, targetSlot, actorSlot);
    default:
        return false;
    }
}

void Append(std::wstring& text, const std::wstring& line) {
    text += line;
    text += L"\r\n";
}

bool EnsureFolder(const std::wstring& path) {
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

void ClearTotalsUnlocked() {
    gTotals.clear();
    gPhysicalAction = PhysicalActionState();
    gNextActionSerial = 0;
    gActive = false;
    gStarted = 0;
    gEmptySince = 0;
    gScenario.clear();
}

void FinishUnlocked(ULONGLONG now) {
    if (!gActive || gTotals.empty()) {
        ClearTotalsUnlocked();
        return;
    }
    const std::wstring privateRoot = gRoot + L"\\patch\\xiaoan66";
    const std::wstring records = privateRoot + L"\\records";
    EnsureFolder(gRoot + L"\\patch");
    EnsureFolder(privateRoot);
    EnsureFolder(records);
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    wchar_t fileName[96] = {};
    swprintf_s(fileName, L"\\%04u%02u%02u-%02u%02u%02u.txt", time.wYear, time.wMonth,
        time.wDay, time.wHour, time.wMinute, time.wSecond);
    std::wstring output = L"战斗记录\r\n";
    if (!gScenario.empty()) Append(output, L"剧本：" + gScenario);
    Append(output, L"持续时间：" + std::to_wstring((now - gStarted) / 1000) + L" 秒");
    Append(output, L"");
    for (const auto& pair : gTotals) {
        const BattleUnitTotals& unit = pair.second;
        Append(output, unit.label.empty() ? L"我军单位" : unit.label);
        if (gSettlementAvailable[SettlementPhysicalDamage] &&
            gSettlementAvailable[SettlementStrategyDamage]) {
            Append(output, L"  造成伤害：" + std::to_wstring(unit.damageDealt));
            Append(output, L"  承受伤害：" + std::to_wstring(unit.damageReceived));
        }
        if (gSettlementAvailable[kRecoveryCapability])
            Append(output, L"  回复量：" + std::to_wstring(unit.recoveryDone));
        if (gCriticalAvailable)
            Append(output, L"  暴击次数：" + std::to_wstring(unit.criticalCount));
        if (gComboAvailable)
            Append(output, L"  连击次数：" + std::to_wstring(unit.comboExtraHitCount));
    }
    const std::wstring path = records + fileName;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const wchar_t bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(file, &bom, sizeof(bom), &written, nullptr);
        WriteFile(file, output.data(), static_cast<DWORD>(output.size() * sizeof(wchar_t)),
            &written, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    ClearTotalsUnlocked();
}

void RecordSettlement(int kind, uintptr_t target, uintptr_t callerFrame, int oldHp, int finalHp) {
    if (InterlockedCompareExchange(&gRecording, TRUE, FALSE) != FALSE) return;
    int targetSlot = -1;
    int targetCamp = -1;
    const bool validTarget = UnitSlotFromPointer(target, targetSlot) &&
        UnitCamp(targetSlot, targetCamp);
    if (validTarget) {
        AcquireSRWLockExclusive(&gStatsLock);
        if (gActive) {
            if (finalHp > oldHp && targetCamp == 0) {
                BattleUnitTotals& targetTotals = gTotals[targetSlot];
                targetTotals.slot = targetSlot;
                targetTotals.recoveryDone += static_cast<long long>(finalHp) - oldHp;
            } else if (oldHp > finalHp) {
                int actorSlot = -1;
                int actorCamp = -1;
                if (ResolveSettlementActor(kind, callerFrame, targetSlot, actorSlot) &&
                    UnitCamp(actorSlot, actorCamp)) {
                    const long long damage = static_cast<long long>(oldHp) - finalHp;
                    if (actorCamp == 0) {
                        BattleUnitTotals& actorTotals = gTotals[actorSlot];
                        actorTotals.slot = actorSlot;
                        actorTotals.damageDealt += damage;
                        if (targetCamp != 0 && oldHp > 0 && finalHp <= 0)
                            ++actorTotals.defeatedEnemies;
                    }
                    if (targetCamp == 0) {
                        BattleUnitTotals& targetTotals = gTotals[targetSlot];
                        targetTotals.slot = targetSlot;
                        targetTotals.damageReceived += damage;
                    }
                }
            }
        }
        ReleaseSRWLockExclusive(&gStatsLock);
    }
    InterlockedExchange(&gRecording, FALSE);
}

bool ReadPhysicalEvent(uintptr_t context, uint32_t comboOffset, bool requireTarget,
    int& actorSlot, int& targetSlot, unsigned& ordinal) {
    BYTE actor = 0xFF;
    BYTE target = 0xFF;
    BYTE combo = 0;
    if (!context || !comboOffset ||
        !ReadBytes(context, &actor, sizeof(actor)) ||
        !ReadBytes(context + comboOffset, &combo, sizeof(combo)) ||
        actor >= kUnitCount) return false;
    if (requireTarget && (!ReadBytes(context + 1, &target, sizeof(target)) ||
        target >= kUnitCount)) return false;
    actorSlot = actor;
    targetSlot = requireTarget ? target : -1;
    ordinal = combo;
    int camp = -1;
    return UnitCamp(actorSlot, camp) && camp == 0;
}

void RecordPhysicalResultStart(uintptr_t context) {
    if (InterlockedCompareExchange(&gEventRecording, TRUE, FALSE) != FALSE) return;
    int actorSlot = -1;
    int targetSlot = -1;
    unsigned ordinal = 0;
    const bool valid = ReadPhysicalEvent(context, gLayout.comboOffset, false,
        actorSlot, targetSlot, ordinal);
    if (valid) {
        AcquireSRWLockExclusive(&gStatsLock);
        if (gActive) {
            if (!ordinal) {
                gPhysicalAction = PhysicalActionState();
                gPhysicalAction.context = context;
                gPhysicalAction.serial = ++gNextActionSerial;
                gPhysicalAction.actorSlot = actorSlot;
            } else if (gComboAvailable && gPhysicalAction.context == context &&
                gPhysicalAction.actorSlot == actorSlot &&
                std::find(gPhysicalAction.comboOrdinals.begin(),
                    gPhysicalAction.comboOrdinals.end(), ordinal) ==
                    gPhysicalAction.comboOrdinals.end()) {
                gPhysicalAction.comboOrdinals.push_back(ordinal);
                BattleUnitTotals& totals = gTotals[actorSlot];
                totals.slot = actorSlot;
                ++totals.comboExtraHitCount;
            }
        }
        ReleaseSRWLockExclusive(&gStatsLock);
    }
    InterlockedExchange(&gEventRecording, FALSE);
}

void RecordCriticalCommit(uintptr_t context) {
    if (InterlockedCompareExchange(&gEventRecording, TRUE, FALSE) != FALSE) return;
    int actorSlot = -1;
    int targetSlot = -1;
    unsigned ordinal = 0;
    const bool valid = ReadPhysicalEvent(context, gLayout.comboOffset, true,
        actorSlot, targetSlot, ordinal);
    if (valid) {
        AcquireSRWLockExclusive(&gStatsLock);
        if (gActive && gCriticalAvailable && gPhysicalAction.context == context &&
            gPhysicalAction.actorSlot == actorSlot && gPhysicalAction.serial) {
            const unsigned key = (ordinal << 8) | static_cast<unsigned>(targetSlot);
            if (std::find(gPhysicalAction.criticalKeys.begin(),
                gPhysicalAction.criticalKeys.end(), key) ==
                gPhysicalAction.criticalKeys.end()) {
                gPhysicalAction.criticalKeys.push_back(key);
                BattleUnitTotals& totals = gTotals[actorSlot];
                totals.slot = actorSlot;
                ++totals.criticalCount;
            }
        }
        ReleaseSRWLockExclusive(&gStatsLock);
    }
    InterlockedExchange(&gEventRecording, FALSE);
}

} // namespace

extern "C" uintptr_t gBattleStatsOriginalSetHp = 0;
extern "C" uintptr_t gBattleStatsOriginalPhysicalResult = 0;
extern "C" uint32_t gBattleStatsCriticalOffset = 0;

extern "C" void __cdecl BattleStatsRecordHp(
    uintptr_t target, uintptr_t callerFrame, int oldHp, int finalHp, int kind) {
    RecordSettlement(kind, target, callerFrame, oldHp, finalHp);
}

extern "C" void __cdecl BattleStatsRecordPhysicalResult(uintptr_t context) {
    RecordPhysicalResultStart(context);
}

extern "C" void __cdecl BattleStatsRecordCritical(uintptr_t context) {
    RecordCriticalCommit(context);
}

extern "C" __declspec(naked) void BattleStatsHpBridgeCommon() {
    __asm {
        // At entry [esp] is the settlement kind pushed by the small bridge,
        // [esp+4] is the game's return address and [esp+8] is the HP value.
        // Keep the complete caller frame while invoking the original setter;
        // the previous offsets were one word short after PUSHAD and read the
        // new HP value as if it were the unit pointer.
        pushad
        pushfd
        sub esp, 8

        // After PUSHAD/PUSHFD/SUB, saved ECX is at +36 and the original
        // stack pointer (the kind word) is at +24.
        mov ecx, dword ptr [esp + 36]
        mov eax, dword ptr [ecx + 10h]
        mov dword ptr [esp], eax
        mov eax, dword ptr [esp + 24]
        mov edx, dword ptr [eax]
        mov dword ptr [esp + 4], edx
        mov ecx, dword ptr [esp + 36]
        push dword ptr [eax + 8]
        call dword ptr [gBattleStatsOriginalSetHp]

        mov ecx, dword ptr [esp + 36]
        mov edx, dword ptr [ecx + 10h]
        // Push RecordHp(target, callerFrame, oldHp, finalHp, kind).
        push dword ptr [esp + 4]
        push edx
        push dword ptr [esp + 8]
        push dword ptr [esp + 32]
        push dword ptr [esp + 52]
        call BattleStatsRecordHp
        add esp, 20

        add esp, 8
        popfd
        popad
        add esp, 4
        ret 4
    }
}

extern "C" __declspec(naked) void BattleStatsPhysicalBridge() {
    __asm {
        push SettlementPhysicalDamage
        jmp BattleStatsHpBridgeCommon
    }
}

extern "C" __declspec(naked) void BattleStatsStrategyDamageBridge() {
    __asm {
        push SettlementStrategyDamage
        jmp BattleStatsHpBridgeCommon
    }
}

extern "C" __declspec(naked) void BattleStatsRecoveryBridge() {
    __asm {
        push SettlementRecoverySimple
        jmp BattleStatsHpBridgeCommon
    }
}

extern "C" __declspec(naked) void BattleStatsRecoveryAnimatedBridge() {
    __asm {
        push SettlementRecoveryAnimated
        jmp BattleStatsHpBridgeCommon
    }
}

extern "C" __declspec(naked) void BattleStatsPhysicalResultBridge() {
    __asm {
        pushfd
        pushad
        push ecx
        call BattleStatsRecordPhysicalResult
        add esp, 4
        popad
        popfd
        jmp dword ptr [gBattleStatsOriginalPhysicalResult]
    }
}

extern "C" __declspec(naked) void BattleStatsCriticalBridge() {
    __asm {
        pushfd
        add dword ptr [esp + 4], 5
        push edx
        mov edx, dword ptr [gBattleStatsCriticalOffset]
        mov dword ptr [eax + edx], 1
        pop edx
        pushad
        push eax
        call BattleStatsRecordCritical
        add esp, 4
        popad
        popfd
        ret
    }
}

void InitializeBattleStats(const wchar_t* gameRoot) {
    ShutdownBattleStats();
    gRoot = gameRoot ? gameRoot : L"";
    ResetBattleStats();
    HMODULE module = GetModuleHandleW(nullptr);
    ResolvedLayout resolved;
    if (!module || !ResolveLayout(reinterpret_cast<const BYTE*>(module),
        reinterpret_cast<uintptr_t>(module), resolved)) return;

    RuntimeLayout pending;
    pending.hpSetter = resolved.hpSetter;
    pending.unitBase = resolved.unitBase;
    pending.strategyBase = resolved.strategyBase;
    pending.strategyCount = resolved.strategyCount;
    pending.criticalOffset = resolved.criticalOffset;
    pending.comboOffset = resolved.comboOffset;
    const uintptr_t bridges[] = {
        reinterpret_cast<uintptr_t>(&BattleStatsPhysicalBridge),
        reinterpret_cast<uintptr_t>(&BattleStatsStrategyDamageBridge),
        reinterpret_cast<uintptr_t>(&BattleStatsRecoveryBridge),
        reinterpret_cast<uintptr_t>(&BattleStatsRecoveryAnimatedBridge)
    };
    PeImage image;
    if (!image.Open(reinterpret_cast<const BYTE*>(module), reinterpret_cast<uintptr_t>(module)))
        return;
    for (size_t index = 0; index < pending.callSites.size(); ++index) {
        if (!resolved.callSites[index]) continue;
        CodePatch patch;
        if (!PrepareCallPatch(image, resolved.callSites[index], resolved.hpSetter,
            bridges[index], patch)) continue;
        pending.callSites[index] = patch.site;
        memcpy(pending.original[index].data(), patch.original.data(), 5);
        memcpy(pending.patched[index].data(), patch.patched.data(), 5);
        pending.thunks[index] = patch.thunk;
    }
    if (resolved.physicalEvents) {
        PrepareCallPatch(image, resolved.physicalResultCall, resolved.physicalResult,
            reinterpret_cast<uintptr_t>(&BattleStatsPhysicalResultBridge), pending.physicalResult);
        if (pending.physicalResult.site)
            PrepareCriticalPatch(resolved.criticalCommit, resolved.criticalOffset,
                reinterpret_cast<uintptr_t>(&BattleStatsCriticalBridge), pending.criticalCommit);
    }

    gBattleStatsOriginalSetHp = pending.hpSetter;
    gBattleStatsOriginalPhysicalResult = resolved.physicalResult;
    gBattleStatsCriticalOffset = resolved.criticalOffset;
    for (size_t index = 0; index < pending.callSites.size(); ++index) {
        if (!pending.callSites[index]) continue;
        if (memcmp(pending.callSites[index], pending.original[index].data(), 5) ||
            !WriteCode(pending.callSites[index], pending.patched[index].data(), 5)) continue;
        pending.installed[index] = true;
    }
    const bool recoveryComplete = pending.callSites[0] && pending.callSites[1] &&
        pending.callSites[2] && pending.callSites[3] && pending.installed[0] &&
        pending.installed[1] && pending.installed[2] && pending.installed[3];
    InstallCodePatch(pending.physicalResult);
    if (pending.physicalResult.installed) InstallCodePatch(pending.criticalCommit);

    AcquireSRWLockExclusive(&gStatsLock);
    gLayout = pending;
    gSettlementAvailable[SettlementPhysicalDamage] = pending.installed[0];
    gSettlementAvailable[SettlementStrategyDamage] = pending.installed[1];
    gSettlementAvailable[kRecoveryCapability] = recoveryComplete;
    gComboAvailable = pending.physicalResult.installed;
    gCriticalAvailable = pending.physicalResult.installed && pending.criticalCommit.installed;
    gHookAvailable = gCriticalAvailable || gComboAvailable ||
        std::any_of(gSettlementAvailable.begin(), gSettlementAvailable.end(),
            [](bool value) { return value; });
    ReleaseSRWLockExclusive(&gStatsLock);
}

void PollBattleStats(ULONGLONG now) {
    if (gRoot.empty() || now - gLastPoll < 250) return;
    gLastPoll = now;
    BattleSnapshot current;
    if (!ReadBattleSnapshot(current)) return;
    size_t visible = 0;
    for (const BattleUnitSnapshot& unit : current.units)
        if (unit.visibility == 2) ++visible;

    AcquireSRWLockExclusive(&gStatsLock);
    for (const BattleUnitSnapshot& unit : current.units) {
        if (unit.camp != 0 || unit.visibility != 2) continue;
        BattleUnitTotals& totals = gTotals[unit.slot];
        totals.slot = unit.slot;
        totals.label = BattleUnitLabel(unit);
    }
    if (!gActive && visible >= 2) {
        gActive = true;
        gStarted = now;
        gEmptySince = 0;
        GetObservedGuideScenario(gScenario);
    }
    if (gActive) {
        if (!visible) {
            if (!gEmptySince) gEmptySince = now;
            if (now - gEmptySince >= 5000) FinishUnlocked(now);
        } else {
            gEmptySince = 0;
        }
    }
    ReleaseSRWLockExclusive(&gStatsLock);
}

void ResetBattleStats() {
    AcquireSRWLockExclusive(&gStatsLock);
    ClearTotalsUnlocked();
    gLastPoll = 0;
    ReleaseSRWLockExclusive(&gStatsLock);
}

void ShutdownBattleStats() {
    RuntimeLayout layout;
    AcquireSRWLockExclusive(&gStatsLock);
    layout = gLayout;
    gLayout = RuntimeLayout();
    gHookAvailable = false;
    gSettlementAvailable.fill(false);
    gCriticalAvailable = false;
    gComboAvailable = false;
    ClearTotalsUnlocked();
    ReleaseSRWLockExclusive(&gStatsLock);

    bool allRestored = RestoreCodePatch(layout.criticalCommit);
    allRestored = RestoreCodePatch(layout.physicalResult) && allRestored;
    std::array<bool, kSettlementCallCount> restored = {true, true, true, true};
    for (size_t i = 0; i < layout.callSites.size(); ++i) {
        if (!layout.callSites[i] || !layout.installed[i]) continue;
        if (!memcmp(layout.callSites[i], layout.patched[i].data(), 5))
            restored[i] = WriteCode(layout.callSites[i], layout.original[i].data(), 5);
        else if (memcmp(layout.callSites[i], layout.original[i].data(), 5))
            restored[i] = false;
        allRestored = restored[i] && allRestored;
    }
    for (size_t i = 0; i < layout.thunks.size(); ++i)
        if (layout.thunks[i] && restored[i]) VirtualFree(layout.thunks[i], 0, MEM_RELEASE);
    if (allRestored) {
        gBattleStatsOriginalSetHp = 0;
        gBattleStatsOriginalPhysicalResult = 0;
        gBattleStatsCriticalOffset = 0;
    }
}

bool ReadBattleStatsSnapshot(BattleStatsSnapshot& snapshot) {
    snapshot = BattleStatsSnapshot();
    AcquireSRWLockShared(&gStatsLock);
    snapshot.hookAvailable = gHookAvailable;
    snapshot.physicalSettlementAvailable = gSettlementAvailable[SettlementPhysicalDamage];
    snapshot.strategyDamageAvailable = gSettlementAvailable[SettlementStrategyDamage];
    snapshot.recoveryAvailable = gSettlementAvailable[kRecoveryCapability];
    snapshot.criticalAvailable = gCriticalAvailable;
    snapshot.comboAvailable = gComboAvailable;
    snapshot.active = gActive;
    snapshot.units.reserve(gTotals.size());
    for (const auto& pair : gTotals) snapshot.units.push_back(pair.second);
    ReleaseSRWLockShared(&gStatsLock);
    std::sort(snapshot.units.begin(), snapshot.units.end(), [](const BattleUnitTotals& left,
        const BattleUnitTotals& right) { return left.slot < right.slot; });
    return snapshot.hookAvailable;
}

bool ResolveBattleStatsImageForTest(const BYTE* image, uintptr_t virtualBase,
    uintptr_t& hpSetter, std::vector<uintptr_t>& callSites) {
    hpSetter = 0;
    callSites.clear();
    ResolvedLayout layout;
    if (!ResolveLayout(image, virtualBase, layout)) return false;
    hpSetter = layout.hpSetter;
    callSites.assign(layout.callSites.begin(), layout.callSites.end());
    return true;
}

bool ResolveBattleStatsImageLayoutForTest(const BYTE* image, uintptr_t virtualBase,
    BattleStatsImageLayout& output) {
    output = BattleStatsImageLayout();
    ResolvedLayout layout;
    if (!ResolveLayout(image, virtualBase, layout)) return false;
    output.hpSetter = layout.hpSetter;
    output.unitBase = layout.unitBase;
    output.strategyBase = layout.strategyBase;
    output.strategyCount = layout.strategyCount;
    output.physicalResultCall = layout.physicalResultCall;
    output.criticalCommit = layout.criticalCommit;
    output.criticalOffset = layout.criticalOffset;
    output.comboOffset = layout.comboOffset;
    output.settlementCalls.assign(layout.callSites.begin(), layout.callSites.end());
    return true;
}

void InitializeBattleStatsModelForTest(uintptr_t unitBase,
    uintptr_t strategyBase, unsigned strategyCount) {
    ShutdownBattleStats();
    AcquireSRWLockExclusive(&gStatsLock);
    gLayout = RuntimeLayout();
    gLayout.unitBase = unitBase;
    gLayout.strategyBase = strategyBase;
    gLayout.strategyCount = strategyCount;
    gLayout.comboOffset = 0x608;
    gHookAvailable = true;
    gSettlementAvailable.fill(true);
    gCriticalAvailable = true;
    gComboAvailable = true;
    ClearTotalsUnlocked();
    ReleaseSRWLockExclusive(&gStatsLock);
}

void SetBattleStatsModelActiveForTest(bool active) {
    AcquireSRWLockExclusive(&gStatsLock);
    gActive = active;
    ReleaseSRWLockExclusive(&gStatsLock);
}

void RecordBattleStatsSettlementForTest(int kind, uintptr_t target,
    uintptr_t callerFrame, int oldHp, int finalHp) {
    RecordSettlement(kind, target, callerFrame, oldHp, finalHp);
}

void RecordBattleStatsPhysicalResultForTest(uintptr_t context, uint32_t comboOffset) {
    gLayout.comboOffset = comboOffset;
    RecordPhysicalResultStart(context);
}

void RecordBattleStatsCriticalForTest(uintptr_t context, uint32_t comboOffset) {
    gLayout.comboOffset = comboOffset;
    RecordCriticalCommit(context);
}
