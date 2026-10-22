#include "BattleStats.h"

#include "BattleRuntime.h"
#include "Guide.h"
#include "EngineProfile.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
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
    // Kept for layout reporting. No unique 6.6 block-result commit has
    // passed semantic validation yet, so both remain zero in production.
    uintptr_t attackBlockCommit = 0;
    uintptr_t spellBlockCommit = 0;
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
    uintptr_t attackBlockCommit = 0;
    uintptr_t spellBlockCommit = 0;
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
std::map<int, BattleUnitTotals> gCampaignTotals;
std::map<int, std::wstring> gLabels;
SRWLOCK gStatsLock = SRWLOCK_INIT;
class ScopedStatsLock {
public:
    explicit ScopedStatsLock(bool exclusive = true) : exclusive_(exclusive) {
        if (exclusive_) AcquireSRWLockExclusive(&gStatsLock);
        else AcquireSRWLockShared(&gStatsLock);
    }
    ~ScopedStatsLock() { Unlock(); }
    void Unlock() {
        if (!held_) return;
        held_ = false;
        if (exclusive_) ReleaseSRWLockExclusive(&gStatsLock);
        else ReleaseSRWLockShared(&gStatsLock);
    }
    ScopedStatsLock(const ScopedStatsLock&) = delete;
    ScopedStatsLock& operator=(const ScopedStatsLock&) = delete;
private:
    bool exclusive_, held_ = true;
};
class ScopedRecording {
public:
    explicit ScopedRecording(volatile LONG* flag) : flag_(flag) {}
    ~ScopedRecording() { InterlockedExchange(flag_, FALSE); }
private:
    volatile LONG* flag_;
};
bool gHookAvailable = false;
std::array<bool, kSettlementCapabilityCount> gSettlementAvailable = {};
bool gCriticalAvailable = false;
bool gComboAvailable = false;
bool gAttackBlockAvailable = false;
bool gSpellBlockAvailable = false;
bool gActive = false;
bool gLoading = false;
ULONGLONG gStarted = 0;
uint64_t gElapsed = 0;
ULONGLONG gLastPoll = 0;
std::wstring gScenario;
int gScenarioOrdinal = -1;
uint64_t gCampaignId = 0;
uint64_t gBattleInstance = 0;
uint32_t gKnownMetrics = 0;
bool gHistoryIncomplete = false;
std::wstring gStatus;
bool gRecordExported = false;
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
                ((CompiledEngineProfile() == EngineProfileId::Ccz66 &&
                  call[5] == 0x8B && call[6] == 0x4D && call[7] == 0xFC) ||
                 (CompiledEngineProfile() == EngineProfileId::Ccz65 &&
                  call[5] == 0xFF && call[6] == 0x75 && call[7] == 0xFC)) &&
                call[8] == 0xE8)
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
        if (CompiledEngineProfile() == EngineProfileId::Ccz65) {
            // 6.5 enumerates strategy IDs in a bounded local-byte loop.  Its
            // table accessor is the same as 6.6, but there is no 6.6 dispatch
            // table.  Prove the bound by linking that loop's local to this
            // accessor instead of borrowing an unrelated CMP immediate.
            for (size_t loop = 0; loop + 10 <= textSize; ++loop) {
                const BYTE* p = text + loop;
                if (p[0] != 0x8B || p[1] != 0x45 || p[3] != 0x3C || !p[4] ||
                    p[5] != 0x73) continue;
                const size_t end = loop + 7u + p[6];
                if (end > textSize || end <= loop + 10) continue;
                for (size_t at = loop + 7; at + 8 <= end; ++at) {
                    const BYTE* call = text + at;
                    if (call[0] == 0x8B && call[1] == 0x4D && call[2] == p[2] &&
                        call[3] == 0xE8 && CallTarget(image, call + 3) == image.Address(accessor))
                        counts.push_back(p[4]);
                }
            }
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

bool UnitPersonId(int slot, int& personId) {
    uint16_t value = 0xFFFF;
    if (slot < 0 || slot >= static_cast<int>(kUnitCount) || !gLayout.unitBase ||
        !ReadBytes(gLayout.unitBase + static_cast<uintptr_t>(slot) * kUnitStride,
            &value, sizeof(value)) || value == 0xFFFF) return false;
    personId = value;
    return true;
}

uint32_t AvailableMetricsUnlocked() {
    uint32_t mask = 0;
    if (gSettlementAvailable[SettlementPhysicalDamage] &&
        gSettlementAvailable[SettlementStrategyDamage])
        mask |= BattleStatsDamageDealt | BattleStatsDamageReceived | BattleStatsDefeatedEnemies;
    if (gSettlementAvailable[kRecoveryCapability]) mask |= BattleStatsRecovery;
    if (gCriticalAvailable) mask |= BattleStatsCritical;
    if (gComboAvailable) mask |= BattleStatsCombo;
    if (gAttackBlockAvailable) mask |= BattleStatsAttackBlock;
    if (gSpellBlockAvailable) mask |= BattleStatsSpellBlock;
    return mask;
}

uint64_t ElapsedUnlocked(ULONGLONG now) {
    const uint64_t delta = gActive && !gLoading && gStarted && now >= gStarted ? now - gStarted : 0;
    return delta > (std::numeric_limits<uint64_t>::max)() - gElapsed ?
        (std::numeric_limits<uint64_t>::max)() : gElapsed + delta;
}

void PauseClockUnlocked(ULONGLONG now) {
    gElapsed = ElapsedUnlocked(now);
    gStarted = 0;
}

BattleUnitTotals& TotalsForPersonUnlocked(std::map<int, BattleUnitTotals>& ledger,
    int personId, int slot) {
    BattleUnitTotals& totals = ledger[personId];
    totals.personId = personId;
    totals.slot = slot;
    const auto label = gLabels.find(personId);
    if (label != gLabels.end()) totals.label = label->second;
    return totals;
}

void AddMetricUnlocked(int slot, long long BattleUnitTotals::* metric,
    long long amount, uint32_t known) {
    int personId = -1;
    if (amount <= 0 || !UnitPersonId(slot, personId)) return;
    BattleUnitTotals* local = &TotalsForPersonUnlocked(gTotals, personId, slot);
    BattleUnitTotals* campaign = &TotalsForPersonUnlocked(gCampaignTotals, personId, slot);
    for (auto* totals : {local, campaign}) {
        long long& value = totals->*metric;
        value = amount > (std::numeric_limits<long long>::max)() - value ?
            (std::numeric_limits<long long>::max)() : value + amount;
    }
    gKnownMetrics |= known;
}

void AddDefeatUnlocked(int slot) {
    int personId = -1;
    if (!UnitPersonId(slot, personId)) return;
    BattleUnitTotals* local = &TotalsForPersonUnlocked(gTotals, personId, slot);
    BattleUnitTotals* campaign = &TotalsForPersonUnlocked(gCampaignTotals, personId, slot);
    for (auto* totals : {local, campaign}) {
        int& value = totals->defeatedEnemies;
        if (value < INT_MAX) ++value;
    }
    gKnownMetrics |= BattleStatsDefeatedEnemies;
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

void ExportCurrentRecordUnlocked() {
    if (gLoading || gRecordExported || gTotals.empty() || gRoot.empty()) return;
    const std::wstring privateRoot = PatchPrivatePath(gRoot, CompiledEngineProfile());
    const std::wstring folder = privateRoot + L"\\records";
    CreateDirectoryW((gRoot + L"\\patch").c_str(), nullptr);
    CreateDirectoryW(privateRoot.c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    wchar_t name[96] = {};
    swprintf_s(name, L"\\%04u%02u%02u-%02u%02u%02u-%03u.txt", time.wYear, time.wMonth,
        time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    std::wstring output = L"战斗记录\r\n剧本：" + gScenario + L"\r\n持续时间：" +
        std::to_wstring(gElapsed / 1000) + L" 秒\r\n\r\n";
    const uint32_t metrics = gKnownMetrics | AvailableMetricsUnlocked();
    for (const auto& pair : gTotals) {
        const auto& unit = pair.second;
        output += (unit.label.empty() ? L"我方角色 " + std::to_wstring(unit.personId) : unit.label) + L"\r\n";
        auto metric = [&](uint32_t bit, const wchar_t* label, long long value) {
            if (metrics & bit) output += std::wstring(L"  ") + label + L"：" + std::to_wstring(value) + L"\r\n";
        };
        metric(BattleStatsDamageDealt, L"造成伤害", unit.damageDealt);
        metric(BattleStatsDamageReceived, L"承受伤害", unit.damageReceived);
        metric(BattleStatsRecovery, L"回复量", unit.recoveryDone);
        metric(BattleStatsCritical, L"暴击次数", unit.criticalCount);
        metric(BattleStatsCombo, L"连击次数", unit.comboExtraHitCount);
        metric(BattleStatsAttackBlock, L"攻击格挡", unit.attackBlockCount);
        metric(BattleStatsSpellBlock, L"法术格挡", unit.spellBlockCount);
    }
    HANDLE file = CreateFileW((folder + name).c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(output.size() * sizeof(wchar_t));
    bool okay = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    okay = okay && WriteFile(file, output.data(), size, &written, nullptr) && written == size && FlushFileBuffers(file);
    okay = CloseHandle(file) != FALSE && okay;
    if (okay) gRecordExported = true;
}

void ClearTotalsUnlocked() {
    gTotals.clear();
    gCampaignTotals.clear();
    gLabels.clear();
    gPhysicalAction = PhysicalActionState();
    gNextActionSerial = 0;
    gActive = false;
    gLoading = false;
    gStarted = 0;
    gElapsed = 0;
    gScenario.clear();
    gScenarioOrdinal = -1;
    gCampaignId = 0;
    gBattleInstance = 0;
    gKnownMetrics = 0;
    gHistoryIncomplete = false;
    gStatus.clear();
    gRecordExported = false;
}

void RecordSettlement(int kind, uintptr_t target, uintptr_t callerFrame, int oldHp, int finalHp) {
    if (InterlockedCompareExchange(&gRecording, TRUE, FALSE) != FALSE) return;
    ScopedRecording recording(&gRecording);
    int targetSlot = -1;
    int targetCamp = -1;
    const bool validTarget = UnitSlotFromPointer(target, targetSlot) &&
        UnitCamp(targetSlot, targetCamp);
    if (validTarget) {
        ScopedStatsLock lock;
        if (gActive && !gLoading) {
            if (finalHp > oldHp && targetCamp == 0) {
                AddMetricUnlocked(targetSlot, &BattleUnitTotals::recoveryDone,
                    static_cast<long long>(finalHp) - oldHp, BattleStatsRecovery);
            } else if (oldHp > finalHp) {
                int actorSlot = -1;
                int actorCamp = -1;
                if (ResolveSettlementActor(kind, callerFrame, targetSlot, actorSlot) &&
                    UnitCamp(actorSlot, actorCamp)) {
                    const long long damage = static_cast<long long>(oldHp) - finalHp;
                    if (actorCamp == 0) {
                        AddMetricUnlocked(actorSlot, &BattleUnitTotals::damageDealt,
                            damage, BattleStatsDamageDealt);
                        if (targetCamp != 0 && oldHp > 0 && finalHp <= 0)
                            AddDefeatUnlocked(actorSlot);
                    }
                    if (targetCamp == 0) {
                        AddMetricUnlocked(targetSlot, &BattleUnitTotals::damageReceived,
                            damage, BattleStatsDamageReceived);
                    }
                }
            }
        }
        lock.Unlock();
    }
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
    ScopedRecording recording(&gEventRecording);
    int actorSlot = -1;
    int targetSlot = -1;
    unsigned ordinal = 0;
    const bool valid = ReadPhysicalEvent(context, gLayout.comboOffset, false,
        actorSlot, targetSlot, ordinal);
    if (valid) {
        ScopedStatsLock lock;
        if (gActive && !gLoading) {
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
                AddMetricUnlocked(actorSlot, &BattleUnitTotals::comboExtraHitCount,
                    1, BattleStatsCombo);
            }
        }
        lock.Unlock();
    }
}

void RecordCriticalCommit(uintptr_t context) {
    if (InterlockedCompareExchange(&gEventRecording, TRUE, FALSE) != FALSE) return;
    ScopedRecording recording(&gEventRecording);
    int actorSlot = -1;
    int targetSlot = -1;
    unsigned ordinal = 0;
    const bool valid = ReadPhysicalEvent(context, gLayout.comboOffset, true,
        actorSlot, targetSlot, ordinal);
    if (valid) {
        ScopedStatsLock lock;
        if (gActive && !gLoading && gCriticalAvailable && gPhysicalAction.context == context &&
            gPhysicalAction.actorSlot == actorSlot && gPhysicalAction.serial) {
            const unsigned key = (ordinal << 8) | static_cast<unsigned>(targetSlot);
            if (std::find(gPhysicalAction.criticalKeys.begin(),
                gPhysicalAction.criticalKeys.end(), key) ==
                gPhysicalAction.criticalKeys.end()) {
                gPhysicalAction.criticalKeys.push_back(key);
                AddMetricUnlocked(actorSlot, &BattleUnitTotals::criticalCount,
                    1, BattleStatsCritical);
            }
        }
        lock.Unlock();
    }
}

enum BlockKind : int {
    BlockAttack = 0,
    BlockSpell = 1
};

void RecordBlockResult(BlockKind kind, int attackerSlot, int targetSlot) {
    if (InterlockedCompareExchange(&gEventRecording, TRUE, FALSE) != FALSE) return;
    ScopedRecording recording(&gEventRecording);
    int attackerCamp = -1;
    int targetCamp = -1;
    const bool valid = attackerSlot >= 0 && attackerSlot < static_cast<int>(kUnitCount) &&
        targetSlot >= 0 && targetSlot < static_cast<int>(kUnitCount) &&
        UnitCamp(attackerSlot, attackerCamp) && UnitCamp(targetSlot, targetCamp);
    if (valid && attackerCamp >= 2 && targetCamp == 0) {
        ScopedStatsLock lock;
        if (gActive && !gLoading && ((kind == BlockAttack && gAttackBlockAvailable) ||
            (kind == BlockSpell && gSpellBlockAvailable))) {
            if (kind == BlockAttack)
                AddMetricUnlocked(targetSlot, &BattleUnitTotals::attackBlockCount,
                    1, BattleStatsAttackBlock);
            else AddMetricUnlocked(targetSlot, &BattleUnitTotals::spellBlockCount,
                1, BattleStatsSpellBlock);
        }
        lock.Unlock();
    }
}

} // namespace

extern "C" uintptr_t gBattleStatsOriginalSetHp = 0;
extern "C" uintptr_t gBattleStatsOriginalPhysicalResult = 0;
extern "C" uint32_t gBattleStatsCriticalOffset = 0;

extern "C" void __cdecl BattleStatsRecordHp(
    uintptr_t target, uintptr_t callerFrame, int oldHp, int finalHp, int kind) {
    // Never unwind a C++ allocation failure through the game's naked bridge.
    try { RecordSettlement(kind, target, callerFrame, oldHp, finalHp); }
    catch (...) {}
}

extern "C" void __cdecl BattleStatsRecordPhysicalResult(uintptr_t context) {
    try { RecordPhysicalResultStart(context); }
    catch (...) {}
}

extern "C" void __cdecl BattleStatsRecordCritical(uintptr_t context) {
    try { RecordCriticalCommit(context); }
    catch (...) {}
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

    ScopedStatsLock lock;
    gLayout = pending;
    gSettlementAvailable[SettlementPhysicalDamage] = pending.installed[0];
    gSettlementAvailable[SettlementStrategyDamage] = pending.installed[1];
    gSettlementAvailable[kRecoveryCapability] = recoveryComplete;
    gComboAvailable = pending.physicalResult.installed;
    gCriticalAvailable = pending.physicalResult.installed && pending.criticalCommit.installed;
    gHookAvailable = gCriticalAvailable || gComboAvailable ||
        std::any_of(gSettlementAvailable.begin(), gSettlementAvailable.end(),
            [](bool value) { return value; });
    lock.Unlock();
}

void PollBattleStats(ULONGLONG now) {
    if (gRoot.empty() || now - gLastPoll < 250) return;
    gLastPoll = now;
    BattleSnapshot current;
    if (!ReadBattleSnapshot(current)) return;
    ScopedStatsLock lock;
    if (gLoading) {
        lock.Unlock();
        return;
    }
    for (const BattleUnitSnapshot& unit : current.units) {
        int currentPerson = -1;
        if (unit.camp != 0 || unit.dataId < 0 || unit.dataId >= 0xFFFF ||
            !UnitPersonId(unit.slot, currentPerson) || currentPerson != unit.dataId) continue;
        const std::wstring label = unit.name.empty() ?
            L"我方角色 " + std::to_wstring(unit.dataId) : unit.name;
        gLabels[unit.dataId] = label;
        for (auto* ledger : {&gTotals, &gCampaignTotals}) {
            const auto found = ledger->find(unit.dataId);
            if (found != ledger->end()) {
                found->second.label = label;
                found->second.slot = unit.slot;
            } else if (gActive && unit.visibility == 2) {
                TotalsForPersonUnlocked(*ledger, unit.dataId, unit.slot).label = label;
            }
        }
    }
    lock.Unlock();
}

void SetBattleStatsLoading(bool loading) {
    ScopedStatsLock lock;
    if (loading != gLoading) {
        const ULONGLONG now = GetTickCount64();
        PauseClockUnlocked(now);
        gLoading = loading;
        if (!loading && gActive) gStarted = now;
        gPhysicalAction = PhysicalActionState();
        gNextActionSerial = 0;
    }
    lock.Unlock();
}

void NotifyBattleStatsScenarioInstalled(int ordinal, bool newCampaign,
    bool forceNewInstance, ULONGLONG now) {
    if (ordinal < 0 || ordinal >= 0xFFFF) return;
    ScopedStatsLock lock;
    // A load transaction owns both ledgers until its saved state is applied.
    if (gLoading) {
        lock.Unlock();
        return;
    }
    const int previousOrdinal = gScenarioOrdinal;
    const bool isBattle = (ordinal & 1) != 0;
    const bool startsInstance = isBattle && (newCampaign || forceNewInstance ||
        previousOrdinal != ordinal || !gBattleInstance);
    PauseClockUnlocked(now);
    if (newCampaign || startsInstance || (!isBattle && gActive)) ExportCurrentRecordUnlocked();
    if (newCampaign) ClearTotalsUnlocked();
    if (!gCampaignId) {
        FILETIME stamp = {};
        GetSystemTimeAsFileTime(&stamp);
        gCampaignId = (static_cast<uint64_t>(stamp.dwHighDateTime) << 32) | stamp.dwLowDateTime;
        gCampaignId ^= static_cast<uint64_t>(GetCurrentProcessId()) << 16;
        if (!gCampaignId) gCampaignId = 1;
    }
    if (startsInstance) {
        gTotals.clear();
        gElapsed = 0;
        gRecordExported = false;
        ++gBattleInstance;
        if (!gBattleInstance) gBattleInstance = 1;
        gPhysicalAction = PhysicalActionState();
        gNextActionSerial = 0;
        wchar_t scenario[32] = {};
        swprintf_s(scenario, L"S_%02d.eex", ordinal >> 1);
        gScenario = scenario;
    }
    gScenarioOrdinal = ordinal;
    gActive = isBattle;
    gStarted = isBattle ? now : 0;
    gKnownMetrics |= AvailableMetricsUnlocked();
    gLastPoll = 0;
    lock.Unlock();
}

void SetBattleStatsStatus(const std::wstring& status, bool historyIncomplete) {
    ScopedStatsLock lock;
    gStatus = status.substr(0, 4096);
    gHistoryIncomplete = gHistoryIncomplete || historyIncomplete;
    lock.Unlock();
}

void ResetBattleStats() {
    ScopedStatsLock lock;
    ClearTotalsUnlocked();
    gLastPoll = 0;
    lock.Unlock();
}

void ShutdownBattleStats() {
    RuntimeLayout layout;
    ScopedStatsLock lock;
    layout = gLayout;
    gLayout = RuntimeLayout();
    gHookAvailable = false;
    gSettlementAvailable.fill(false);
    gCriticalAvailable = false;
    gComboAvailable = false;
    gAttackBlockAvailable = false;
    gSpellBlockAvailable = false;
    ClearTotalsUnlocked();
    lock.Unlock();

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
    ScopedStatsLock lock(false);
    snapshot.hookAvailable = gHookAvailable;
    snapshot.physicalSettlementAvailable = gSettlementAvailable[SettlementPhysicalDamage];
    snapshot.strategyDamageAvailable = gSettlementAvailable[SettlementStrategyDamage];
    snapshot.recoveryAvailable = gSettlementAvailable[kRecoveryCapability];
    snapshot.criticalAvailable = gCriticalAvailable;
    snapshot.comboAvailable = gComboAvailable;
    snapshot.attackBlockAvailable = gAttackBlockAvailable;
    snapshot.spellBlockAvailable = gSpellBlockAvailable;
    snapshot.active = gActive;
    snapshot.loading = gLoading;
    snapshot.scenarioOrdinal = gScenarioOrdinal;
    snapshot.campaignId = gCampaignId;
    snapshot.battleInstance = gBattleInstance;
    snapshot.knownMetrics = gKnownMetrics | AvailableMetricsUnlocked();
    snapshot.historyIncomplete = gHistoryIncomplete;
    snapshot.elapsedMilliseconds = ElapsedUnlocked(GetTickCount64());
    snapshot.scenario = gScenario;
    snapshot.status = gStatus;
    snapshot.units.reserve(gTotals.size());
    for (const auto& pair : gTotals) snapshot.units.push_back(pair.second);
    snapshot.campaignUnits.reserve(gCampaignTotals.size());
    for (const auto& pair : gCampaignTotals) snapshot.campaignUnits.push_back(pair.second);
    lock.Unlock();
    std::sort(snapshot.units.begin(), snapshot.units.end(), [](const BattleUnitTotals& left,
        const BattleUnitTotals& right) { return left.personId < right.personId; });
    return true;
}

bool CaptureBattleStatsState(BattleStatsPersistedState& state, ULONGLONG now) {
    state = BattleStatsPersistedState();
    ScopedStatsLock lock(false);
    state.scenarioOrdinal = gScenarioOrdinal;
    state.campaignId = gCampaignId;
    state.battleInstance = gBattleInstance;
    state.knownMetrics = gKnownMetrics | AvailableMetricsUnlocked();
    state.historyIncomplete = gHistoryIncomplete;
    state.status = gStatus;
    state.active = gActive;
    state.elapsedMilliseconds = ElapsedUnlocked(now);
    state.scenario = gScenario;
    state.units.reserve(gTotals.size());
    for (const auto& pair : gTotals) state.units.push_back(pair.second);
    state.campaignUnits.reserve(gCampaignTotals.size());
    for (const auto& pair : gCampaignTotals) state.campaignUnits.push_back(pair.second);
    lock.Unlock();
    std::sort(state.units.begin(), state.units.end(), [](const BattleUnitTotals& left,
        const BattleUnitTotals& right) { return left.personId < right.personId; });
    return true;
}

bool RestoreBattleStatsState(const BattleStatsPersistedState& state, ULONGLONG now) {
    if ((state.schemaVersion != 1 && state.schemaVersion != 2) ||
        state.units.size() > kUnitCount || state.campaignUnits.size() > 0xFFFF ||
        state.scenario.size() > 1024 || state.status.size() > 4096 ||
        state.scenarioOrdinal < -1 || state.scenarioOrdinal >= 0xFFFF) return false;
    uintptr_t legacyUnitBase = 0;
    {
        ScopedStatsLock lock(false);
        legacyUnitBase = gLayout.unitBase;
    }
    std::map<int, BattleUnitTotals> restored, campaign;
    const bool legacy = state.schemaVersion == 1;
    auto readLedger = [&](const std::vector<BattleUnitTotals>& source,
        std::map<int, BattleUnitTotals>& target) {
        for (BattleUnitTotals unit : source) {
            if (unit.slot < -1 || unit.slot >= static_cast<int>(kUnitCount) ||
                unit.label.size() > 256 || unit.damageDealt < 0 || unit.damageReceived < 0 ||
                unit.recoveryDone < 0 || unit.criticalCount < 0 || unit.comboExtraHitCount < 0 ||
                unit.attackBlockCount < 0 || unit.spellBlockCount < 0 || unit.defeatedEnemies < 0)
                return false;
            if (legacy) {
                uint16_t personId = 0xFFFF;
                if (!legacyUnitBase || unit.slot < 0 ||
                    !ReadBytes(legacyUnitBase + static_cast<uintptr_t>(unit.slot) * kUnitStride,
                        &personId, sizeof(personId)) || personId == 0xFFFF) continue;
                unit.personId = personId;
            }
            if (unit.personId < 0 || unit.personId >= 0xFFFF || target.count(unit.personId))
                return false;
            target.emplace(unit.personId, std::move(unit));
        }
        return true;
    };
    if (!readLedger(state.units, restored) || (!legacy && !readLedger(state.campaignUnits, campaign))) {
        return false;
    }
    if (legacy) campaign = restored;
    std::map<int, std::wstring> labels;
    for (const auto& pair : campaign) labels[pair.first] = pair.second.label;
    for (const auto& pair : restored) labels[pair.first] = pair.second.label;
    std::wstring scenario = state.scenario;
    std::wstring status = legacy ? L"旧版统计已迁移；此前的全局历史记录不完整。" : state.status;
    // All allocations precede replacement. A failed allocation leaves both
    // live ledgers intact, and every lock releases during stack unwinding.
    ScopedStatsLock lock;
    gTotals = std::move(restored);
    gCampaignTotals = std::move(campaign);
    gLabels = std::move(labels);
    gPhysicalAction = PhysicalActionState();
    gNextActionSerial = 0;
    gActive = state.active;
    gElapsed = state.elapsedMilliseconds;
    gStarted = gActive && !gLoading ? now : 0;
    gScenario = std::move(scenario);
    gScenarioOrdinal = state.scenarioOrdinal;
    gCampaignId = state.campaignId;
    gBattleInstance = state.battleInstance;
    gKnownMetrics = state.knownMetrics;
    gHistoryIncomplete = state.historyIncomplete || legacy;
    gStatus = std::move(status);
    gRecordExported = false;
    if (legacy) gKnownMetrics |= AvailableMetricsUnlocked();
    gLastPoll = 0;
    lock.Unlock();
    return true;
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
    output.attackBlockCommit = layout.attackBlockCommit;
    output.spellBlockCommit = layout.spellBlockCommit;
    output.criticalOffset = layout.criticalOffset;
    output.comboOffset = layout.comboOffset;
    output.settlementCalls.assign(layout.callSites.begin(), layout.callSites.end());
    return true;
}

void InitializeBattleStatsModelForTest(uintptr_t unitBase,
    uintptr_t strategyBase, unsigned strategyCount) {
    ShutdownBattleStats();
    ScopedStatsLock lock;
    gLayout = RuntimeLayout();
    gLayout.unitBase = unitBase;
    gLayout.strategyBase = strategyBase;
    gLayout.strategyCount = strategyCount;
    gLayout.comboOffset = 0x608;
    gHookAvailable = true;
    gSettlementAvailable.fill(true);
    gCriticalAvailable = true;
    gComboAvailable = true;
    // The model receives an explicit confirmed result through its test
    // entry points. Production remains disabled until a unique 6.6 result
    // commit is semantically resolved.
    gAttackBlockAvailable = true;
    gSpellBlockAvailable = true;
    ClearTotalsUnlocked();
    lock.Unlock();
}

void SetBattleStatsModelActiveForTest(bool active) {
    ScopedStatsLock lock;
    gActive = active;
    lock.Unlock();
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

void RecordBattleStatsAttackBlockForTest(int attackerSlot, int targetSlot) {
    RecordBlockResult(BlockAttack, attackerSlot, targetSlot);
}

void RecordBattleStatsSpellBlockForTest(int attackerSlot, int targetSlot) {
    RecordBlockResult(BlockSpell, attackerSlot, targetSlot);
}
