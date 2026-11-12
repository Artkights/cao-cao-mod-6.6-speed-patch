#include "BattleStats.h"
#include "StatsDiagnostics.h"

#include "BattleRuntime.h"
#include "Guide.h"
#include "EngineProfile.h"
#include "ScenarioScript.h"
#ifndef XA66_TEST
#include "AnimationRuntime.h"
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
// AnimationRuntime owns the verified return-slot bridge in production.
// Test-only model builds intentionally keep the optional bridge absent.
bool ResolveNativeAnimationReturn(uintptr_t slot, uintptr_t observed, uintptr_t& native) {
#if defined(XA66_TEST)
    (void)slot; (void)observed; (void)native; return false;
#else
    return ::ResolveNativeAnimationReturn(slot, observed, native);
#endif
}

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

enum DodgeRole : unsigned { DodgeFinal = 0, DodgePhysicalImmune, DodgePhysicalValid,
    DodgeStrategyValid, DodgeStrategyImmune, DodgeStrategyPerson, DodgeStrategyForcedZero, DodgeRoleCount };
struct DodgeCall { uintptr_t site = 0, target = 0; DodgeRole role = DodgeFinal; };
struct DodgeLayout {
    uintptr_t base = 0, finalHit = 0, physicalCalcReturn = 0;
    std::vector<DodgeCall> calls;
    std::vector<std::pair<uintptr_t, int>> strategyCalcs;
    std::array<uintptr_t, 3> strategyDirect = {};
    std::array<uintptr_t, 2> strategyActions = {};
    bool available = false;
};

enum ReceivedRole : unsigned { ReceivedPhysical=0, ReceivedDamage, ReceivedMixed,
    ReceivedState, ReceivedSpecial, ReceivedAll, ReceivedRandom, ReceivedScatter, ReceivedRoleCount };
struct ReceivedCall {
    uintptr_t site=0,target=0;
    ReceivedRole role=ReceivedPhysical;
    bool entry=false;
    int contextLocal=0,indexLocal=0;
};
struct ReceivedLayout {
    uintptr_t base=0;
    std::vector<ReceivedCall> calls;
    std::array<uintptr_t,2> physicalActions={},strategyActions={};
    bool physical=false,strategy=false;
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
    DodgeLayout dodge;
    ReceivedLayout received;
};

struct CodePatch {
    BYTE* site = nullptr;
    size_t length = 0;
    std::array<BYTE, 16> original = {};
    std::array<BYTE, 16> patched = {};
    void* thunk = nullptr;
    bool installed = false;
    DWORD protection = 0;
    bool protectionKnown = false, protectionPending = false, cachePending = false;
    bool attempted = false, currentKnown = false;
    std::array<BYTE,16> current = {};
};

struct RuntimeLayout {
    uintptr_t hpSetter = 0;
    uintptr_t unitBase = 0;
    uintptr_t strategyBase = 0;
    unsigned strategyCount = 0;
    std::array<CodePatch,kSettlementCallCount> settlements;
    CodePatch physicalResult;
    CodePatch criticalCommit;
    DodgeLayout dodge;
    ReceivedLayout received;
    std::vector<CodePatch> dodgePatches;
    std::vector<CodePatch> receivedPatches;
    uintptr_t attackBlockCommit = 0;
    uintptr_t spellBlockCommit = 0;
    uint32_t criticalOffset = 0;
    uint32_t comboOffset = 0;
    bool recoveryPending = false;
};

struct PhysicalActionState {
    uintptr_t context = 0;
    unsigned long long serial = 0;
    uint64_t generation = 0;
    int actorSlot = -1;
    std::vector<unsigned> criticalKeys;
    std::vector<unsigned> comboOrdinals;
};

std::wstring gRoot;
RuntimeLayout gLayout;
std::map<int, BattleUnitTotals> gTotals;
std::map<int, BattleUnitTotals> gCampaignTotals;
std::map<int, BattleRosterEntry> gCampaignRoster;
std::set<int> gCurrentBattleParticipants;
bool gRosterHistoryIncomplete = false;
bool gRosterAvailable = false;
bool gDeploymentAvailable = false;
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
bool gPhysicalReceivedAvailable = false;
bool gStrategyReceivedAvailable = false;
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
volatile LONG gNativeHitActive = 0;
volatile LONG gStatsDetaching = TRUE;
#ifdef XA66_TEST
int gStatsFailForward=-1,gStatsFailRestore=-1,gStatsForwardWrites=0,gStatsRestoreWrites=0;
unsigned gStatsForwardFailure=0,gStatsRestoreFailure=0;
#endif
PhysicalActionState gPhysicalAction;
unsigned long long gNextActionSerial = 0;
uint64_t gDodgeGeneration = 0;
std::array<uintptr_t,DodgeRoleCount> gDodgeFallbackTargets = {};
std::vector<ReceivedCall> gReceivedFallbackCalls;

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
                 ((CompiledEngineProfile() == EngineProfileId::Ccz65 || CompiledEngineProfile() == EngineProfileId::Ccz64) &&
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
        if (CompiledEngineProfile() == EngineProfileId::Ccz65 || CompiledEngineProfile() == EngineProfileId::Ccz64) {
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

#include "BattleStatsReceivedLayout.inl"

bool ResolveDodgeLayout(const PeImage& image, DodgeLayout& output) {
    output = DodgeLayout();
    const bool v64 = CompiledEngineProfile() == EngineProfileId::Ccz64;
    const bool v66 = CompiledEngineProfile() == EngineProfileId::Ccz66;
    const uintptr_t delta = image.virtualBase - image.nt->OptionalHeader.ImageBase;
    auto address = [delta](uintptr_t value) { return value + delta; };
    auto call = [&](uintptr_t site, uintptr_t target, DodgeRole role, bool observe = true) {
        const BYTE* code = image.At(address(site), 5);
        if (!code || code[0] != 0xE8 || CallTarget(image, code) != address(target)) return false;
        if (observe) output.calls.push_back({address(site), address(target), role});
        return true;
    };
    auto bytes = [&](uintptr_t site, std::initializer_list<BYTE> expected) {
        const BYTE* code = image.At(address(site), expected.size());
        return code && !memcmp(code, expected.begin(), expected.size());
    };
    const uintptr_t final = v64 ? 0x43B4BA : 0x43B4B6;
    const bool extended66 = v66 && CallTarget(image,image.At(address(0x43B4CA),5)) == address(0x43B600);
    if (extended66 && (!bytes(0x43B600,{0x55,0x8B,0xEC,0x33,0xC0,0x50,0x51,0x83,0xEC,0x08}) ||
        !call(0x43B62D,0x4E4000,DodgeFinal,false) || !bytes(0x43B632,{0x85,0xC0,0x75,0x1D}) ||
        !bytes(0x43B4DA,{0x85,0xC0,0x74,0x56,0x83,0xF8,0x19}))) return false;
    // Verify the ABI and the native final-return path. Never observe an inner
    // RNG result: 6.4 applies an additional forced-result override afterwards.
    if (!(v64 ? bytes(final,{0x55,0x8B,0xEC,0x83,0xEC,0x08}) :
        bytes(final,{0x55,0x8B,0xEC,0x51,0x6A,0x01})) ||
        !call(v64 ? 0x405E70 : 0x405E6D,v66 ? 0x43BC5A : 0x43BC62,DodgeFinal,false) ||
        !call(0x452802,0x405DE1,DodgeFinal,false) ||
        !bytes(0x405DE1,{0x55,0x8B,0xEC,0x83,0xEC}) ||
        !bytes(0x425627,{0x55,0x8B,0xEC,0x51,0xFF,0x75,0x0C,0xFF,0x75,0x08}) ||
        !call(0x43BD1F,final,DodgeFinal) ||
        !call(v66 ? 0x43C08F : 0x43C084,final,DodgeFinal)) return false;
    output.strategyDirect = {address(v66 ? 0x42327A : 0x42327B),
        address(v66 ? 0x4235D7 : 0x4235D5),address(v66 ? 0x423ADC : 0x423ADF)};
    for (uintptr_t site : output.strategyDirect)
        if (!call(site-delta,final,DodgeFinal)) return false;
    // The fourth direct call belongs to recovery. Verify it, but leave it
    // unpatched so healing and friendly support cannot become dodge events.
    if (!call(v66 ? 0x424770 : 0x424778,final,DodgeFinal,false)) return false;
    const uintptr_t calc = v66 ? 0x43C01D : 0x43C022;
    const std::pair<uintptr_t,int> v65Calcs[] = {{0x4231A4,8},{0x4235FE,4},
        {0x42435F,12},{v64 ? 0x42448Au : 0x424485u,8},{0x424637,4}};
    const std::pair<uintptr_t,int> v66Calcs[] = {{0x423622,4},{0x42433E,12},
        {0x424467,8},{0x424636,4}};
    auto addCalc = [&](std::pair<uintptr_t,int> entry) {
        if (!call(entry.first,calc,DodgeFinal,false)) return false;
        output.strategyCalcs.push_back({address(entry.first+5),entry.second});
        return true;
    };
    if (v66) { for (const auto& entry : v66Calcs) if (!addCalc(entry)) return false; }
    else { for (const auto& entry : v65Calcs) if (!addCalc(entry)) return false; }
    output.strategyActions = {address(v66 ? 0x4179DF : 0x4179DD),
        address(v64 ? 0x405BC8 : 0x41C0FE)};
    for (uintptr_t site : output.strategyActions)
        if (!call(site-5-delta,0x425627,DodgeFinal,false)) return false;
    if (!call(0x43B63C,v66 ? 0x4E4060 : 0x47EF3D,DodgePhysicalValid) ||
        !call(0x43B79F,0x42044D,DodgeStrategyValid)) return false;
    if (!v64 && !call(v66 ? (extended66 ? 0x43B61E : 0x43B62D) : 0x43B625,0x43BA9D,DodgePhysicalImmune))
        return false;
    if (!v64 && !call(v66 ? 0x43B7E7 : 0x43B8E9,0x42F86A,DodgeStrategyImmune))
        return false;
    if (v64 && !call(0x43B8D5,0x406610,DodgeStrategyPerson)) return false;
    // A native strategy rule can reject a target after the general
    // applicability check. Observe its zero-rate commit rather than infer a
    // dodge from the final false return or repeat any of its native queries.
    if (!bytes(0x43B968,{0x88,0x45,0xF0,0xEB,0x17})) return false;
    output.calls.push_back({address(0x43B968),0,DodgeStrategyForcedZero});
    output.base = address(0x400000);
    output.finalHit = address(final);
    output.physicalCalcReturn = address(v64 ? 0x405E75 : 0x405E72);
    if (!call(v64 ? 0x43B4CE : 0x43B4CA,v66 ? (extended66 ? 0x43B600 : 0x43B617) : (v64 ? 0x43B60B : 0x43B600),DodgeFinal,false) ||
        !call(0x43B4D5,v66 ? 0x43B730 : 0x43B6F3,DodgeFinal,false) ||
        !bytes(v64 ? 0x4322A0 : (v66 && !extended66 ? 0x43B529 : 0x43B534),
            v64 ? std::initializer_list<BYTE>{0x89,0xEC,0x5D,0xC2,0x0C,0x00} :
                  std::initializer_list<BYTE>{0x8B,0xE5,0x5D,0xC2,0x0C,0x00})) return false;
    if (v64) {
        const BYTE* jump = image.At(address(0x43B506),5);
        int32_t displacement = 0;
        if (!jump || jump[0] != 0xE9) return false;
        memcpy(&displacement,jump+1,4);
        if (address(0x43B50B)+displacement != address(0x43225A)) return false;
    }
    if (!bytes(v66 ? 0x424454 : (v64 ? 0x424477 : 0x424472),
        v66 ? std::initializer_list<BYTE>{0xB0,0x36,0xEB,0x02,0xB0,0x2E,0x6A,0x00} :
              std::initializer_list<BYTE>{0xB0,0x26,0xEB,0x02,0xB0,0x21,0x6A,0x00})) return false;
    // An added final-hit caller is an unreviewed execution path. Disable this
    // capability as a whole until it has an explicit semantic classification.
    const BYTE* text = nullptr; size_t textSize = 0;
    if (!image.ExecutableRange(text,textSize)) return false;
    unsigned finalCalls = 0;
    for (size_t index=0;index+5<=textSize;++index)
        if (text[index] == 0xE8 && CallTarget(image,text+index) == output.finalHit) ++finalCalls;
    if (finalCalls != 6) return false;
    output.available = true;
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
        if (CompiledEngineProfile()==EngineProfileId::Ccz64 && i+47<=textSize) {
            // Xisui records the first victim's damage in a global total before
            // passing (HP - result[index]) to the same clamping setter.
            const BYTE before[]={0x8B,0x4D,0xE0,0xE8};
            const BYTE results[]={0x8B,0x4D,0xF8,0x8B,0x55,0xF4,0x8B,0x94,0x8A,0x84,0,0,0,0x85,0xC9,0x75,6,0x01,0x15};
            const BYTE subtract[]={0x2B,0xC2,0x50,0x8B,0x4D,0xE0,0xE8};
            uint32_t total=0; memcpy(&total,p+27,4);
            if (!memcmp(p,before,sizeof(before)) && !memcmp(p+8,results,sizeof(results)) &&
                !memcmp(p+31,subtract,sizeof(subtract)) && image.InImage(total,4) &&
                ValidateHpSetter(image,CallTarget(image,p+37)) &&
                p[42]==0x8B && p[43]==0x4D && p[44]==0xE0 && p[45]==0xE8) {
                const uintptr_t target=CallTarget(image,p+37);
                damageSites.push_back(image.Address(p+37));
                if (!hpSetter) hpSetter=target;
                else if(hpSetter!=target) return false;
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
    ResolveDodgeLayout(image, result.dodge);
    ResolveReceivedLayout(image,result.received);
    return std::any_of(result.callSites.begin(), result.callSites.end(),
        [](uintptr_t site) { return site != 0; }) || result.physicalEvents || result.dodge.available;
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

bool PreparePatchMemory(CodePatch& patch) {
    MEMORY_BASIC_INFORMATION memory={};
    if (!patch.site || !patch.length || patch.length>patch.current.size() ||
        !VirtualQuery(patch.site,&memory,sizeof(memory)) || memory.State!=MEM_COMMIT ||
        !ReadBytes(reinterpret_cast<uintptr_t>(patch.site),patch.current.data(),patch.length)) return false;
    patch.protection=memory.Protect;
    patch.protectionKnown=patch.currentKnown=true;
    return true;
}

bool WriteCode(CodePatch& patch, const BYTE* bytes, bool restoring=false) {
    unsigned failure=0;
#ifdef XA66_TEST
    int& writes=restoring ? gStatsRestoreWrites : gStatsForwardWrites;
    if (writes++==(restoring ? gStatsFailRestore : gStatsFailForward))
        failure=restoring ? gStatsRestoreFailure : gStatsForwardFailure;
#else
    (void)restoring;
#endif
    DWORD protection=0;
    if (!patch.protectionKnown || failure==1 ||
        !VirtualProtect(patch.site,patch.length,PAGE_EXECUTE_READWRITE,&protection)) return false;
    patch.attempted=true;
    patch.protectionPending=true;
    bool copied=false;
    __try { memcpy(patch.site,bytes,patch.length);copied=true; }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
    DWORD ignored=0;
    const bool protectedAgain=failure!=2 &&
        VirtualProtect(patch.site,patch.length,patch.protection,&ignored)!=FALSE;
    patch.protectionPending=!protectedAgain;
    const bool flushed=failure!=3 &&
        FlushInstructionCache(GetCurrentProcess(),patch.site,patch.length)!=FALSE;
    patch.cachePending=!flushed;
    patch.currentKnown=ReadBytes(reinterpret_cast<uintptr_t>(patch.site),patch.current.data(),patch.length);
    patch.installed=patch.currentKnown && !memcmp(patch.current.data(),patch.patched.data(),patch.length);
    return copied && protectedAgain && flushed && patch.currentKnown &&
        !memcmp(patch.current.data(),bytes,patch.length);
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
            if (!FlushInstructionCache(GetCurrentProcess(),thunk,7)) {
                VirtualFree(thunk,0,MEM_RELEASE);
                return nullptr;
            }
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
    if (!PreparePatchMemory(patch)) return false;
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
    if (!PreparePatchMemory(patch)) return false;
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
        !WriteCode(patch,patch.patched.data())) return false;
    patch.installed = true;
    return true;
}

bool RestoreCodePatch(CodePatch& patch) {
    if (!patch.site) return true;
    bool restored = true;
    if (patch.attempted || patch.protectionPending || patch.cachePending) {
        std::array<BYTE,16> actual={};
        if (!ReadBytes(reinterpret_cast<uintptr_t>(patch.site),actual.data(),patch.length) ||
            (memcmp(actual.data(),patch.original.data(),patch.length) &&
                (!patch.currentKnown || memcmp(actual.data(),patch.current.data(),patch.length)))) return false;
        restored=WriteCode(patch,patch.original.data(),true);
        if (restored) {
            patch.installed=patch.attempted=false;
        }
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
    if (gPhysicalReceivedAvailable) mask |= BattleStatsPhysicalReceived;
    if (gStrategyReceivedAvailable) mask |= BattleStatsStrategyReceived;
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

BattleRosterEntry* FindRosterUnlocked(int personId) {
    const auto found = gCampaignRoster.find(personId);
    return found == gCampaignRoster.end() ? nullptr : &found->second;
}

void SetObservedLabelUnlocked(int personId, const std::wstring& label) {
    // Prepare every allocating string operation before committing labels in
    // the roster and either ledger, so a failed allocation cannot split them.
    std::wstring rosterLabel = label, currentLabel = label, campaignLabel = label;
    auto known = gLabels.find(personId);
    if (known == gLabels.end()) gLabels.emplace(personId, label);
    else { std::wstring knownLabel = label; known->second.swap(knownLabel); }
    if (BattleRosterEntry* entry = FindRosterUnlocked(personId)) entry->label.swap(rosterLabel);
    const auto current = gTotals.find(personId);
    if (current != gTotals.end()) current->second.label.swap(currentLabel);
    const auto campaign = gCampaignTotals.find(personId);
    if (campaign != gCampaignTotals.end()) campaign->second.label.swap(campaignLabel);
}

void TraceLedgerUnlocked(const wchar_t* stage, const wchar_t* reason) noexcept;

bool IsCurrentParticipantUnlocked(int personId) {
    return gCurrentBattleParticipants.find(personId) != gCurrentBattleParticipants.end();
}

bool AddCurrentParticipantUnlocked(int personId, int slot) {
    auto row = gTotals.find(personId);
    bool inserted = false;
    if (row == gTotals.end()) {
        BattleUnitTotals total;
        total.personId = personId;
        total.slot = slot;
        const auto label = gLabels.find(personId);
        if (label != gLabels.end()) total.label = label->second;
        row = gTotals.emplace(personId, std::move(total)).first;
        inserted = true;
    }
    try { gCurrentBattleParticipants.insert(personId); }
    catch (...) { if (inserted) gTotals.erase(row); throw; }
    return true;
}

bool ConfirmEventParticipantUnlocked(int personId, int slot) {
    if (!(gDeploymentAvailable || (BattleRuntimeCapabilities() & BattleCapabilityDeployment)) ||
        slot < 0 || slot >= 250) return false;
    int actualPerson = -1, camp = -1;
    BYTE visibility = 0, coordinates[2] = {};
    const uintptr_t unit = gLayout.unitBase + static_cast<uintptr_t>(slot) * kUnitStride;
    return UnitPersonId(slot, actualPerson) && actualPerson == personId &&
        UnitCamp(slot, camp) && camp == 0 && ReadBytes(unit + 0x0C, &visibility, 1) &&
        visibility == 2 && ReadBytes(unit + 6, coordinates, sizeof(coordinates)) &&
        coordinates[0] != 255 && coordinates[1] != 255 && AddCurrentParticipantUnlocked(personId, slot);
}

void MarkRosterCampUnlocked(int personId, int camp) {
    if (!gRosterAvailable) return;
    BattleRosterEntry* entry = FindRosterUnlocked(personId);
    if (!entry || camp < 0) return;
    const bool changed = entry->lastCamp != camp || (camp != 0 && entry->tracking);
    entry->lastCamp = camp;
    if (camp != 0 && entry->tracking) {
        entry->changedFaction = true;
        entry->tracking = false;
    }
    if (changed) TraceLedgerUnlocked(L"roster-camp", L"confirmed-unit-camp");
}

void ObserveSlotCampUnlocked(int slot) {
    int personId = -1, camp = -1;
    if (UnitPersonId(slot, personId) && UnitCamp(slot, camp))
        MarkRosterCampUnlocked(personId, camp);
}

bool CampaignTrackableUnlocked(int personId, int slot) {
    if (!gRosterAvailable) return false;
    BattleRosterEntry* entry = FindRosterUnlocked(personId);
    if (!entry) return false;
    if (!entry->joined || !entry->tracking || entry->changedFaction) return false;
    int camp = -1;
    if (!UnitCamp(slot, camp) || camp != 0) {
        if (camp >= 0) MarkRosterCampUnlocked(personId, camp);
        return false;
    }
    return true;
}

bool CurrentTrackableUnlocked(int personId, int slot) {
    int camp = -1;
    if (!gActive || !UnitCamp(slot, camp) || camp != 0) return false;
    const BattleRosterEntry* roster = FindRosterUnlocked(personId);
    if (roster && (!roster->tracking || roster->changedFaction)) return false;
    return IsCurrentParticipantUnlocked(personId) || ConfirmEventParticipantUnlocked(personId, slot);
}

void AddTotalsValueUnlocked(BattleUnitTotals& totals,
    long long BattleUnitTotals::* metric, long long amount) {
    long long& value = totals.*metric;
    value = amount > (std::numeric_limits<long long>::max)() - value ?
        (std::numeric_limits<long long>::max)() : value + amount;
}

void AddMetricUnlocked(int slot, long long BattleUnitTotals::* metric,
    long long amount, uint32_t known) {
    if (InterlockedCompareExchange(&gStatsDetaching,FALSE,FALSE)!=FALSE) return;
    int personId = -1;
    if (amount <= 0 || !UnitPersonId(slot, personId)) return;
    const bool local = CurrentTrackableUnlocked(personId, slot);
    const bool campaign = CampaignTrackableUnlocked(personId, slot);
    if (local) AddTotalsValueUnlocked(TotalsForPersonUnlocked(gTotals, personId, slot), metric, amount);
    if (campaign) AddTotalsValueUnlocked(TotalsForPersonUnlocked(gCampaignTotals, personId, slot), metric, amount);
    if (local || campaign) gKnownMetrics |= known;
}

void AddDefeatUnlocked(int slot) {
    if (InterlockedCompareExchange(&gStatsDetaching,FALSE,FALSE)!=FALSE) return;
    int personId = -1;
    if (!UnitPersonId(slot, personId)) return;
    const bool local = CurrentTrackableUnlocked(personId, slot);
    const bool campaign = CampaignTrackableUnlocked(personId, slot);
    if (local) {
        int& value = TotalsForPersonUnlocked(gTotals, personId, slot).defeatedEnemies;
        if (value < INT_MAX) ++value;
    }
    if (campaign) {
        int& value = TotalsForPersonUnlocked(gCampaignTotals, personId, slot).defeatedEnemies;
        if (value < INT_MAX) ++value;
    }
    if (local || campaign) gKnownMetrics |= BattleStatsDefeatedEnemies;
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

std::wstring CsvRecordField(const std::wstring& value) {
    if (value.find_first_of(L",\"\r\n") == std::wstring::npos) return value;
    std::wstring escaped = L"\"";
    for (const wchar_t character : value) {
        if (character == L'\"') escaped += L'\"';
        escaped += character;
    }
    escaped += L'\"';
    return escaped;
}

bool WriteUtf16Record(const std::wstring& path, const std::wstring& output) {
    if (output.size() > MAXDWORD / sizeof(wchar_t)) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(output.size() * sizeof(wchar_t));
    bool okay = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    okay = okay && WriteFile(file, output.data(), size, &written, nullptr) && written == size && FlushFileBuffers(file);
    okay = CloseHandle(file) != FALSE && okay;
    if (!okay) DeleteFileW(path.c_str());
    return okay;
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
    swprintf_s(name, L"\\%04u%02u%02u-%02u%02u%02u-%03u", time.wYear, time.wMonth,
        time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    std::wstring output = L"战斗记录\r\n剧本：" + gScenario + L"\r\n持续时间：" +
        std::to_wstring(gElapsed / 1000) + L" 秒\r\n历史不完整：" +
        (gHistoryIncomplete ? L"是" : L"否") + L"\r\n\r\n";
    std::wstring csv = L"剧本,战役编号,战场实例,人物编号,单位,造成伤害,承受伤害,回复量,暴击次数,连击次数,攻击闪避,策略闪避,击退敌军,物理受击,策略受击,历史不完整\r\n";
    const uint32_t metrics = gKnownMetrics | AvailableMetricsUnlocked();
    for (const auto& pair : gTotals) {
        const auto& unit = pair.second;
        const std::wstring label = unit.label.empty() ? L"我方角色 " + std::to_wstring(unit.personId) : unit.label;
        output += label + L"\r\n";
        csv += CsvRecordField(gScenario) + L"," + std::to_wstring(gCampaignId) + L"," +
            std::to_wstring(gBattleInstance) + L"," + std::to_wstring(unit.personId) + L"," + CsvRecordField(label);
        auto metric = [&](uint32_t bit, const wchar_t* heading, long long value, bool available = true) {
            const bool known = available && (metrics & bit) != 0;
            const std::wstring text = known ? std::to_wstring(value) : L"--";
            if (known || bit == BattleStatsPhysicalReceived || bit == BattleStatsStrategyReceived)
                output += std::wstring(L"  ") + heading + L"：" + text + L"\r\n";
            csv += L"," + text;
        };
        metric(BattleStatsDamageDealt, L"造成伤害", unit.damageDealt);
        metric(BattleStatsDamageReceived, L"承受伤害", unit.damageReceived);
        metric(BattleStatsRecovery, L"回复量", unit.recoveryDone);
        metric(BattleStatsCritical, L"暴击次数", unit.criticalCount);
        metric(BattleStatsCombo, L"连击次数", unit.comboExtraHitCount);
        metric(BattleStatsAttackBlock, L"攻击闪避", unit.attackBlockCount);
        metric(BattleStatsSpellBlock, L"策略闪避", unit.spellBlockCount);
        metric(BattleStatsDefeatedEnemies, L"击退敌军", unit.defeatedEnemies);
        metric(BattleStatsPhysicalReceived, L"物理受击", unit.physicalReceivedCount, gPhysicalReceivedAvailable);
        metric(BattleStatsStrategyReceived, L"策略受击", unit.strategyReceivedCount, gStrategyReceivedAvailable);
        csv += gHistoryIncomplete ? L",是\r\n" : L",否\r\n";
    }
    const std::wstring textPath = folder + name + L".txt";
    if (!WriteUtf16Record(textPath, output)) return;
    if (!WriteUtf16Record(folder + name + L".csv", csv)) {
        DeleteFileW(textPath.c_str());
        return;
    }
    gRecordExported = true;
}

bool gAwaitingSceneReady = false;

void TraceLedgerUnlocked(const wchar_t* stage, const wchar_t* reason = L"") noexcept {
    try {
        BattleStatsPersistedState state;
        state.scenarioOrdinal = gScenarioOrdinal;
        state.campaignId = gCampaignId;
        state.battleInstance = gBattleInstance;
        state.knownMetrics = gKnownMetrics;
        state.historyIncomplete = gHistoryIncomplete;
        state.rosterHistoryIncomplete = gRosterHistoryIncomplete;
        state.active = gActive;
        state.elapsedMilliseconds = ElapsedUnlocked(GetTickCount64());
        state.scenario = gScenario;
        for (const auto& pair : gTotals) state.units.push_back(pair.second);
        for (const auto& pair : gCampaignTotals) state.campaignUnits.push_back(pair.second);
        for (const auto& pair : gCampaignRoster) state.campaignRoster.push_back(pair.second);
        state.currentBattleParticipants.assign(gCurrentBattleParticipants.begin(), gCurrentBattleParticipants.end());
        RecordStatsDiagnostic(0, stage, L"", &state, reason);
    } catch (...) {}
}

void ClearTotalsUnlocked(const wchar_t* reason = L"model-initialization") {
    TraceLedgerUnlocked(L"reset_before", reason);
    gTotals.clear();
    gCampaignTotals.clear();
    gCampaignRoster.clear();
    gCurrentBattleParticipants.clear();
    gRosterHistoryIncomplete = false;
    gLabels.clear();
    ++gDodgeGeneration;
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
    gAwaitingSceneReady = false;
    TraceLedgerUnlocked(L"reset_after", reason);
}

void RecordSettlement(int kind, uintptr_t target, uintptr_t callerFrame, int oldHp, int finalHp) {
    if (InterlockedCompareExchange(&gStatsDetaching,FALSE,FALSE)!=FALSE) return;
    if (InterlockedCompareExchange(&gRecording, TRUE, FALSE) != FALSE) return;
    ScopedRecording recording(&gRecording);
    int targetSlot = -1;
    int targetCamp = -1;
    const bool validTarget = UnitSlotFromPointer(target, targetSlot) &&
        UnitCamp(targetSlot, targetCamp);
    if (validTarget) {
        ScopedStatsLock lock;
        if (gActive && !gLoading) {
            ObserveSlotCampUnlocked(targetSlot);
            if (finalHp > oldHp && targetCamp == 0) {
                AddMetricUnlocked(targetSlot, &BattleUnitTotals::recoveryDone,
                    static_cast<long long>(finalHp) - oldHp, BattleStatsRecovery);
            } else if (oldHp > finalHp) {
                int actorSlot = -1;
                int actorCamp = -1;
                if (ResolveSettlementActor(kind, callerFrame, targetSlot, actorSlot) &&
                    UnitCamp(actorSlot, actorCamp)) {
                    ObserveSlotCampUnlocked(actorSlot);
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
    return UnitCamp(actorSlot, camp);
}

void RecordPhysicalResultStart(uintptr_t context) {
    if (InterlockedCompareExchange(&gStatsDetaching,FALSE,FALSE)!=FALSE) return;
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
            ObserveSlotCampUnlocked(actorSlot);
            if (!ordinal) {
                gPhysicalAction = PhysicalActionState();
                gPhysicalAction.context = context;
                gPhysicalAction.serial = ++gNextActionSerial;
                gPhysicalAction.generation = gDodgeGeneration;
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
    if (InterlockedCompareExchange(&gStatsDetaching,FALSE,FALSE)!=FALSE) return;
    if (InterlockedCompareExchange(&gEventRecording, TRUE, FALSE) != FALSE) return;
    ScopedRecording recording(&gEventRecording);
    int actorSlot = -1;
    int targetSlot = -1;
    unsigned ordinal = 0;
    const bool valid = ReadPhysicalEvent(context, gLayout.comboOffset, true,
        actorSlot, targetSlot, ordinal);
    if (valid) {
        ScopedStatsLock lock;
        if (!gLoading) {
            ObserveSlotCampUnlocked(actorSlot);
            ObserveSlotCampUnlocked(targetSlot);
        }
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
    if (valid) {
        ScopedStatsLock lock;
        if (!gLoading) {
            ObserveSlotCampUnlocked(attackerSlot);
            ObserveSlotCampUnlocked(targetSlot);
        }
        if (attackerCamp >= 2 && targetCamp == 0 && gActive && !gLoading &&
            ((kind == BlockAttack && gAttackBlockAvailable) ||
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


// Layout of PUSHFD/PUSHAD at the CALL bridge. The native function receives
// its original ECX and stack arguments exactly once. Its complete register
// and EFLAGS result is copied back before returning to the game.
struct NativeHitSnapshot {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax, flags;
    uintptr_t returnAddress;
    uint32_t args[4];
};
static_assert(offsetof(NativeHitSnapshot, args) == 40, "x86 snapshot ABI");
struct HitObservation {
    HitObservation* previous = nullptr;
    uintptr_t attacker = 0, target = 0, nativeCallerFrame = 0;
    BYTE strategyId = 0;
    int actorSlot = -1, targetSlot = -1, personId = -1;
    uint64_t generation = 0;
    bool strategy = false, eligible = false, excluded = false, applicabilitySeen = false;
};
__declspec(thread) HitObservation* gHitObservation = nullptr;
extern "C" void __cdecl BattleStatsInvokeNative(NativeHitSnapshot*, uintptr_t, unsigned);

bool FrameWord(uintptr_t frame, int offset, uintptr_t& value) {
    return frame && !(frame & 3) && ReadBytes(frame + offset, &value, sizeof(value));
}
#include "BattleStatsReceivedObservers.inl"

bool ContextMatches(uintptr_t context, const NativeHitSnapshot& snapshot, bool strategy,
    bool randomSubstrategy) {
    uintptr_t actor = 0;
    BYTE actorSlot = 0xFF, targetSlot = 0xFF, strategyId = 0xFF;
    int actualActor = -1, actualTarget = -1;
    if (!context || !UnitSlotFromPointer(snapshot.ecx, actualActor) ||
        !UnitSlotFromPointer(snapshot.args[0], actualTarget) ||
        !ReadBytes(context + (strategy ? 8 : 12), &actor, sizeof(actor)) || actor != snapshot.ecx ||
        !ReadBytes(context + (strategy ? 1 : 0), &actorSlot, 1) || actorSlot != actualActor ||
        !ReadBytes(context + (strategy ? 12 : 1), &targetSlot, 1) || targetSlot != actualTarget)
        return false;
    if (!strategy) return snapshot.args[1] == 1;
    if (snapshot.args[1] != 2 || !ReadBytes(context, &strategyId, 1)) return false;
    const bool v66 = CompiledEngineProfile() == EngineProfileId::Ccz66;
    const BYTE actualStrategy = static_cast<BYTE>(snapshot.args[2]);
    return actualStrategy == strategyId || (randomSubstrategy &&
        (actualStrategy == (v66 ? 0x2E : 0x21) || actualStrategy == (v66 ? 0x36 : 0x26)));
}

bool IsRealHitContext(const NativeHitSnapshot& snapshot, bool& strategy) {
    const DodgeLayout& layout = gLayout.dodge;
    if (!layout.available) return false;
    const uintptr_t base = layout.base;
    const auto at = [base](uintptr_t preferred) { return base + preferred - 0x400000; };
    uintptr_t frame = snapshot.ebp, caller = 0, parent = 0, context = 0;
    const uintptr_t site = snapshot.returnAddress - 5;
    strategy = site != at(0x43BD1F);
    if (!strategy) {
        return snapshot.args[1] == 1 && FrameWord(frame,4,caller) && caller == layout.physicalCalcReturn &&
            FrameWord(frame,0,parent) && parent > frame && parent-frame < 0x10000 &&
            FrameWord(parent,4,caller) && caller == at(0x452807) &&
            FrameWord(parent,-12,context) && ContextMatches(context,snapshot,false,false);
    }
    bool random = false;
    const bool calc = site == at(CompiledEngineProfile() == EngineProfileId::Ccz66 ?
        0x43C08F : 0x43C084);
    if (calc) {
        if (!FrameWord(frame,4,caller) || !FrameWord(frame,0,parent) ||
            parent <= frame || parent-frame >= 0x10000) return false;
        const auto found = std::find_if(layout.strategyCalcs.begin(),layout.strategyCalcs.end(),
            [caller](const std::pair<uintptr_t,int>& entry) { return entry.first == caller; });
        if (found == layout.strategyCalcs.end() || !FrameWord(parent,-found->second,context)) return false;
        const uintptr_t randomReturn = at(CompiledEngineProfile() == EngineProfileId::Ccz66 ?
            0x42446C : (CompiledEngineProfile() == EngineProfileId::Ccz64 ? 0x42448F : 0x42448A));
        random = caller == randomReturn;
        frame = parent;
    } else {
        const auto found = std::find(layout.strategyDirect.begin(),layout.strategyDirect.end(),site);
        if (found == layout.strategyDirect.end()) return false;
        const int local = found == layout.strategyDirect.begin() ? 8 : 4;
        if (!FrameWord(frame,-local,context)) return false;
    }
    if (!ContextMatches(context,snapshot,true,random)) return false;
    // Start at the game's saved EBP, never at one of our C++ frames. Require
    // the same context to belong to an actual native execution entry. An AI
    // forecast re-entered from a message pump cannot satisfy the immediate
    // calculation caller and this context identity together.
    for (unsigned depth = 0; depth < 12; ++depth) {
        if (!FrameWord(frame,4,caller)) return false;
        // The animation observer owns an actual native entry's return slot
        // while its scope is live. Recover only that exact TLS-owned slot;
        // the original native caller and context must still pass below.
        uintptr_t nativeCaller = 0;
        if (ResolveNativeAnimationReturn(frame + sizeof(uintptr_t),caller,nativeCaller))
            caller = nativeCaller;
        if (std::find(layout.strategyActions.begin(),layout.strategyActions.end(),caller) !=
            layout.strategyActions.end()) {
            uintptr_t actionContext = 0;
            return FrameWord(frame,-4,actionContext) && actionContext == context;
        }
        if (!FrameWord(frame,0,parent) || parent <= frame || parent-frame >= 0x10000) return false;
        frame = parent;
    }
    return false;
}

void BeginHitObservation(NativeHitSnapshot& snapshot, HitObservation& observation) {
    observation.previous = gHitObservation;
    observation.attacker = snapshot.ecx;
    observation.target = snapshot.args[0];
    observation.nativeCallerFrame = snapshot.ebp;
    observation.strategyId = static_cast<BYTE>(snapshot.args[2]);
    int actorCamp = -1, targetCamp = -1;
    observation.eligible = IsRealHitContext(snapshot,observation.strategy) &&
        UnitSlotFromPointer(observation.attacker,observation.actorSlot) &&
        UnitSlotFromPointer(observation.target,observation.targetSlot) &&
        UnitCamp(observation.actorSlot,actorCamp) && actorCamp >= 2 &&
        UnitCamp(observation.targetSlot,targetCamp) && targetCamp == 0 &&
        UnitPersonId(observation.targetSlot,observation.personId);
    {
        ScopedStatsLock lock;
        if (!gLoading) {
            ObserveSlotCampUnlocked(observation.actorSlot);
            ObserveSlotCampUnlocked(observation.targetSlot);
        }
        observation.generation = gDodgeGeneration;
        observation.eligible = observation.eligible && gActive && !gLoading &&
            (observation.strategy ? gSpellBlockAvailable : gAttackBlockAvailable);
    }
    gHitObservation = &observation;
}
void CommitHitObservation(const NativeHitSnapshot& snapshot, const HitObservation& observation) {
    if (snapshot.eax != 0 || !observation.eligible || observation.excluded ||
        !observation.applicabilitySeen) return;
    try {
        ScopedStatsLock lock;
        int personId = -1, actorCamp = -1, targetCamp = -1;
        if (!gActive || gLoading || observation.generation != gDodgeGeneration ||
            !UnitPersonId(observation.targetSlot,personId) || personId != observation.personId ||
            !UnitCamp(observation.actorSlot,actorCamp) || actorCamp < 2 ||
            !UnitCamp(observation.targetSlot,targetCamp) || targetCamp != 0) return;
        AddMetricUnlocked(observation.targetSlot, observation.strategy ?
            &BattleUnitTotals::spellBlockCount : &BattleUnitTotals::attackBlockCount,
            1, observation.strategy ? BattleStatsSpellBlock : BattleStatsAttackBlock);
    } catch (...) {} // Accounting must never unwind through a game CALL.
}
void ObserveHitGate(DodgeRole role, const NativeHitSnapshot& snapshot,
    uintptr_t originalEcx, const uint32_t* originalArgs) {
    HitObservation* observation = gHitObservation;
    if (!observation || !observation->eligible) return;
    // Only gates directly inside this native final judgment are evidence.
    // A forecast re-entering the rate function while a custom callback pumps
    // messages must not contaminate an outer real judgment's exclusions.
    uintptr_t finalFrame = 0, callerFrame = 0, rateReturn = 0;
    const uintptr_t expectedReturn = gLayout.dodge.base +
        (observation->strategy ? 0x3B4DA :
            (CompiledEngineProfile() == EngineProfileId::Ccz64 ? 0x3B4D3 : 0x3B4CF));
    if (!FrameWord(snapshot.ebp,0,finalFrame) || !FrameWord(finalFrame,0,callerFrame) ||
        callerFrame != observation->nativeCallerFrame ||
        !FrameWord(snapshot.ebp,4,rateReturn) || rateReturn != expectedReturn) return;
    const bool result = snapshot.eax != 0;
    switch (role) {
    case DodgePhysicalImmune:
        if (!observation->strategy && originalArgs[0] == observation->target && !result)
            observation->excluded = true;
        break;
    case DodgePhysicalValid:
        if (!observation->strategy && originalEcx == observation->attacker &&
            originalArgs[0] == observation->target) {
            observation->applicabilitySeen = true;
            if (!result) observation->excluded = true;
        }
        break;
    case DodgeStrategyValid:
        if (observation->strategy && static_cast<BYTE>(originalArgs[0]) == observation->strategyId &&
            static_cast<BYTE>(originalArgs[1]) == observation->actorSlot &&
            static_cast<BYTE>(originalArgs[2]) == observation->targetSlot) {
            observation->applicabilitySeen = true;
            if (!result) observation->excluded = true;
        }
        break;
    case DodgeStrategyImmune:
        if (observation->strategy && originalEcx == observation->target && result) {
            uintptr_t bypass = 0;
            // 6.6 bypasses this immunity for strategies whose already computed
            // native 423CB3 property is nonzero. Do not call it a second time.
            if (CompiledEngineProfile() != EngineProfileId::Ccz66 ||
                !FrameWord(snapshot.ebp,-0x3C,bypass) || bypass == 0)
                observation->excluded = true;
        }
        break;
    case DodgeStrategyPerson:
        if (observation->strategy && (snapshot.eax & 0xFF) == 0xFF)
            observation->excluded = true;
        break;
    case DodgeStrategyForcedZero:
        if (observation->strategy && (snapshot.eax & 0xFF) == 0) observation->excluded = true;
        break;
    default: break;
    }
}

extern "C" void __cdecl BattleStatsDispatchHit(NativeHitSnapshot* snapshot, unsigned rawRole) {
    InterlockedIncrement(&gNativeHitActive);
    __try {
    const DodgeRole role = static_cast<DodgeRole>(rawRole);
    const bool detaching=InterlockedCompareExchange(&gStatsDetaching,FALSE,FALSE)!=FALSE;
    if (role == DodgeStrategyForcedZero) {
        if (!detaching) ObserveHitGate(role,*snapshot,snapshot->ecx,snapshot->args);
        return;
    }
    uintptr_t target=rawRole<DodgeRoleCount ? gDodgeFallbackTargets[rawRole] : 0;
    if (!detaching) {
        const DodgeLayout& layout=gLayout.dodge;
        const uintptr_t site=snapshot->returnAddress-5;
        const auto found=std::find_if(layout.calls.begin(),layout.calls.end(),
            [site,role](const DodgeCall& call) { return call.site==site && call.role==role; });
        if (found!=layout.calls.end()) target=found->target;
    }
    if (!target) return;
    const unsigned argumentCount = role == DodgeStrategyValid ? 4 :
        (role == DodgeStrategyImmune || role == DodgeStrategyPerson ? 0 : (role == DodgeFinal ? 3 : 1));
    if (detaching) {
        BattleStatsInvokeNative(snapshot,target,argumentCount);
        return;
    }
    if (role != DodgeFinal) {
        const uintptr_t ecx = snapshot->ecx;
        uint32_t args[4]; memcpy(args,snapshot->args,sizeof(args));
        BattleStatsInvokeNative(snapshot,target,argumentCount);
        ObserveHitGate(role,*snapshot,ecx,args);
        return;
    }
    HitObservation observation;
    BeginHitObservation(*snapshot,observation);
    __try {
        BattleStatsInvokeNative(snapshot,target,argumentCount);
        CommitHitObservation(*snapshot,observation);
    } __finally {
        gHitObservation = observation.previous;
    }
    } __finally {
        InterlockedDecrement(&gNativeHitActive);
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
        lock inc dword ptr [gNativeHitActive]
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
        lock dec dword ptr [gNativeHitActive]
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
        lock inc dword ptr [gNativeHitActive]
        mov eax,esp
        push eax
        call BattleStatsObservePhysicalEntry
        add esp,4
        push dword ptr [esp+24]
        call BattleStatsRecordPhysicalResult
        add esp,4
        mov eax, dword ptr [gBattleStatsOriginalPhysicalResult]
        mov dword ptr [esp+12],eax
        lock dec dword ptr [gNativeHitActive]
        popad
        popfd
        push dword ptr [esp-24]
        ret
    }
}

extern "C" __declspec(naked) void BattleStatsCriticalBridge() {
    __asm {
        pushfd
        lock inc dword ptr [gNativeHitActive]
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
        lock dec dword ptr [gNativeHitActive]
        popfd
        ret
    }
}


extern "C" __declspec(naked) void __cdecl BattleStatsInvokeNative(NativeHitSnapshot*, uintptr_t, unsigned) {
    __asm {
        push ebp
        mov ebp,esp
        push ebx
        push esi
        push edi
        push dword ptr [ebp+12]
        push ebp
        push dword ptr [ebp+8]
        mov esi,dword ptr [ebp+8]
        mov ecx,dword ptr [ebp+16]
        test ecx,ecx
        jz argumentsReady
    argumentLoop:
        push dword ptr [esi+ecx*4+36]
        loop argumentLoop
    argumentsReady:
        // Remember argc in an otherwise unused private word underneath the
        // copied arguments, then select the matching indirect CALL encoding.
        cmp dword ptr [ebp+16],4
        je callFour
        cmp dword ptr [ebp+16],3
        je callThree
        cmp dword ptr [ebp+16],1
        je callOne
        push dword ptr [esi+32]
        popfd
        mov edi,[esi]
        mov ebx,[esi+16]
        mov edx,[esi+20]
        mov ecx,[esi+24]
        mov eax,[esi+28]
        mov ebp,[esi+8]
        mov esi,[esi+4]
        call dword ptr [esp+8]
        jmp captureResult
    callOne:
        push dword ptr [esi+32]
        popfd
        mov edi,[esi]
        mov ebx,[esi+16]
        mov edx,[esi+20]
        mov ecx,[esi+24]
        mov eax,[esi+28]
        mov ebp,[esi+8]
        mov esi,[esi+4]
        call dword ptr [esp+12]
        jmp captureResult
    callThree:
        push dword ptr [esi+32]
        popfd
        mov edi,[esi]
        mov ebx,[esi+16]
        mov edx,[esi+20]
        mov ecx,[esi+24]
        mov eax,[esi+28]
        mov ebp,[esi+8]
        mov esi,[esi+4]
        call dword ptr [esp+20]
        jmp captureResult
    callFour:
        push dword ptr [esi+32]
        popfd
        mov edi,[esi]
        mov ebx,[esi+16]
        mov edx,[esi+20]
        mov ecx,[esi+24]
        mov eax,[esi+28]
        mov ebp,[esi+8]
        mov esi,[esi+4]
        call dword ptr [esp+24]
    captureResult:
        pushfd
        pushad
        mov ebp,[esp+40]
        mov edi,[esp+36]
        mov esi,esp
        mov ecx,9
        cld
        rep movsd
        lea esp,[ebp-12]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
#define HIT_BRIDGE(name,role,cleanup) \
extern "C" __declspec(naked) void name() { __asm { pushfd } __asm { pushad } \
    __asm { mov eax,esp } __asm { push role } __asm { push eax } \
    __asm { call BattleStatsDispatchHit } __asm { add esp,8 } __asm { popad } \
    __asm { popfd } __asm { ret cleanup } }
HIT_BRIDGE(BattleStatsFinalHitBridge,DodgeFinal,12)
HIT_BRIDGE(BattleStatsPhysicalImmuneBridge,DodgePhysicalImmune,4)
HIT_BRIDGE(BattleStatsPhysicalValidBridge,DodgePhysicalValid,4)
HIT_BRIDGE(BattleStatsStrategyValidBridge,DodgeStrategyValid,16)
HIT_BRIDGE(BattleStatsStrategyImmuneBridge,DodgeStrategyImmune,0)
HIT_BRIDGE(BattleStatsStrategyPersonBridge,DodgeStrategyPerson,0)
#undef HIT_BRIDGE
extern "C" __declspec(naked) void BattleStatsStrategyForcedZeroBridge() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        push DodgeStrategyForcedZero
        push eax
        call BattleStatsDispatchHit
        add esp,8
        popad
        popfd
        mov byte ptr [ebp-10h],al
        pushfd
        add dword ptr [esp+4],17h
        popfd
        ret
    }
}

#include "BattleStatsReceivedBridges.inl"

bool PrepareDodgeObservers(const PeImage& image, const DodgeLayout& resolved, RuntimeLayout& pending) {
    pending.dodge = resolved;
    pending.dodgePatches.reserve(resolved.calls.size());
    const uintptr_t dodgeBridges[] = {
        reinterpret_cast<uintptr_t>(&BattleStatsFinalHitBridge),
        reinterpret_cast<uintptr_t>(&BattleStatsPhysicalImmuneBridge),
        reinterpret_cast<uintptr_t>(&BattleStatsPhysicalValidBridge),
        reinterpret_cast<uintptr_t>(&BattleStatsStrategyValidBridge),
        reinterpret_cast<uintptr_t>(&BattleStatsStrategyImmuneBridge),
        reinterpret_cast<uintptr_t>(&BattleStatsStrategyPersonBridge),
        reinterpret_cast<uintptr_t>(&BattleStatsStrategyForcedZeroBridge)};
    if (pending.dodge.available) {
        for (const DodgeCall& call : pending.dodge.calls) {
            CodePatch patch;
            bool prepared = false;
            if (call.role == DodgeStrategyForcedZero) {
                patch.site = reinterpret_cast<BYTE*>(call.site); patch.length = 5;
                memcpy(patch.original.data(),patch.site,5);
                void* target = PreparePatchMemory(patch) ?
                    ReachableTarget(patch.site,dodgeBridges[call.role],patch.thunk) : nullptr;
                if (target) {
                    patch.patched[0] = 0xE8;
                    const int32_t displacement = static_cast<int32_t>(reinterpret_cast<BYTE*>(target)-(patch.site+5));
                    memcpy(patch.patched.data()+1,&displacement,4);
                    prepared = true;
                }
            } else {
                prepared = PrepareCallPatch(image,call.site,call.target,dodgeBridges[call.role],patch);
                gDodgeFallbackTargets[call.role] = call.target;
            }
            if (!prepared) {
                pending.dodge.available = false;
                break;
            }
            pending.dodgePatches.push_back(patch);
        }
    }

    return pending.dodge.available;
}

bool InstallDodgeObservers(RuntimeLayout& pending) {
    bool dodgeComplete = pending.dodge.available;
    if (dodgeComplete) {
        for (CodePatch& patch : pending.dodgePatches)
            if (!InstallCodePatch(patch)) { dodgeComplete = false; break; }
    }
    return dodgeComplete;
}

bool RestoreRuntimeLayout(RuntimeLayout& layout) {
    bool restored=true;
    for (auto patch=layout.receivedPatches.rbegin();patch!=layout.receivedPatches.rend();++patch)
        restored=RestoreCodePatch(*patch) && restored;
    restored=RestoreCodePatch(layout.criticalCommit) && restored;
    for (auto patch=layout.dodgePatches.rbegin();patch!=layout.dodgePatches.rend();++patch)
        restored=RestoreCodePatch(*patch) && restored;
    restored=RestoreCodePatch(layout.physicalResult) && restored;
    for (auto patch=layout.settlements.rbegin();patch!=layout.settlements.rend();++patch)
        restored=RestoreCodePatch(*patch) && restored;
    layout.recoveryPending=!restored;
    return restored;
}

static void InstallResolvedBattleStats(const PeImage& image, const ResolvedLayout& resolved) {
    if (gLayout.recoveryPending) return;
    InterlockedExchange(&gStatsDetaching,TRUE);
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
    for (size_t index = 0; index < pending.settlements.size(); ++index) {
        if (!resolved.callSites[index]) continue;
        if (!PrepareCallPatch(image, resolved.callSites[index], resolved.hpSetter,
            bridges[index],pending.settlements[index])) pending.settlements[index]=CodePatch();
    }
    if (resolved.physicalEvents) {
        if (!PrepareCallPatch(image,resolved.physicalResultCall,resolved.physicalResult,
            reinterpret_cast<uintptr_t>(&BattleStatsPhysicalResultBridge),pending.physicalResult))
            pending.physicalResult=CodePatch();
        if (pending.physicalResult.site && !PrepareCriticalPatch(resolved.criticalCommit,resolved.criticalOffset,
            reinterpret_cast<uintptr_t>(&BattleStatsCriticalBridge),pending.criticalCommit))
            pending.criticalCommit=CodePatch();
    }

    PrepareDodgeObservers(image,resolved.dodge,pending);
    PrepareReceivedObservers(image,resolved.received,pending);

    gBattleStatsOriginalSetHp = pending.hpSetter;
    gBattleStatsOriginalPhysicalResult = resolved.physicalResult;
    gBattleStatsCriticalOffset = resolved.criticalOffset;
    bool writeFailed=false;
    for (CodePatch& patch:pending.settlements) {
        if (patch.site && !InstallCodePatch(patch)) { writeFailed=true;break; }
    }
    if (!writeFailed && pending.physicalResult.site) writeFailed=!InstallCodePatch(pending.physicalResult);
    if (!writeFailed && pending.physicalResult.installed && pending.criticalCommit.site)
        writeFailed=!InstallCodePatch(pending.criticalCommit);
    bool dodgeComplete=false;
    if (!writeFailed && pending.dodge.available) {
        dodgeComplete=InstallDodgeObservers(pending);
        writeFailed=!dodgeComplete;
    }
    if (!writeFailed) {
        for (CodePatch& patch:pending.receivedPatches)
            if (!InstallCodePatch(patch)) { writeFailed=true;break; }
    }
    if (writeFailed) {
        RestoreRuntimeLayout(pending);
        pending.dodge.available=false;
        dodgeComplete=false;
    }
    const bool recoveryComplete=!writeFailed && std::all_of(pending.settlements.begin(),pending.settlements.end(),
        [](const CodePatch& patch) { return patch.installed; });
    ScopedStatsLock lock;
    gLayout = std::move(pending);
    gSettlementAvailable[SettlementPhysicalDamage] = !writeFailed && gLayout.settlements[0].installed;
    gSettlementAvailable[SettlementStrategyDamage] = !writeFailed && gLayout.settlements[1].installed;
    gSettlementAvailable[kRecoveryCapability] = recoveryComplete;
    gAttackBlockAvailable = dodgeComplete;
    gSpellBlockAvailable = dodgeComplete;
    gPhysicalReceivedAvailable = !writeFailed && gLayout.received.physical && gLayout.physicalResult.installed;
    gStrategyReceivedAvailable = !writeFailed && gLayout.received.strategy;
    gComboAvailable = !writeFailed && gLayout.physicalResult.installed;
    gCriticalAvailable = !writeFailed && gLayout.physicalResult.installed && gLayout.criticalCommit.installed;
    gHookAvailable = gCriticalAvailable || gComboAvailable || dodgeComplete ||
        gPhysicalReceivedAvailable || gStrategyReceivedAvailable ||
        std::any_of(gSettlementAvailable.begin(), gSettlementAvailable.end(),
            [](bool value) { return value; });
    if (!writeFailed) InterlockedExchange(&gStatsDetaching,FALSE);
    lock.Unlock();
}

void InitializeBattleStats(const wchar_t* gameRoot) {
    ShutdownBattleStats();
    if (gLayout.recoveryPending) return;
    gRoot = gameRoot ? gameRoot : L"";
    ResetBattleStats(L"model-initialization");
    HMODULE module = GetModuleHandleW(nullptr);
    ResolvedLayout resolved;
    PeImage image;
    if (!module || !ResolveLayout(reinterpret_cast<const BYTE*>(module),
        reinterpret_cast<uintptr_t>(module), resolved) ||
        !image.Open(reinterpret_cast<const BYTE*>(module),reinterpret_cast<uintptr_t>(module))) return;
    InstallResolvedBattleStats(image,resolved);
}

static void ObserveBattleStatsSnapshotUnlocked(const BattleSnapshot& current) {
    // R scenes retain stale S tactical rows. Only the authoritative runtime
    // person table may report R camp changes after a successful rejoin.
    if (gLoading || !gActive) return;
    gDeploymentAvailable = (current.capabilities & BattleCapabilityDeployment) != 0;
    for (const BattleUnitSnapshot& unit : current.units) {
        int currentPerson = -1;
        if (unit.dataId < 0 || unit.dataId >= 0xFFFF ||
            !UnitPersonId(unit.slot, currentPerson) || currentPerson != unit.dataId) continue;
        MarkRosterCampUnlocked(unit.dataId, unit.camp);
        if (unit.camp != 0) continue;
        const std::wstring label = unit.name.empty() ?
            L"我方角色 " + std::to_wstring(unit.dataId) : unit.name;
        SetObservedLabelUnlocked(unit.dataId, label);
        if (BattleRosterEntry* roster = FindRosterUnlocked(unit.dataId)) {
            if (roster->tracking && gCampaignTotals.find(unit.dataId) == gCampaignTotals.end())
                TotalsForPersonUnlocked(gCampaignTotals, unit.dataId, unit.slot).label = label;
        }
        // The runtime resolver must verify the native status-2 deployment
        // consumers before this byte can establish battlefield membership.
        // Keep an established person in the set after retreat or death.
        if (gActive && (current.capabilities & BattleCapabilityDeployment) &&
            unit.visibility == 2 && unit.slot >= 0 && unit.slot < 250 &&
            unit.x >= 0 && unit.x < 255 && unit.y >= 0 && unit.y < 255) {
            AddCurrentParticipantUnlocked(unit.dataId, unit.slot);
            gTotals.find(unit.dataId)->second.label = label;
        }
        for (auto* ledger : {&gTotals, &gCampaignTotals}) {
            const auto found = ledger->find(unit.dataId);
            if (found != ledger->end()) {
                found->second.label = label;
                found->second.slot = unit.slot;
            }
        }
    }
    if (gAwaitingSceneReady) {
        TraceLedgerUnlocked(L"scene_ready");
        gAwaitingSceneReady = false;
    }
}

void PollBattleStats(ULONGLONG now) {
    if (gRoot.empty() || now - gLastPoll < 250) return;
    gLastPoll = now;
    BattleSnapshot current;
    if (!ReadBattleSnapshot(current)) return;
    ScopedStatsLock lock;
    ObserveBattleStatsSnapshotUnlocked(current);
}

void SetBattleStatsLoading(bool loading) {
    ScopedStatsLock lock;
    if (loading != gLoading) {
        ++gDodgeGeneration;
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
    if (newCampaign) ClearTotalsUnlocked(L"new-game");
    if (!gCampaignId) {
        FILETIME stamp = {};
        GetSystemTimeAsFileTime(&stamp);
        gCampaignId = (static_cast<uint64_t>(stamp.dwHighDateTime) << 32) | stamp.dwLowDateTime;
        gCampaignId ^= static_cast<uint64_t>(GetCurrentProcessId()) << 16;
        if (!gCampaignId) gCampaignId = 1;
    }
    if (startsInstance) {
        TraceLedgerUnlocked(L"new-battle-before", L"confirmed-scenario-instance");
        gTotals.clear();
        gCurrentBattleParticipants.clear();
        gElapsed = 0;
        gRecordExported = false;
        ++gBattleInstance;
        if (!gBattleInstance) gBattleInstance = 1;
        ++gDodgeGeneration;
        gPhysicalAction = PhysicalActionState();
        gNextActionSerial = 0;
        wchar_t scenario[32] = {};
        swprintf_s(scenario, L"S_%02d.eex", ordinal >> 1);
        gScenario = scenario;
    }
    if (!isBattle) {
        // R scenes are campaign/menu scenes. Never expose the previous S
        // ledger as the current battlefield while retaining campaign totals.
        gTotals.clear();
        gCurrentBattleParticipants.clear();
        gElapsed = 0;
        gRecordExported = false;
        wchar_t scenario[32] = {};
        swprintf_s(scenario, L"R_%02d.eex", ordinal >> 1);
        gScenario = scenario;
    }
    gScenarioOrdinal = ordinal;
    gActive = isBattle;
    gStarted = isBattle ? now : 0;
    gKnownMetrics |= AvailableMetricsUnlocked();
    gLastPoll = 0;
    TraceLedgerUnlocked(startsInstance ? L"new-battle-after" : L"scenario_installed",
        newCampaign ? L"new-game" : L"confirmed-scenario-install");
    lock.Unlock();
}

void NotifyBattleRosterJoined(int personId, int scenarioOrdinal,
    size_t commandOrdinal, ULONGLONG now) {
    (void)now;
    if (personId < 0 || personId >= 1024 || commandOrdinal >= 0xFFFFFFFFu || scenarioOrdinal < 0 ||
        (scenarioOrdinal & 1) != 0) return;
    ScopedStatsLock lock;
    if (!gRosterAvailable || gLoading || gScenarioOrdinal != scenarioOrdinal) return;
    auto roster = gCampaignRoster.find(personId);
    auto ledger = gCampaignTotals.find(personId);
    const bool duplicate = roster != gCampaignRoster.end() && roster->second.joined &&
        roster->second.tracking && !roster->second.changedFaction &&
        roster->second.source == BattleRosterSource::RuntimeJoin3B;
    if (duplicate && ledger != gCampaignTotals.end()) return;
    BattleRosterEntry entry = roster == gCampaignRoster.end() ? BattleRosterEntry() : roster->second;
    if (!duplicate) {
        entry.personId = personId;
        entry.joined = entry.tracking = true;
        entry.changedFaction = false;
        entry.lastCamp = 0;
        entry.joinScenarioOrdinal = scenarioOrdinal;
        entry.joinCommandOrdinal = static_cast<uint32_t>(commandOrdinal);
        entry.source = BattleRosterSource::RuntimeJoin3B;
    }
    const auto label = gLabels.find(personId);
    if (label != gLabels.end()) entry.label = label->second;
    if (entry.label.empty()) entry.label = L"我方角色 " + std::to_wstring(personId);
    BattleUnitTotals total = ledger == gCampaignTotals.end() ? BattleUnitTotals() : ledger->second;
    total.personId = personId;
    total.label = entry.label;
    // All strings are prepared first. If allocating a new map node fails,
    // neither an orphan roster nor an orphan campaign row may survive.
    bool insertedLedger = false;
    if (ledger == gCampaignTotals.end()) {
        ledger = gCampaignTotals.emplace(personId, std::move(total)).first;
        insertedLedger = true;
    }
    try {
        if (roster == gCampaignRoster.end())
            gCampaignRoster.emplace(personId, std::move(entry));
        else roster->second = std::move(entry);
    } catch (...) {
        if (insertedLedger) gCampaignTotals.erase(ledger);
        throw;
    }
    if (!insertedLedger) ledger->second = std::move(total);
    TraceLedgerUnlocked(L"roster-join", L"successful-runtime-3B");
    lock.Unlock();
}

void SetBattleRosterCapability(bool available, const wchar_t* reason) {
    ScopedStatsLock lock;
    const bool changed = gRosterAvailable != available;
    gRosterAvailable = available;
    if (!available && reason && *reason) gStatus = reason;
    if (changed) TraceLedgerUnlocked(available ? L"roster-capability-ready" :
        L"roster-capability-unavailable", reason ? reason : L"");
}

bool ReadBattleRosterTrackingIds(std::vector<int>& personIds) {
    personIds.clear();
    ScopedStatsLock lock(false);
    if (!gRosterAvailable || gLoading) return false;
    personIds.reserve(gCampaignRoster.size());
    for (const auto& pair : gCampaignRoster) {
        const BattleRosterEntry& entry = pair.second;
        if (entry.joined && entry.tracking && !entry.changedFaction)
            personIds.push_back(pair.first);
    }
    return true;
}

void NotifyBattleRosterCamp(int personId, int camp, ULONGLONG now) {
    (void)now;
    if (personId < 0 || personId >= 0xFFFF || camp < 0 || camp > 255) return;
    ScopedStatsLock lock;
    if (gLoading) return;
    MarkRosterCampUnlocked(personId, camp);
    lock.Unlock();
}

void NotifyBattleRosterLabel(int personId, const std::wstring& label) {
    if (personId < 0 || personId >= 1024 || label.empty() || label.size() > 256) return;
    for (wchar_t ch : label) if (ch < 0x20) return;
    ScopedStatsLock lock;
    if (gLoading) return;
    BattleRosterEntry* entry = FindRosterUnlocked(personId);
    if (!entry || !entry->joined) return;
    SetObservedLabelUnlocked(personId, label);
}

void SetBattleStatsStatus(const std::wstring& status, bool historyIncomplete) {
    ScopedStatsLock lock;
    gStatus = status.substr(0, 4096);
    gHistoryIncomplete = gHistoryIncomplete || historyIncomplete;
    lock.Unlock();
}

void ResetBattleStats(const wchar_t* reason) {
    ScopedStatsLock lock;
    ClearTotalsUnlocked(reason);
    gLastPoll = 0;
    lock.Unlock();
}

void ResetBattleStatsForLoadedScenario(const wchar_t* reason, int ordinal) noexcept {
    ScopedStatsLock lock;
    const bool loading=gLoading;
    ClearTotalsUnlocked(reason);
    gLoading=loading;
    gScenarioOrdinal=ordinal>=0 && ordinal<0xFFFF ? ordinal : -1;
    gActive=gScenarioOrdinal>=0 && (gScenarioOrdinal&1)!=0;
    gHistoryIncomplete=true;
    gKnownMetrics=AvailableMetricsUnlocked();
    FILETIME stamp={};
    GetSystemTimeAsFileTime(&stamp);
    gCampaignId=((static_cast<uint64_t>(stamp.dwHighDateTime)<<32)|stamp.dwLowDateTime) ^
        (static_cast<uint64_t>(GetCurrentProcessId())<<16);
    if (!gCampaignId) gCampaignId=1;
    gBattleInstance=gActive ? 1:0;
    gStarted=gActive && !gLoading ? GetTickCount64() : 0;
    gLastPoll=0;
    gAwaitingSceneReady=true;
    // UI labels are optional. A persistent allocation failure must not
    // prevent the empty replacement from recording the newly loaded battle.
    try {
        gStatus=L"该存档历史统计无法恢复，从此次载入后继续记录。";
        wchar_t scenario[32]={};
        swprintf_s(scenario, gActive ? L"S_%02d.eex" : L"R_%02d.eex",
            gScenarioOrdinal >= 0 ? gScenarioOrdinal >> 1 : 0);
        gScenario=scenario;
    } catch (...) {}
    TraceLedgerUnlocked(L"loaded-scenario-fallback",reason);
    lock.Unlock();
}

void ShutdownBattleStats() {
    InterlockedExchange(&gStatsDetaching,TRUE);
    ScopedStatsLock lock;
    gHookAvailable = false;
    gSettlementAvailable.fill(false);
    gCriticalAvailable = false;
    gComboAvailable = false;
    gAttackBlockAvailable = false;
    gSpellBlockAvailable = false;
    gPhysicalReceivedAvailable = gStrategyReceivedAvailable = false;
    gRosterAvailable = false;
    gDeploymentAvailable = false;
    ClearTotalsUnlocked(L"shutdown");
    if (InterlockedCompareExchange(&gNativeHitActive,0,0)!=0 ||
        InterlockedCompareExchange(&gRecording,FALSE,FALSE)!=FALSE ||
        InterlockedCompareExchange(&gEventRecording,FALSE,FALSE)!=FALSE) {
        gLayout.recoveryPending=true;
        lock.Unlock();
        return;
    }
    RuntimeLayout layout=std::move(gLayout);
    gLayout=RuntimeLayout();
    lock.Unlock();
    const bool allRestored=RestoreRuntimeLayout(layout);
    ScopedStatsLock publish;
    if (allRestored) {
        gBattleStatsOriginalSetHp = 0;
        gBattleStatsOriginalPhysicalResult = 0;
        gBattleStatsCriticalOffset = 0;
    } else gLayout=std::move(layout);
    publish.Unlock();
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
    snapshot.physicalReceivedAvailable = gPhysicalReceivedAvailable;
    snapshot.strategyReceivedAvailable = gStrategyReceivedAvailable;
    snapshot.active = gActive;
    snapshot.loading = gLoading;
    snapshot.scenarioOrdinal = gScenarioOrdinal;
    snapshot.campaignId = gCampaignId;
    snapshot.battleInstance = gBattleInstance;
    snapshot.knownMetrics = gKnownMetrics | AvailableMetricsUnlocked();
    snapshot.historyIncomplete = gHistoryIncomplete;
    snapshot.rosterHistoryIncomplete = gRosterHistoryIncomplete;
    snapshot.rosterAvailable = gRosterAvailable;
    snapshot.scenarioIsR = !gActive && gScenarioOrdinal >= 0 &&
        (gScenarioOrdinal & 1) == 0;
    snapshot.elapsedMilliseconds = ElapsedUnlocked(GetTickCount64());
    snapshot.scenario = gScenario;
    snapshot.status = gStatus;
    snapshot.units.reserve(gTotals.size());
    for (const auto& pair : gTotals) snapshot.units.push_back(pair.second);
    snapshot.campaignUnits.reserve(gCampaignTotals.size());
    for (const auto& pair : gCampaignTotals) snapshot.campaignUnits.push_back(pair.second);
    snapshot.currentBattleParticipants.assign(gCurrentBattleParticipants.begin(),
        gCurrentBattleParticipants.end());
    snapshot.campaignRoster.reserve(gCampaignRoster.size());
    for (const auto& pair : gCampaignRoster) snapshot.campaignRoster.push_back(pair.second);
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
    state.rosterHistoryIncomplete = gRosterHistoryIncomplete;
    state.status = gStatus;
    state.active = gActive;
    state.elapsedMilliseconds = ElapsedUnlocked(now);
    state.scenario = gScenario;
    state.units.reserve(gTotals.size());
    for (const auto& pair : gTotals) state.units.push_back(pair.second);
    state.campaignUnits.reserve(gCampaignTotals.size());
    for (const auto& pair : gCampaignTotals) state.campaignUnits.push_back(pair.second);
    state.currentBattleParticipants.assign(gCurrentBattleParticipants.begin(),
        gCurrentBattleParticipants.end());
    state.campaignRoster.reserve(gCampaignRoster.size());
    for (const auto& pair : gCampaignRoster) state.campaignRoster.push_back(pair.second);
    lock.Unlock();
    std::sort(state.units.begin(), state.units.end(), [](const BattleUnitTotals& left,
        const BattleUnitTotals& right) { return left.personId < right.personId; });
    return true;
}

bool RestoreBattleStatsState(const BattleStatsPersistedState& state, ULONGLONG now) {
    if ((state.schemaVersion != 1 && state.schemaVersion != 2 && state.schemaVersion != 3 &&
        state.schemaVersion != 4) || state.campaignRoster.size() > 0xFFFF ||
        state.currentBattleParticipants.size() > kUnitCount ||
        state.units.size() > kUnitCount || state.campaignUnits.size() > 0xFFFF ||
        state.scenario.size() > 1024 || state.status.size() > 4096 ||
        state.scenarioOrdinal < -1 || state.scenarioOrdinal >= 0xFFFF ||
        !ValidateBattleStatsRosterState(state)) return false;
    uintptr_t legacyUnitBase = 0;
    std::wstring gameRoot;
    {
        ScopedStatsLock lock(false);
        legacyUnitBase = gLayout.unitBase;
        gameRoot = gRoot;
    }
    std::map<int, BattleUnitTotals> restored, campaign;
    std::map<int, BattleRosterEntry> roster;
    const bool legacy = state.schemaVersion == 1;
    auto readLedger = [&](const std::vector<BattleUnitTotals>& source,
        std::map<int, BattleUnitTotals>& target) {
        for (BattleUnitTotals unit : source) {
            if (unit.slot < -1 || unit.slot >= static_cast<int>(kUnitCount) ||
                unit.label.size() > 256 || unit.damageDealt < 0 || unit.damageReceived < 0 ||
                unit.recoveryDone < 0 || unit.criticalCount < 0 || unit.comboExtraHitCount < 0 ||
                unit.attackBlockCount < 0 || unit.spellBlockCount < 0 || unit.defeatedEnemies < 0 ||
                unit.physicalReceivedCount < 0 || unit.strategyReceivedCount < 0)
                return false;
            if (legacy) {
                uint16_t personId = 0xFFFF;
                if (!legacyUnitBase || unit.slot < 0 ||
                    !ReadBytes(legacyUnitBase + static_cast<uintptr_t>(unit.slot) * kUnitStride,
                        &personId, sizeof(personId)) || personId == 0xFFFF) continue;
                unit.personId = personId;
            }
            // Earlier production builds did not know either dodge bit. Never
            // reinterpret their reserved/unknown block values as dodge history.
            if (legacy || !(state.knownMetrics & BattleStatsAttackDodge)) unit.attackBlockCount = 0;
            if (legacy || !(state.knownMetrics & BattleStatsStrategyDodge)) unit.spellBlockCount = 0;
            if (state.schemaVersion < 3 || !(state.knownMetrics & BattleStatsPhysicalReceived)) unit.physicalReceivedCount=0;
            if (state.schemaVersion < 3 || !(state.knownMetrics & BattleStatsStrategyReceived)) unit.strategyReceivedCount=0;
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
    for (const BattleRosterEntry& item : state.campaignRoster) {
        if (item.personId < 0 || item.personId >= 0xFFFF || roster.count(item.personId) ||
            item.label.size() > 256 || item.lastCamp < -1 || item.lastCamp > 255)
            return false;
        roster.emplace(item.personId, item);
    }
    // Only legacy formats may infer membership. A schema-4 save is an exact
    // execution ledger and must never gain rows merely because a script has
    // an unexecuted 0x3B branch.
    std::set<int> participants;
    if (state.schemaVersion < 4) {
        for (const auto& pair : campaign) {
            BattleRosterEntry item;
            item.personId = pair.first;
            item.label = pair.second.label;
            item.joined = true;
            item.tracking = true;
            item.lastCamp = 0;
            item.source = BattleRosterSource::ExistingCampaignLedger;
            roster.emplace(item.personId, std::move(item));
        }
        if (state.scenarioOrdinal >= 0 && !gameRoot.empty()) {
            wchar_t filename[40] = {};
            swprintf_s(filename, L"\\RS\\R_%02d.eex", state.scenarioOrdinal >> 1);
            std::vector<int> candidates;
            std::wstring diagnostics;
            if (ExtractScenarioJoinCandidates(gameRoot + filename, candidates, diagnostics)) {
                for (int personId : candidates) {
                    if (roster.count(personId)) continue;
                    BattleRosterEntry item;
                    item.personId = personId;
                    item.label = L"我方角色 " + std::to_wstring(personId);
                    item.joined = item.tracking = true;
                    item.lastCamp = 0;
                    item.joinScenarioOrdinal = state.scenarioOrdinal & ~1;
                    item.source = BattleRosterSource::LegacyStaticR;
                    BattleUnitTotals total;
                    total.personId = personId;
                    total.label = item.label;
                    campaign.emplace(personId, std::move(total));
                    roster.emplace(personId, std::move(item));
                }
            }
        }
        if (state.active) {
            for (const auto& pair : restored) participants.insert(pair.first);
        } else restored.clear();
    } else {
        participants.insert(state.currentBattleParticipants.begin(), state.currentBattleParticipants.end());
    }
    std::map<int, std::wstring> labels;
    for (const auto& pair : campaign) labels[pair.first] = pair.second.label;
    for (const auto& pair : restored) labels[pair.first] = pair.second.label;
    // Old ledgers can carry a newer in-battle name than the campaign row.
    // Normalize display labels before the atomic replacement; counters and
    // person identities remain unchanged.
    for (auto& pair : campaign) pair.second.label = labels[pair.first];
    for (auto& pair : roster) pair.second.label = labels[pair.first];
    std::wstring scenario = state.scenario;
    std::wstring status = state.status;
    if (state.schemaVersion < 4) {
        const wchar_t* warning = L"旧档加入名单由 R 剧本静态推断，可能包含未执行分支。";
        if (status.size() + wcslen(warning) + 1 <= 4096) {
            if (!status.empty()) status += L"\n";
            status += warning;
        }
    }
    // All allocations precede replacement. A failed allocation leaves both
    // live ledgers intact, and every lock releases during stack unwinding.
    ScopedStatsLock lock;
    TraceLedgerUnlocked(L"model_replace_before");
    gTotals = std::move(restored);
    gCampaignTotals = std::move(campaign);
    gCampaignRoster = std::move(roster);
    gCurrentBattleParticipants = std::move(participants);
    gRosterHistoryIncomplete = state.rosterHistoryIncomplete || state.schemaVersion < 4;
    gLabels = std::move(labels);
    ++gDodgeGeneration;
    gPhysicalAction = PhysicalActionState();
    gNextActionSerial = 0;
    gActive = state.active;
    gElapsed = state.elapsedMilliseconds;
    gStarted = gActive && !gLoading ? now : 0;
    gScenario = std::move(scenario);
    gScenarioOrdinal = state.scenarioOrdinal;
    gCampaignId = state.campaignId;
    gBattleInstance = state.battleInstance;
    gKnownMetrics = state.knownMetrics & (state.schemaVersion < 3 ? 0xFFu : 0x3FFu);
    const uint32_t newMetrics = BattleStatsAttackDodge | BattleStatsStrategyDodge |
        BattleStatsPhysicalReceived | BattleStatsStrategyReceived;
    gHistoryIncomplete = state.historyIncomplete || legacy || state.schemaVersion < 3 ||
        (((AvailableMetricsUnlocked() & newMetrics) & ~state.knownMetrics) != 0);
    gHistoryIncomplete = gHistoryIncomplete || gRosterHistoryIncomplete;
    gStatus = std::move(status);
    gRecordExported = false;
    gAwaitingSceneReady = true;
    if (legacy) gKnownMetrics |= AvailableMetricsUnlocked();
    gLastPoll = 0;
    TraceLedgerUnlocked(L"model_replace_after");
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
    output.dodgeAvailable = layout.dodge.available;
    output.finalHit = layout.dodge.finalHit;
    output.physicalReceivedAvailable=layout.received.physical;
    output.strategyReceivedAvailable=layout.received.strategy;
    for (const auto& call:layout.received.calls) output.receivedCalls.push_back(call.site);
    output.spellBlockCommit = layout.spellBlockCommit;
    output.criticalOffset = layout.criticalOffset;
    output.comboOffset = layout.comboOffset;
    output.settlementCalls.assign(layout.callSites.begin(), layout.callSites.end());
    return true;
}

void InitializeBattleStatsModelForTest(uintptr_t unitBase,
    uintptr_t strategyBase, unsigned strategyCount, const wchar_t* gameRoot) {
    ShutdownBattleStats();
    if (gLayout.recoveryPending) return;
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
    // Model-only tests can supply confirmed dodge events directly. Production
    // availability requires the complete final-hit observer graph above.
    gAttackBlockAvailable = true;
    gSpellBlockAvailable = true;
    gPhysicalReceivedAvailable = gStrategyReceivedAvailable = true;
    InterlockedExchange(&gStatsDetaching,FALSE);
    ClearTotalsUnlocked();
    if (gameRoot) gRoot = gameRoot;
    lock.Unlock();
}

void SetBattleStatsModelActiveForTest(bool active) {
    ScopedStatsLock lock;
    gActive = active;
    lock.Unlock();
}

void ObserveBattleStatsSnapshotForTest(const BattleSnapshot& snapshot) {
    ScopedStatsLock lock;
    ObserveBattleStatsSnapshotUnlocked(snapshot);
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

#ifdef XA66_TEST
bool ConfigureBattleStatsReceivedForTest(const BYTE* bytes, uintptr_t virtualBase) {
    if (gLayout.recoveryPending) return false;
    PeImage image; ReceivedLayout layout;
    if (!image.Open(bytes,virtualBase) || !ResolveReceivedLayout(image,layout)) return false;
    ScopedStatsLock lock;
    gLayout.received=std::move(layout);
    gPhysicalReceivedAvailable=gLayout.received.physical;
    gStrategyReceivedAvailable=gLayout.received.strategy;
    InterlockedExchange(&gStatsDetaching,FALSE);
    lock.Unlock();
    return gPhysicalReceivedAvailable || gStrategyReceivedAvailable;
}
void DispatchBattleStatsReceivedForTest(void* rawSnapshot, unsigned rawRole) {
    auto* snapshot=static_cast<NativeHitSnapshot*>(rawSnapshot);
    if (!snapshot) return;
    for (const auto& call:gLayout.received.calls) if (static_cast<unsigned>(call.role)==rawRole) {
        snapshot->returnAddress=call.site+5;
        BattleStatsObserveReceivedSite(snapshot);
        return;
    }
}
void RecordBattleStatsReceivedForTest(bool strategy, int attackerSlot, int targetSlot) {
    RecordReceivedAttempt(strategy,attackerSlot,targetSlot,gDodgeGeneration);
}
bool ConfigureBattleStatsDodgeForTest(const BYTE* bytes, uintptr_t virtualBase) {
    if (gLayout.recoveryPending) return false;
    PeImage image;
    DodgeLayout layout;
    if (!image.Open(bytes,virtualBase) || !ResolveDodgeLayout(image,layout)) return false;
    gLayout.dodge = std::move(layout);
    gAttackBlockAvailable = gSpellBlockAvailable = true;
    return true;
}
void SetBattleStatsDodgeTargetForTest(unsigned role, uintptr_t target) {
    if (role<DodgeRoleCount) gDodgeFallbackTargets[role]=target;
    for (DodgeCall& call : gLayout.dodge.calls)
        if (call.role == role) call.target = target;
}
void DispatchBattleStatsDodgeForTest(void* snapshot, unsigned role) {
    BattleStatsDispatchHit(static_cast<NativeHitSnapshot*>(snapshot),role);
}
bool BattleStatsDodgeObservationActiveForTest() { return gHitObservation != nullptr; }
bool InstallBattleStatsDodgeHooksForTest(BYTE* mapped, unsigned sabotage,
    std::vector<uintptr_t>& sites) {
    if (gLayout.recoveryPending) return false;
    InterlockedExchange(&gStatsDetaching,TRUE);
    PeImage image; DodgeLayout resolved; RuntimeLayout pending;
    pending.unitBase = gLayout.unitBase;
    if (!image.Open(mapped,reinterpret_cast<uintptr_t>(mapped)) || !ResolveDodgeLayout(image,resolved)) return false;
    if (!PrepareDodgeObservers(image,resolved,pending)) return false;
    sites.clear();
    for (const CodePatch& patch : pending.dodgePatches) sites.push_back(reinterpret_cast<uintptr_t>(patch.site));
    if (sabotage < pending.dodgePatches.size()) pending.dodgePatches[sabotage].site[0] = 0xCC;
    const bool installed = InstallDodgeObservers(pending);
    if (!installed) {
        RestoreRuntimeLayout(pending);
        pending.dodge.available=false;
    }
    gLayout = std::move(pending);
    gAttackBlockAvailable = gSpellBlockAvailable = installed;
    if (installed) InterlockedExchange(&gStatsDetaching,FALSE);
    return installed;
}
bool InstallAllBattleStatsHooksForTest(BYTE* mapped, uintptr_t virtualBase,
    std::vector<uintptr_t>& sites) {
    if (gLayout.recoveryPending) return false;
    ResolvedLayout resolved;
    PeImage runtime;
    const uintptr_t mappedBase=reinterpret_cast<uintptr_t>(mapped);
    if (!ResolveLayout(mapped,virtualBase,resolved) || !runtime.Open(mapped,mappedBase)) return false;
    const auto relocate=[mappedBase,virtualBase](uintptr_t& value) {
        if (value) value=mappedBase+value-virtualBase;
    };
    relocate(resolved.hpSetter);relocate(resolved.unitBase);relocate(resolved.strategyBase);
    for (uintptr_t& site:resolved.callSites) relocate(site);
    relocate(resolved.physicalResult);relocate(resolved.physicalResultCall);relocate(resolved.criticalCommit);
    relocate(resolved.attackBlockCommit);relocate(resolved.spellBlockCommit);
    relocate(resolved.dodge.base);relocate(resolved.dodge.finalHit);relocate(resolved.dodge.physicalCalcReturn);
    for (DodgeCall& call:resolved.dodge.calls) { relocate(call.site);relocate(call.target); }
    for (auto& calc:resolved.dodge.strategyCalcs) relocate(calc.first);
    for (uintptr_t& site:resolved.dodge.strategyDirect) relocate(site);
    for (uintptr_t& site:resolved.dodge.strategyActions) relocate(site);
    relocate(resolved.received.base);
    for (auto& call:resolved.received.calls) { relocate(call.site);relocate(call.target); }
    for (auto& site:resolved.received.physicalActions) relocate(site);
    for (auto& site:resolved.received.strategyActions) relocate(site);
    // Reuse the entire production preparation, installation and capability
    // publication path; only the offline address translation is test-specific.
    InstallResolvedBattleStats(runtime,resolved);
    sites.clear();
    for (const CodePatch& patch:gLayout.settlements) sites.push_back(reinterpret_cast<uintptr_t>(patch.site));
    sites.push_back(reinterpret_cast<uintptr_t>(gLayout.physicalResult.site));
    sites.push_back(reinterpret_cast<uintptr_t>(gLayout.criticalCommit.site));
    for (const CodePatch& patch:gLayout.dodgePatches) sites.push_back(reinterpret_cast<uintptr_t>(patch.site));
    for (const CodePatch& patch:gLayout.receivedPatches) sites.push_back(reinterpret_cast<uintptr_t>(patch.site));
    return std::all_of(gLayout.settlements.begin(),gLayout.settlements.end(),[](const CodePatch& patch) { return patch.installed; }) &&
        gLayout.physicalResult.installed && gLayout.criticalCommit.installed && gLayout.dodge.available &&
        gPhysicalReceivedAvailable && gStrategyReceivedAvailable;
}
void SetBattleStatsWriteFaultForTest(int forwardIndex,unsigned forwardFailure,
    int restoreIndex,unsigned restoreFailure) {
    gStatsFailForward=forwardIndex;gStatsForwardFailure=forwardFailure;
    gStatsFailRestore=restoreIndex;gStatsRestoreFailure=restoreFailure;
    gStatsForwardWrites=gStatsRestoreWrites=0;
}
bool BattleStatsRecoveryPendingForTest() { return gLayout.recoveryPending; }
#endif
