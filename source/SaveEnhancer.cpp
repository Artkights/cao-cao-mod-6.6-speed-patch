#include "SaveEnhancer.h"
#include "EngineProfile.h"

#include "BattleStats.h"
#include "BattleStatsPersistence.h"
#include "SaveTransactions.h"
#include "ScenarioLifecycle.h"
#include "Features.h"
#include "Guide.h"
#include "InstallCommon.h"
#include "ResourceIds.h"

#include <algorithm>
#include <commctrl.h>
#include <cstdio>
#include <string>
#include <vector>
#include <array>

extern "C" uintptr_t gSaveStatsLoadContinuation = 0;
extern "C" uintptr_t gSaveNativeAuto = 0;
extern "C" uintptr_t gSaveNativeEndAuto = 0;
extern "C" uintptr_t gSaveNativeSave = 0;
extern "C" int32_t gSaveEndGameFrameOffset = -8;
extern "C" BYTE* gSaveNativeAutoEnable = nullptr;
extern "C" BYTE* gSaveNativeEndAutoEnable = nullptr;
extern "C" uintptr_t gSaveCommitContinuation = 0;
extern "C" uintptr_t gSaveCommitFailure = 0;
extern "C" uintptr_t gSavePostWrite = 0;
extern "C" uintptr_t gSaveLoadStartContinuation = 0;

namespace {
using NativeAutoFn = void (__thiscall*)(void*);
using NativeSaveFn = void (__thiscall*)(void*, int);
using NativeFilenameFn = void (__stdcall*)(char*, int);

struct SaveLayout {
    BYTE* extensionSaveSite = nullptr;
    uintptr_t extensionSaveSlot = 0;
    BYTE originalExtensionSave[6] = {}, patchedExtensionSave[6] = {};
    void* extensionSaveThunk = nullptr;
    BYTE* commitSite = nullptr;
    BYTE* loadStartSite = nullptr;
    uintptr_t commitFailure = 0;
    uintptr_t postWrite = 0;
    uintptr_t nativeWrite = 0;
    uintptr_t nativeClose = 0;
    std::array<BYTE*, 5> ioSites = {};
    std::array<std::array<BYTE, 5>, 5> originalIo = {}, patchedIo = {};
    std::array<void*, 5> ioThunks = {};
    BYTE originalCommit[7] = {}, patchedCommit[7] = {};
    BYTE originalLoadStart[7] = {}, patchedLoadStart[7] = {};
    void* commitThunk = nullptr;
    void* loadStartThunk = nullptr;
    BYTE* callSite = nullptr;
    NativeAutoFn nativeAuto = nullptr;
    NativeAutoFn nativeEndAuto = nullptr;
    BYTE* endCallSite = nullptr;
    BYTE originalEndCall[5] = {};
    BYTE patchedEndCall[5] = {};
    void* endCallThunk = nullptr;
    NativeSaveFn nativeSave = nullptr;
    BYTE* saveResultSite = nullptr;
    BYTE* loadObserveSite = nullptr;
    BYTE* loadResultSite = nullptr;
    BYTE* campaignByte = nullptr;
    NativeFilenameFn nativeFilename = nullptr;
    BYTE* saveFilenameSite = nullptr;
    BYTE originalSaveFilename[5] = {};
    BYTE patchedSaveFilename[5] = {};
    void* saveFilenameThunk = nullptr;
    bool loadObserverIsCall = false;
    bool endObserverIsSave = false;
    int32_t endGameFrameOffset = 0;
    BYTE* nativeAutoEnable = nullptr;
    BYTE* nativeEndAutoEnable = nullptr;
    BYTE* nativeBypassSites[2] = {};
    WORD nativeBypassPatched[2] = {};
    uintptr_t loadContinuation = 0;
    BYTE originalCall[5] = {};
    BYTE patchedCall[5] = {};
    BYTE originalSaveResult[6] = {};
    BYTE patchedSaveResult[6] = {};
    BYTE originalLoadObserve[5] = {};
    BYTE patchedLoadObserve[5] = {};
    BYTE originalLoadResult[6] = {};
    BYTE patchedLoadResult[6] = {};
    void* callThunk = nullptr;
    void* saveResultThunk = nullptr;
    void* loadObserveThunk = nullptr;
    void* loadResultThunk = nullptr;
};

struct SaveEntry {
    bool automatic = false;
    bool record = false;
    std::wstring title;
    std::wstring savePath;
    std::wstring extensionPath;
    std::wstring indexPath;
    std::wstring scenario;
    std::wstring time;
    std::wstring note;
};

SaveLayout gSaveLayout;
std::wstring gSaveRoot;
volatile LONG gSaving = FALSE;
volatile LONG gEnhancerReady = FALSE;
thread_local bool gPrivateSaveActive = false;
thread_local bool gPrivateSaveSucceeded = false;
thread_local int gPrivateSaveRawSlot = -1;
constexpr int kPrivateSaveSlot65 = 256;
constexpr char kPrivateSaveFilename65[] = "SV\\XA65.E5S";
volatile LONG gPendingLoadSlot = -1;
struct LoadContext { int slot; std::wstring path; };
thread_local std::vector<LoadContext> gLoadContexts;
thread_local std::wstring gPrivateTargetBase;
thread_local bool gCommitSucceeded = false;
thread_local unsigned gNativeSaveWriteCount=0;
bool gTransactionalHooks = false;
#ifdef XA66_TEST
bool gSaveFailureDialogForTest=true;
#endif
HWND gBrowser = nullptr;
HWND gSaveList = nullptr;
HWND gSaveDetails = nullptr;
HWND gSaveNote = nullptr;
HWND gSavePreview = nullptr;
HBITMAP gPreviewBitmap = nullptr;
HFONT gSaveFont = nullptr;
std::vector<SaveEntry> gEntries;

bool EnsureFolder(const std::wstring& path) {
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ReadSafe(const void* address, void* output, size_t size) {
    bool ok = false;
    __try { memcpy(output, address, size); ok = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

uintptr_t CallTarget(const BYTE* instruction) {
    if (!instruction || instruction[0] != 0xE8) return 0;
    int32_t displacement = 0;
    memcpy(&displacement, instruction + 1, sizeof(displacement));
    return reinterpret_cast<uintptr_t>(instruction + 5) + displacement;
}

uintptr_t JumpTarget(const BYTE* instruction) {
    if (!instruction || instruction[0] != 0xE9) return 0;
    int32_t displacement = 0;
    memcpy(&displacement, instruction + 1, sizeof(displacement));
    return reinterpret_cast<uintptr_t>(instruction + 5) + displacement;
}

BYTE* FindUniqueResultEpilogue(BYTE* function, size_t limit) {
    if (!function || limit < 6) return nullptr;
    const BYTE expected[] = {0x8B, 0xE5, 0x5D, 0xC2, 0x04, 0x00};
    BYTE* result = nullptr;
    size_t matches = 0;
    for (size_t index = 0; index + sizeof(expected) <= limit; ++index) {
        if (memcmp(function + index, expected, sizeof(expected))) continue;
        result = function + index;
        ++matches;
    }
    return matches == 1 ? result : nullptr;
}

bool ResolveNativeSaveBypass(BYTE* base, uintptr_t virtualBase, size_t imageSize,
    BYTE* nativeSave, SaveLayout& result) {
    size_t writes = 0;
    for (size_t i = 0; i + 9 <= 0x60; ++i) {
        BYTE* p = nativeSave + i;
        if (p[0] != 0x66 || p[1] != 0xC7 || p[2] != 0x05) continue;
        uint32_t address = 0;
        WORD value = 0;
        memcpy(&address, p + 3, sizeof(address));
        memcpy(&value, p + 7, sizeof(value));
        if (imageSize < 2 || address < virtualBase || address - virtualBase >= imageSize - 2)
            return false;
        BYTE* site = base + (address - virtualBase);
        if (writes < 2) {
            if ((value & 0xFF) != 0xEB) return false;
            result.nativeBypassSites[writes] = site;
            result.nativeBypassPatched[writes] = value;
        } else if (writes < 4) {
            if (site != result.nativeBypassSites[writes - 2] || memcmp(site, &value, sizeof(value)))
                return false;
        }
        ++writes;
    }
    return writes == 4 && result.nativeBypassSites[0] != result.nativeBypassSites[1];
}

bool ResolveSaveLayout66InImage(BYTE* base, uintptr_t virtualBase, SaveLayout& result) {
    if (!base) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return false;
    BYTE* text = nullptr;
    size_t textSize = 0;
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        text = base + sections[i].VirtualAddress;
        textSize = sections[i].Misc.VirtualSize;
        break;
    }
    if (!text || textSize < 128) return false;
    BYTE* nativeAuto = nullptr;
    size_t autoMatches = 0;
    for (size_t i = 0; i + 48 <= textSize; ++i) {
        BYTE* p = text + i;
        if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x80 && p[4] == 0x3D &&
            p[9] == 0x00 && p[10] == 0x74 && p[11] == 0x20 && p[12] == 0xB8 &&
            p[17] == 0x50 && p[18] == 0xFF && p[19] == 0x30 && p[20] == 0xC6 &&
            p[21] == 0x00 && p[22] == 0x2C && p[23] == 0x0F && p[24] == 0xB6 &&
            p[25] == 0x41 && p[26] == 0x02 && p[27] == 0x6A && p[28] == 0x14 &&
            p[29] == 0xE8 && p[34] == 0x52 && p[35] == 0xE8) {
            nativeAuto = p;
            ++autoMatches;
        }
    }
    if (autoMatches != 1 || !nativeAuto) return false;
    const uintptr_t nativeSave = CallTarget(nativeAuto + 35);
    if (!nativeSave || nativeSave < reinterpret_cast<uintptr_t>(base) ||
        nativeSave >= reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage) return false;
    BYTE* saveCore = nullptr;
    size_t saveCoreMatches = 0;
    BYTE* nativeSaveBytes = reinterpret_cast<BYTE*>(nativeSave);
    for (size_t index = 0; index + 8 <= 0x60; ++index) {
        BYTE* p = nativeSaveBytes + index;
        if (p[0] != 0xFF || p[1] != 0x75 || p[2] != 0x08 || p[3] != 0xE8) continue;
        const uintptr_t target = CallTarget(p + 3);
        if (target < reinterpret_cast<uintptr_t>(base) ||
            target >= reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage) continue;
        saveCore = reinterpret_cast<BYTE*>(target);
        ++saveCoreMatches;
    }
    if (saveCoreMatches != 1 || !saveCore) return false;
    BYTE* saveResultSite = FindUniqueResultEpilogue(saveCore, 0xC0);
    if (!saveResultSite) return false;

    BYTE* loadCore = nullptr;
    BYTE* loadObserveSite = nullptr;
    BYTE* campaignByte = nullptr;
    uintptr_t loadContinuation = 0;
    size_t loadMatches = 0;
    for (size_t index = 0; index + 12 <= textSize; ++index) {
        BYTE* p = text + index;
        if (p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC || p[3] != 0x6A || p[4] ||
            p[5] != 0x6A || p[6] || p[7] != 0xE9) continue;
        BYTE* cave = reinterpret_cast<BYTE*>(JumpTarget(p + 7));
        if (!cave || cave < text || cave + 45 > text + textSize ||
            cave[0] != 0xFF || cave[1] != 0x35 || cave[6] != 0x0F ||
            cave[7] != 0xB6 || cave[8] != 0x0D || cave[13] != 0x80 ||
            cave[14] != 0x7D || cave[15] != 0x08 || cave[16] != 0x14 ||
            cave[31] != 0xFF || cave[32] != 0x15 || cave[37] != 0x83 ||
            cave[38] != 0xC4 || cave[39] != 0x04 || cave[40] != 0xE9) continue;
        uint32_t campaign = 0;
        memcpy(&campaign, cave + 9, sizeof(campaign));
        if (campaign < virtualBase || campaign >= virtualBase + nt->OptionalHeader.SizeOfImage)
            continue;
        const uintptr_t continuation = JumpTarget(cave + 40);
        if (p[12] != 0x90 || continuation != reinterpret_cast<uintptr_t>(p + 13)) continue;
        loadCore = p;
        loadObserveSite = cave + 40;
        campaignByte = base + (static_cast<uintptr_t>(campaign) - virtualBase);
        loadContinuation = continuation;
        ++loadMatches;
    }
    if (loadMatches != 1 || !loadCore || !loadObserveSite || !campaignByte) return false;
    BYTE* loadResultSite = FindUniqueResultEpilogue(loadCore, 0x140);
    if (!loadResultSite) return false;

    BYTE* callSite = nullptr;
    size_t callMatches = 0;
    for (size_t i = 0; i + 14 <= textSize; ++i) {
        BYTE* p = text + i;
        if (p[0] != 0x83 || p[1] != 0x7D || p[2] != 0x08 || p[3] != 0x00 ||
            p[4] != 0x75 || p[5] != 0x08 || p[6] != 0x8B || p[7] != 0x4D || p[8] != 0xF8 ||
            p[9] != 0xE8 || CallTarget(p + 9) != reinterpret_cast<uintptr_t>(nativeAuto)) continue;
        callSite = p + 9;
        ++callMatches;
    }
    if (callMatches != 1 || !callSite) return false;
    BYTE* endCallSite = nullptr;
    size_t endCallMatches = 0;
    for (size_t i = 0; i + 18 <= textSize; ++i) {
        BYTE* p = text + i;
        const BYTE prefix[] = {0x83,0x7D,0xF4,0,0x74,0x0C,0x6A,0x14,0xE8};
        // The local holding the all-units-finished decision is compiler
        // dependent; it must be a frame local, with this validated skip graph.
        if (p[0] != prefix[0] || p[1] != prefix[1] || static_cast<int8_t>(p[2]) >= 0 ||
            memcmp(p + 3, prefix + 3, sizeof(prefix) - 3) ||
            CallTarget(p + 8) != nativeSave || p[13] != 0xE9) continue;
        endCallSite = p + 8;
        ++endCallMatches;
    }
    if (endCallMatches != 1) return false;
    result = SaveLayout();
    result.callSite = callSite;
    result.nativeAuto = reinterpret_cast<NativeAutoFn>(nativeAuto);
    result.nativeSave = reinterpret_cast<NativeSaveFn>(nativeSave);
    result.endCallSite = endCallSite;
    result.endObserverIsSave = true;
    memcpy(result.originalEndCall, endCallSite, sizeof(result.originalEndCall));
    result.saveResultSite = saveResultSite;
    result.loadObserveSite = loadObserveSite;
    result.loadResultSite = loadResultSite;
    result.campaignByte = campaignByte;
    result.loadContinuation = loadContinuation;
    memcpy(result.originalCall, callSite, sizeof(result.originalCall));
    memcpy(result.originalSaveResult, saveResultSite, sizeof(result.originalSaveResult));
    memcpy(result.originalLoadObserve, loadObserveSite, sizeof(result.originalLoadObserve));
    memcpy(result.originalLoadResult, loadResultSite, sizeof(result.originalLoadResult));
    if (!ResolveNativeSaveBypass(base, virtualBase, nt->OptionalHeader.SizeOfImage,
        nativeSaveBytes, result)) return false;
    // Start and end belong to the same native player-action function.  The
    // start call loads its game owner from this frame local; the end native
    // save has no ECX dependency and must recover the owner from that local.
    if (endCallSite <= callSite || endCallSite - callSite > 0x800 ||
        callSite[-3] != 0x8B || callSite[-2] != 0x4D || static_cast<int8_t>(callSite[-1]) >= 0)
        return false;
    result.endGameFrameOffset = static_cast<int8_t>(callSite[-1]);
    return true;
}

bool ResolveSaveLayout65InImage(BYTE* base, uintptr_t virtualBase, SaveLayout& result) {
    if (!base) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
        return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return false;
    BYTE* text = nullptr;
    size_t textSize = 0;
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (sections[i].VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
            sections[i].Misc.VirtualSize > nt->OptionalHeader.SizeOfImage - sections[i].VirtualAddress)
            return false;
        text = base + sections[i].VirtualAddress;
        textSize = sections[i].Misc.VirtualSize;
        break;
    }
    if (!text || textSize < 128) return false;
    auto inText = [&](const BYTE* p, size_t size) {
        return p >= text && size <= textSize && static_cast<size_t>(p - text) <= textSize - size;
    };
    BYTE* nativeAuto = nullptr;
    size_t autoMatches = 0;
    for (size_t i = 0; i + 48 <= textSize; ++i) {
        BYTE* p = text + i;
        const BYTE divideTurn[] = {0x33,0xD2,0xB9,0x14,0,0,0,0xF7,0xF1,0x52,0xE8};
        if (p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC || p[3] != 0x80 ||
            p[4] != 0x3D || p[9] || p[10] != 0x74 || p[11] != 0x22 || p[12] != 0xB8 ||
            p[17] != 0x50 || p[18] != 0xFF || p[19] != 0x30 || p[20] != 0xC6 ||
            p[21] || p[22] != 0x2C || p[23] != 0x0F || p[24] != 0xB6 || p[25] != 0x41 ||
            p[26] != 0x02 || memcmp(p + 27, divideTurn, sizeof(divideTurn))) continue;
        nativeAuto = p;
        ++autoMatches;
    }
    if (autoMatches != 1) return false;
    BYTE* nativeSave = reinterpret_cast<BYTE*>(CallTarget(nativeAuto + 37));
    if (!inText(nativeSave, 0x60)) return false;
    BYTE* saveCore = nullptr;
    size_t saveMatches = 0;
    for (size_t i = 0; i + 8 <= 0x60; ++i) {
        BYTE* p = nativeSave + i;
        if (p[0] != 0xFF || p[1] != 0x75 || p[2] != 0x08 || p[3] != 0xE8) continue;
        BYTE* target = reinterpret_cast<BYTE*>(CallTarget(p + 3));
        if (!inText(target, 0xC0)) continue;
        saveCore = target;
        ++saveMatches;
    }
    if (saveMatches != 1) return false;
    BYTE* saveResult = FindUniqueResultEpilogue(saveCore, 0xC0);
    if (!saveResult) return false;
    // The native writer is reached only after the save-core success branch.
    BYTE* writer = nullptr;
    size_t writerMatches = 0;
    for (size_t i = 0; i + 8 <= static_cast<size_t>(saveResult - saveCore); ++i) {
        BYTE* p = saveCore + i;
        if (p[0] == 0xFF && p[1] == 0x75 && p[2] == 0x08 && p[3] == 0xE8) {
            BYTE* target = reinterpret_cast<BYTE*>(CallTarget(p + 3));
            if (inText(target, 0xB0)) { writer = target; ++writerMatches; }
        }
    }
    if (writerMatches != 1) return false;
    const BYTE writerPrefix[] = {0x55,0x8B,0xEC,0x6A,0,0x6A,0,0x83,0xEC,0x10,
        0xFF,0x75,0x08,0x8D,0x45,0xE8,0x50,0xE8};
    if (memcmp(writer, writerPrefix, sizeof(writerPrefix))) return false;
    BYTE* filename = reinterpret_cast<BYTE*>(CallTarget(writer + 17));
    if (!inText(filename, 47)) return false;
    const BYTE filenamePrefix[] = {0x55,0x8B,0xEC,0x8B,0x45,0x0C,0x3C,0x14,0x74,0x0F,
        0x0F,0xB6,0x0D};
    const BYTE filenameSlot[] = {0x6B,0xC9,0x14,0x03,0xC1,0x40,0xEB,0x02,0x33,0xC0,0x50};
    if (memcmp(filename, filenamePrefix, sizeof(filenamePrefix)) ||
        memcmp(filename + 17, filenameSlot, sizeof(filenameSlot))) return false;
    uint32_t campaign = 0, autoCampaign = 0;
    memcpy(&campaign, filename + 13, sizeof(campaign));
    memcpy(&autoCampaign, nativeAuto + 13, sizeof(autoCampaign));
    if (campaign != autoCampaign || campaign < virtualBase ||
        campaign - virtualBase >= nt->OptionalHeader.SizeOfImage) return false;
    BYTE* load = nullptr;
    size_t loadMatches = 0;
    for (size_t i = 0; i + 28 <= textSize; ++i) {
        BYTE* p = text + i;
        const BYTE prefix[] = {0x55,0x8B,0xEC,0x6A,0,0x6A,0,0xFF,0x35};
        const BYTE pathCall[] = {0x83,0xEC,0x10,0xFF,0x75,0x08,0x8D,0x45,0xE4,0x50,0xE8};
        if (memcmp(p, prefix, sizeof(prefix)) || memcmp(p + 13, pathCall, sizeof(pathCall)) ||
            CallTarget(p + 23) != reinterpret_cast<uintptr_t>(filename) || !inText(p, 0x140)) continue;
        load = p;
        ++loadMatches;
    }
    if (loadMatches != 1) return false;
    BYTE* loadResult = FindUniqueResultEpilogue(load, 0x140);
    if (!loadResult) return false;
    BYTE* callSite = nullptr;
    BYTE* boundaryAuto = nullptr;
    size_t callMatches = 0;
    for (size_t i = 0; i + 14 <= textSize; ++i) {
        BYTE* p = text + i;
        const BYTE prefix[] = {0x83,0x7D,0x08,0,0x75,0x08,0x8B,0x4D};
        if (memcmp(p, prefix, sizeof(prefix)) || p[9] != 0xE8) continue;
        BYTE* target = reinterpret_cast<BYTE*>(CallTarget(p + 9));
        bool valid = target == nativeAuto;
        // The standard 6.5 build routes the boundary through its 40-slot
        // native autosaver, retaining the original 20-slot implementation.
        // Link the real caller to the same native save and campaign owner.
        const BYTE rotating[] = {0x33,0xD2,0xB9,0x28,0,0,0,0xF7,0xF1,0x83,0xC2,0x28,0x52,0xE8};
        const BYTE restore[] = {0x59,0x58,0x88,0x08,0x5D,0xC3};
        if (inText(target, 51) && !memcmp(target, nativeAuto, 27) &&
            !memcmp(target + 27, rotating, sizeof(rotating)) &&
            !memcmp(target + 45, restore, sizeof(restore)) &&
            CallTarget(target + 40) == reinterpret_cast<uintptr_t>(nativeSave)) valid = true;
        // The enabling flag is independent for the live native autosaver.
        if (!valid && inText(target, 51) && !memcmp(target, nativeAuto, 5) &&
            !memcmp(target + 9, nativeAuto + 9, 13) && target[22] == 0x2B &&
            !memcmp(target + 23, nativeAuto + 23, 4) &&
            !memcmp(target + 27, rotating, sizeof(rotating)) &&
            !memcmp(target + 45, restore, sizeof(restore)) &&
            CallTarget(target + 40) == reinterpret_cast<uintptr_t>(nativeSave)) valid = true;
        if (!valid) continue;
        callSite = p + 9;
        boundaryAuto = target;
        ++callMatches;
    }
    if (callMatches != 1) return false;
    BYTE* endCallSite = nullptr;
    BYTE* nativeEndAuto = nullptr;
    size_t endMatches = 0;
    for (size_t i = 0; i + 14 <= textSize; ++i) {
        BYTE* p = text + i;
        const BYTE prefix[] = {0x83,0x7D,0xF4,0,0x74,0x2B,0x8B,0x4D};
        if (memcmp(p, prefix, sizeof(prefix)) || p[8] != callSite[-1] || p[9] != 0xE8) continue;
        BYTE* target = reinterpret_cast<BYTE*>(CallTarget(p + 9));
        const BYTE rotating[] = {0x33,0xD2,0xB9,0x28,0,0,0,0xF7,0xF1,0x83,0xC2,0x28,0x52,0xE8};
        const BYTE restore[] = {0x59,0x58,0x88,0x08,0x5D,0xC3};
        if (!inText(target, 51) || memcmp(target, boundaryAuto, 5) ||
            memcmp(target + 9, boundaryAuto + 9, 13) || target[22] != 0x2D ||
            memcmp(target + 23, boundaryAuto + 23, 4) ||
            memcmp(target + 27, rotating, sizeof(rotating)) ||
            memcmp(target + 45, restore, sizeof(restore)) ||
            CallTarget(target + 40) != reinterpret_cast<uintptr_t>(nativeSave)) continue;
        endCallSite = p + 9;
        nativeEndAuto = target;
        ++endMatches;
    }
    if (endMatches != 1) return false;
    result = SaveLayout();
    result.callSite = callSite;
    result.nativeAuto = reinterpret_cast<NativeAutoFn>(boundaryAuto);
    result.nativeEndAuto = reinterpret_cast<NativeAutoFn>(nativeEndAuto);
    result.endCallSite = endCallSite;
    uint32_t startEnable = 0, endEnable = 0;
    memcpy(&startEnable, boundaryAuto + 5, sizeof(startEnable));
    memcpy(&endEnable, nativeEndAuto + 5, sizeof(endEnable));
    for (uint32_t flag : {startEnable, endEnable})
        if (flag < virtualBase || flag - virtualBase >= nt->OptionalHeader.SizeOfImage) return false;
    result.nativeAutoEnable = base + (startEnable - virtualBase);
    result.nativeEndAutoEnable = base + (endEnable - virtualBase);
    memcpy(result.originalEndCall, endCallSite, sizeof(result.originalEndCall));
    result.nativeSave = reinterpret_cast<NativeSaveFn>(nativeSave);
    result.saveResultSite = saveResult;
    result.loadObserveSite = load + 23;
    result.loadResultSite = loadResult;
    result.campaignByte = base + (campaign - virtualBase);
    result.nativeFilename = reinterpret_cast<NativeFilenameFn>(filename);
    result.saveFilenameSite = writer + 17;
    result.loadObserverIsCall = true;
    memcpy(result.originalCall, callSite, sizeof(result.originalCall));
    memcpy(result.originalSaveResult, saveResult, sizeof(result.originalSaveResult));
    memcpy(result.originalLoadObserve, result.loadObserveSite, sizeof(result.originalLoadObserve));
    memcpy(result.originalLoadResult, loadResult, sizeof(result.originalLoadResult));
    memcpy(result.originalSaveFilename, result.saveFilenameSite, sizeof(result.originalSaveFilename));
    return ResolveNativeSaveBypass(base, virtualBase, nt->OptionalHeader.SizeOfImage, nativeSave, result);
}

// Persistence has its own verified graph. Automatic turn boundaries are
// optional and can fail without disabling ordinary save/load transactions.
bool ResolveCoreLayout(BYTE* base, uintptr_t virtualBase, SaveLayout& result) {
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
    BYTE* text = nullptr; size_t size = 0;
    auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) if (sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
        text = base + sections[i].VirtualAddress; size = sections[i].Misc.VirtualSize; break;
    }
    auto inside = [&](const BYTE* p, size_t n) { return text && p >= text && n <= size && static_cast<size_t>(p-text) <= size-n; };
    const BYTE prefix[] = {0x55,0x8B,0xEC,0x6A,0,0x6A,0,0x83,0xEC,0x10,0xFF,0x75,0x08,0x8D,0x45,0xE8,0x50,0xE8};
    BYTE* writer = nullptr; unsigned matches = 0;
    for (size_t i = 0; i + 0xB0 <= size; ++i) if (!memcmp(text+i, prefix, sizeof(prefix))) { writer = text+i; ++matches; }
    if (matches != 1) return false;
    BYTE* filename = reinterpret_cast<BYTE*>(CallTarget(writer+17));
    const BYTE fp[] = {0x55,0x8B,0xEC,0x8B,0x45,0x0C,0x3C,0x14,0x74,0x0F,0x0F,0xB6,0x0D};
    if (!inside(filename, 47) || memcmp(filename, fp, sizeof(fp))) return false;
    uint32_t campaign = 0; memcpy(&campaign, filename+13, 4);
    if (campaign < virtualBase || campaign-virtualBase >= nt->OptionalHeader.SizeOfImage) return false;
    BYTE* core = nullptr; BYTE* commit = nullptr; matches = 0;
    const BYTE success[] = {0x83,0xC4,0x04,0x6A,0x01,0x6A,0,0x68};
    for (size_t i = 8; i + 0x33 < size; ++i) {
        BYTE* p = text+i;
        if (p[0] != 0x6A || p[1] || p[2] != 0xE8 || memcmp(p+7, success, sizeof(success)) ||
            p[30] != 0xB0 || p[31] != 1) continue;
        BYTE* before = p-5;
        bool linked = before[0] == 0xE8 && CallTarget(before) == reinterpret_cast<uintptr_t>(writer);
        if (before[0] == 0xE9) {
            BYTE* cave = reinterpret_cast<BYTE*>(JumpTarget(before));
            linked = inside(cave, 45) && cave[0] == 0xE8 && CallTarget(cave) == reinterpret_cast<uintptr_t>(writer) &&
                cave[39] == 0xE9 && JumpTarget(cave+39) == reinterpret_cast<uintptr_t>(p);
            if (linked) {
                if (cave[30]!=0xFF || cave[31]!=0x15) return false;
                uint32_t slot=0; memcpy(&slot,cave+32,4);
                if (slot<virtualBase || slot-virtualBase>nt->OptionalHeader.SizeOfImage-4) return false;
                result.extensionSaveSite=cave+30;
                result.extensionSaveSlot=reinterpret_cast<uintptr_t>(base+(slot-virtualBase));
                memcpy(result.originalExtensionSave,cave+30,6);
            }
        }
        if (!linked || before[-3] != 0xFF || before[-2] != 0x75 || before[-1] != 8) continue;
        commit = p; ++matches;
        for (size_t back = 8; back < 0xA0 && p >= text+back; ++back) {
            BYTE* q = p-back;
            if (q[0] == 0x55 && q[1] == 0x8B && q[2] == 0xEC && q[3] == 0x6A && q[4] == 0xFF) { core=q; break; }
        }
    }
    if (matches != 1 || !core || !commit) return false;
    BYTE* load = nullptr; matches = 0;
    for (size_t i=0; i+0x140<=size; ++i) {
        BYTE* p=text+i;
        const BYTE lp[]={0x55,0x8B,0xEC,0x6A,0,0x6A,0};
        if (memcmp(p,lp,sizeof(lp))) continue;
        bool linked = p[7] == 0xFF && p[8] == 0x35 && p[23] == 0xE8 &&
            CallTarget(p+23) == reinterpret_cast<uintptr_t>(filename);
        if (p[7] == 0xE9) {
            BYTE* cave=reinterpret_cast<BYTE*>(JumpTarget(p+7));
            linked=inside(cave,45) && cave[0]==0xFF && cave[1]==0x35 && cave[6]==0x0F &&
                cave[31]==0xFF && cave[32]==0x15 && cave[40]==0xE9 && JumpTarget(cave+40)==reinterpret_cast<uintptr_t>(p+13);
        }
        if (linked) { load=p; ++matches; }
    }
    if (matches!=1) return false;
    // Observe the actual generated E5S filename in both engine versions.
    // The 6.6 extension-load cave runs earlier and cannot supply that path.
    if (load[23]!=0xE8 || CallTarget(load+23)!=reinterpret_cast<uintptr_t>(filename)) return false;
    result.loadObserveSite=load+23;
    result.loadObserverIsCall=true;
    result.loadContinuation=0;
    memcpy(result.originalLoadObserve,load+23,5);
    result.saveResultSite=FindUniqueResultEpilogue(core,0xC0);
    result.loadResultSite=FindUniqueResultEpilogue(load,0x140);
    if (!result.saveResultSite || !result.loadResultSite) return false;
    result.commitSite=commit; result.postWrite=CallTarget(commit+2); result.commitFailure=reinterpret_cast<uintptr_t>(commit+32);
    result.loadStartSite=load; result.nativeFilename=reinterpret_cast<NativeFilenameFn>(filename);
    result.saveFilenameSite=writer+17; result.campaignByte=base+(campaign-virtualBase);
    const size_t offsets[]={0x55,0x6D,0x85,0x9C,0xA7};
    const BYTE nativePrefixWrite[]={0x68,0,0,3,0};
    if (memcmp(writer+0x48,nativePrefixWrite,sizeof(nativePrefixWrite))) return false;
    for(size_t i=0;i<5;++i) {
        BYTE* p=writer+offsets[i]; if(p[0]!=0xE8) return false;
        auto target=CallTarget(p); if(!inside(reinterpret_cast<BYTE*>(target),0x20)) return false;
        if(i==0) result.nativeWrite=target;
        if(i<4 && target!=result.nativeWrite) return false;
        if(i==4) result.nativeClose=target;
        result.ioSites[i]=p; memcpy(result.originalIo[i].data(),p,5);
    }
    memcpy(result.originalSaveFilename,result.saveFilenameSite,5);
    memcpy(result.originalCommit,commit,7); memcpy(result.originalLoadStart,load,7);
    memcpy(result.originalSaveResult,result.saveResultSite,6); memcpy(result.originalLoadResult,result.loadResultSite,6);
    return true;
}
bool ResolveSaveLayoutInImage(BYTE* base, uintptr_t virtualBase, SaveLayout& result) {
    SaveLayout boundaries;
    bool hasBoundaries = CompiledEngineProfile() == EngineProfileId::Ccz65 ?
        ResolveSaveLayout65InImage(base,virtualBase,boundaries) : ResolveSaveLayout66InImage(base,virtualBase,boundaries);
    result = hasBoundaries ? boundaries : SaveLayout();
    return ResolveCoreLayout(base,virtualBase,result);
}

bool ResolveSaveLayout(SaveLayout& result) {
    BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    return ResolveSaveLayoutInImage(base, reinterpret_cast<uintptr_t>(base), result);
}

bool WriteCode(BYTE* address, const BYTE* bytes, size_t size) {
    DWORD protection = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &protection)) return false;
    memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD ignored = 0;
    return VirtualProtect(address, size, protection, &ignored) != FALSE;
}

bool RestoreSavePatch(BYTE* address, const BYTE* original, const BYTE* patched,
    size_t size) {
    if (!address || !original || !patched || !size) return false;
    if (!memcmp(address, original, size)) return true;
    if (memcmp(address, patched, size)) return false;
    return WriteCode(address, original, size);
}

void* NearTarget(BYTE* instruction, uintptr_t target, void*& allocation) {
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
            const int64_t relative = reinterpret_cast<uintptr_t>(thunk) - reinterpret_cast<uintptr_t>(instruction + 5);
            if (relative < INT32_MIN || relative > INT32_MAX) { VirtualFree(thunk, 0, MEM_RELEASE); continue; }
            thunk[0] = 0xB8;
            const uint32_t absolute = static_cast<uint32_t>(target);
            memcpy(thunk + 1, &absolute, sizeof(absolute));
            thunk[5] = 0xFF; thunk[6] = 0xE0;
            allocation = thunk;
            return thunk;
        }
    }
    return nullptr;
}

bool PrepareJumpPatch(BYTE* site, size_t size, uintptr_t target, BYTE* patched,
    void*& thunk) {
    if (!site || !patched || size < 5) return false;
    void* reachable = NearTarget(site, target, thunk);
    if (!reachable) return false;
    memset(patched, 0x90, size);
    patched[0] = 0xE9;
    const int32_t displacement = static_cast<int32_t>(reinterpret_cast<BYTE*>(reachable) -
        (site + 5));
    memcpy(patched + 1, &displacement, sizeof(displacement));
    return true;
}

int EffectiveSaveSlot(int rawSlot) {
    if ((rawSlot & 0xFF) == 20) return 0;
    BYTE campaign = 0;
    if (rawSlot < 0 || rawSlot > 999 || !gSaveLayout.campaignByte ||
        !ReadSafe(gSaveLayout.campaignByte, &campaign, sizeof(campaign))) return -1;
    const int slot = static_cast<int>(campaign) * 20 + rawSlot + 1;
    return slot >= 0 && slot <= 999 ? slot : -1;
}

std::wstring SaveBaseForSlot(int slot) {
    wchar_t name[32] = {};
    swprintf_s(name, L"\\SV\\SV%03d", slot);
    return gSaveRoot + name;
}

std::wstring StatsPathForSlot(int slot) {
    wchar_t name[32] = {};
    swprintf_s(name, L"\\SV%03d.stats", slot);
    return PatchPrivatePath(gSaveRoot, CompiledEngineProfile()) + L"\\save-index" + name;
}

void PersistBattleStats(int rawSlot) {
    if (!InterlockedCompareExchange(&gEnhancerReady, FALSE, FALSE) || gPrivateSaveActive) return;
    const int slot = EffectiveSaveSlot(rawSlot);
    BattleStatsPersistedState state;
    if (slot < 0 || !CaptureBattleStatsState(state, GetTickCount64())) return;
    EnsureFolder(gSaveRoot + L"\\patch");
    EnsureFolder(PatchPrivatePath(gSaveRoot, CompiledEngineProfile()));
    EnsureFolder(PatchPrivatePath(gSaveRoot, CompiledEngineProfile()) + L"\\save-index");
    std::wstring error;
    SaveBattleStatsSidecar(SaveBaseForSlot(slot) + L".E5S", StatsPathForSlot(slot),
        state, error);
}

void StatsFailureNoThrow(const wchar_t* message) {
    try { SetBattleStatsStatus(message,true); } catch (...) {}
}

int __cdecl BeginLoadSlot(int rawSlot) {
    std::wstring error;
    const bool outer=gLoadContexts.empty();
    try {
        if (outer && !BeginSaveGroupRead(gSaveRoot,error)) {
            StatsFailureNoThrow(error.c_str()); return 0;
        }
        const int slot=EffectiveSaveSlot(rawSlot);
        gLoadContexts.push_back({slot,slot>=0 ? SaveBaseForSlot(slot)+L".E5S" : L""});
        try { BeginScenarioLoad(); }
        catch (...) { gLoadContexts.pop_back(); throw; }
        SetBattleStatsLoading(true);
        return 1;
    } catch (...) { if (outer) EndSaveGroupRead(); return 0; }
}

void __cdecl ObserveLoadSlot(int rawSlot) {
    // Filename/cave observations are part of the invocation already opened
    // by LoadStartBridge; they must never create a second load transaction.
    if (gLoadContexts.empty()) return;
    auto& context=gLoadContexts.back();
    context.slot=EffectiveSaveSlot(rawSlot);
    try { context.path=context.slot>=0 ? SaveBaseForSlot(context.slot)+L".E5S" : L""; }
    catch (...) { context.path.clear(); }
}

void __cdecl FinishLoadSlot(int succeeded) {
    if (gLoadContexts.empty()) return;
    const LoadContext context=std::move(gLoadContexts.back());
    gLoadContexts.pop_back();
    const int slot=context.slot;
    try { if (succeeded && slot >= 0 && !context.path.empty()) {
        BattleStatsPersistedState state;
        std::wstring error;
        const auto result = ReadBattleStatsEmbedded(context.path,state,error);
        bool loaded = result == BattleStatsReadResult::Loaded;
        if (result == BattleStatsReadResult::Missing) {
            loaded=LoadBattleStatsSidecar(context.path,StatsPathForSlot(slot),state,error);
            if (!loaded) {
                // A copied private autosave from an old release may still
                // have its sidecar adjacent to the actual loaded file.
                auto adjacent=context.path.substr(0,context.path.size()-4)+L".stats";
                loaded=LoadBattleStatsSidecar(context.path,adjacent,state,error);
            }
        }
        if (!loaded) {
            state=BattleStatsPersistedState(); state.historyIncomplete=true;
            state.scenarioOrdinal=CurrentScenarioOrdinal();
            state.active=state.scenarioOrdinal>=0 && (state.scenarioOrdinal&1)!=0;
            state.status=result==BattleStatsReadResult::Missing ?
                L"该存档没有历史统计，从此次载入后开始记录。" : error;
        }
        if (!RestoreBattleStatsState(state,GetTickCount64())) {
            state=BattleStatsPersistedState(); state.historyIncomplete=true;
            state.scenarioOrdinal=CurrentScenarioOrdinal();
            state.active=state.scenarioOrdinal>=0 && (state.scenarioOrdinal&1)!=0;
            state.status=L"存档统计内容无效，从此次载入后重新记录。";
            RestoreBattleStatsState(state,GetTickCount64());
        }
    } else if (succeeded) {
        ResetBattleStats();
        StatsFailureNoThrow(L"无法确认本次读取的存档路径，从此次载入后重新记录。");
    } } catch (...) {
        // A native load already replaced the game state. Never keep another
        // save's ledger if allocating or decoding its replacement failed.
        ResetBattleStats();
        StatsFailureNoThrow(L"无法完成存档统计恢复，历史记录不完整。");
    }
    EndScenarioLoad(succeeded!=0);
    SetBattleStatsLoading(!gLoadContexts.empty());
    if (gLoadContexts.empty()) EndSaveGroupRead();
}

void __cdecl FinishSaveSlot(int rawSlot) {
    if (!InterlockedCompareExchange(&gEnhancerReady, FALSE, FALSE)) return;
    if (gTransactionalHooks) {
        if (gPrivateSaveActive && rawSlot==gPrivateSaveRawSlot) gPrivateSaveSucceeded=gCommitSucceeded;
        if (!gCommitSucceeded) AbortSaveTransaction();
        return;
    }
    if (gPrivateSaveActive && rawSlot == gPrivateSaveRawSlot) {
        gPrivateSaveSucceeded = true;
        return;
    }
    PersistBattleStats(rawSlot);
}

void __cdecl FinishNativeSaveResult(int succeeded,int rawSlot) {
    // Opening the staged file may succeed even when a later native step
    // returns failure before reaching the commit bridge. Release its journal
    // and restore the pair on every native return, including AL == 0.
    try { if (succeeded || gTransactionalHooks) FinishSaveSlot(rawSlot); }
    catch (...) { AbortSaveTransaction(); StatsFailureNoThrow(L"存档统计提交未完成。"); }
}

std::wstring FileTimeText(const FILETIME& utc) {
    FILETIME local = {};
    SYSTEMTIME time = {};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &time)) return {};
    wchar_t value[64] = {};
    swprintf_s(value, L"%04u-%02u-%02u %02u:%02u:%02u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return value;
}

bool CaptureScreenshot(const std::wstring& path) {
    if (!gMainWindow || !IsWindow(gMainWindow)) return false;
    RECT client = {};
    if (!GetClientRect(gMainWindow, &client) || client.right <= 0 || client.bottom <= 0) return false;
    HDC source = GetDC(gMainWindow);
    HDC memory = source ? CreateCompatibleDC(source) : nullptr;
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(source, client.right, client.bottom) : nullptr;
    if (!source || !memory || !bitmap) {
        if (bitmap) DeleteObject(bitmap); if (memory) DeleteDC(memory); if (source) ReleaseDC(gMainWindow, source);
        return false;
    }
    HGDIOBJ old = SelectObject(memory, bitmap);
    const bool copied = BitBlt(memory, 0, 0, client.right, client.bottom, source, 0, 0, SRCCOPY) != FALSE;
    BITMAPINFOHEADER header = {};
    header.biSize = sizeof(header);
    header.biWidth = client.right;
    header.biHeight = client.bottom;
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    const DWORD stride = (client.right * 3u + 3u) & ~3u;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * client.bottom);
    BITMAPINFO information = {};
    information.bmiHeader = header;
    const bool read = copied && GetDIBits(memory, bitmap, 0, client.bottom, pixels.data(), &information, DIB_RGB_COLORS) != 0;
    SelectObject(memory, old);
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(gMainWindow, source);
    if (!read) return false;
    BITMAPFILEHEADER fileHeader = {};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(header);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixels.size());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr) && written == sizeof(fileHeader) &&
        WriteFile(file, &header, sizeof(header), &written, nullptr) && written == sizeof(header) &&
        WriteFile(file, pixels.data(), static_cast<DWORD>(pixels.size()), &written, nullptr) && written == pixels.size();
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

bool MoveSavedFile(const std::wstring& source, const std::wstring& target) {
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    return MoveFileExW(source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

void SaveFilename65Impl(char* output, int rawSlot) {
    // The native stack buffer holds 16 bytes.  A short private filename
    // isolates this transaction from every numbered player/native save.
    if (gTransactionalHooks && output) {
        gSaveLayout.nativeFilename(output,rawSlot);
        const std::wstring nativeName(output,output+strlen(output));
        const int slot=EffectiveSaveSlot(rawSlot);
        const std::wstring sourceExtension=slot>=0 ? SaveBaseForSlot(slot)+L".EXS" : L"";
        const std::wstring target=gPrivateSaveActive ? gPrivateTargetBase+L".E5S" : gSaveRoot+L"\\"+nativeName;
        const std::wstring extension=gPrivateSaveActive ? gPrivateTargetBase+L".EXS" : sourceExtension;
        gCommitSucceeded=false;
        gNativeSaveWriteCount=0;
        std::wstring error;
        if (ScenarioLoadInProgress() || !BeginSaveTransaction(gSaveRoot,target,sourceExtension,extension,output,error,
                CompiledEngineProfile()==EngineProfileId::Ccz66)) {
            // Intentionally invalid filename: a failed transaction must not
            // fall through and let the engine overwrite the original.
            strcpy_s(output,16,"?:\\XAFAIL.E5S");
            SetBattleStatsStatus(error.empty()?L"正在读档，暂不能保存。":error,true);
        }
        return;
    }
    if (output && rawSlot == kPrivateSaveSlot65 && gPrivateSaveActive) {
        memcpy(output, kPrivateSaveFilename65, sizeof(kPrivateSaveFilename65));
        return;
    }
    gSaveLayout.nativeFilename(output, rawSlot);
}

void __stdcall SaveFilename65Bridge(char* output, int rawSlot) {
    try { SaveFilename65Impl(output,rawSlot); }
    catch (...) {
        AbortSaveTransaction();
        if (output) strcpy_s(output,16,"?:\\XAFAIL.E5S");
        StatsFailureNoThrow(L"无法建立完整存档事务，原存档未被替换。");
    }
}

void __stdcall LoadFilename65Bridge(char* output, int rawSlot) {
    ObserveLoadSlot(rawSlot);
    gSaveLayout.nativeFilename(output, rawSlot);
    if (output && !gLoadContexts.empty()) {
        try { gLoadContexts.back().path=gSaveRoot+L"\\"+std::wstring(output,output+strlen(output)); }
        catch (...) { gLoadContexts.back().path.clear(); }
    }
}

int __fastcall SaveWriteBridge(void* file, void*, const void* data, unsigned bytes) {
    using WriteFn=int(__thiscall*)(void*,const void*,unsigned);
    int written=reinterpret_cast<WriteFn>(gSaveLayout.nativeWrite)(file,data,bytes);
    const bool validPrefix=gNativeSaveWriteCount++!=0 || bytes==0x30000;
    ObserveSaveWrite(bytes,validPrefix ? static_cast<unsigned>(written):0);
    return written;
}
int __fastcall SaveCloseBridge(void* file, void*) {
    using CloseFn=int(__thiscall*)(void*);
    HANDLE handle=nullptr; bool flushed=ReadSafe(file,&handle,sizeof(handle)) &&
        handle && handle!=INVALID_HANDLE_VALUE && FlushFileBuffers(handle);
    int closed=reinterpret_cast<CloseFn>(gSaveLayout.nativeClose)(file);
    ObserveSaveClose(flushed && closed!=0);
    return closed;
}
int __cdecl CommitSave() {
    std::wstring error;
    try { gCommitSucceeded=FinishSaveTransaction(error); }
    catch (...) { gCommitSucceeded=false; AbortSaveTransaction(); }
    if (!gCommitSucceeded) {
        StatsFailureNoThrow(error.empty()?L"存档事务未能完成，原存档保留。":error.c_str());
        bool showDialog=!gPrivateSaveActive;
#ifdef XA66_TEST
        showDialog=showDialog && gSaveFailureDialogForTest;
#endif
        if (showDialog) MessageBoxW(gMainWindow,error.empty()?L"存档事务未能完成，原存档保留。":error.c_str(),L"存档未完成",MB_OK|MB_ICONERROR);
    }
    return gCommitSucceeded ? 1:0;
}
void __cdecl SaveExtensionBridge(int slot) {
    if (!SaveTransactionHealthy() || !gSaveLayout.extensionSaveSlot) { ObserveSaveExtension(false); return; }
    uintptr_t target=0;
    if (!ReadSafe(reinterpret_cast<void*>(gSaveLayout.extensionSaveSlot),&target,sizeof(target)) || !target) {
        ObserveSaveExtension(false); return;
    }
    using ExtensionFn=void(__cdecl*)(int);
    reinterpret_cast<ExtensionFn>(target)(slot);
    ObserveNativeSaveExtension(target);
}

bool NativeSaveSafe(void* game, int slot) {
    WORD before[2] = {};
    for (size_t i = 0; i < 2; ++i) {
        if (gSaveLayout.nativeBypassSites[i] &&
            !ReadSafe(gSaveLayout.nativeBypassSites[i], &before[i], sizeof(before[i]))) return false;
    }
    bool saved = false;
    __try { gSaveLayout.nativeSave(game, slot); saved = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { saved = false; }
    // Both native versions temporarily skip metadata accesses while writing special
    // slots.  Restore its own branch edits even when the writer faults.
    for (size_t i = 0; i < 2; ++i) {
        BYTE* site = gSaveLayout.nativeBypassSites[i];
        if (!site) continue;
        WORD current = 0;
        if (!ReadSafe(site, &current, sizeof(current))) { saved = false; continue; }
        if (current == before[i]) continue;
        if (current != gSaveLayout.nativeBypassPatched[i] ||
            !WriteCode(site, reinterpret_cast<const BYTE*>(&before[i]), sizeof(before[i]))) saved = false;
    }
    return saved;
}

void SavePrivate(void* game, bool atStart) {
    if (!InterlockedCompareExchange(&gEnhancerReady, FALSE, FALSE) || !game ||
        !gSaveLayout.nativeSave || ScenarioLoadInProgress() || InterlockedCompareExchange(&gSaving, TRUE, FALSE)) return;
    BYTE turn = 0;
    if (!ReadSafe(static_cast<const BYTE*>(game) + 2, &turn, 1)) {
        InterlockedExchange(&gSaving, FALSE);
        return;
    }
    const int slots = Bound(gFeatureSettings.autoSaveSlots, 1, 100);
    const int rotating = turn % slots;
    const bool engine65 = CompiledEngineProfile() == EngineProfileId::Ccz65;
    const int nativeSlot = engine65 ? kPrivateSaveSlot65 : (atStart ? 800 : 900) + rotating;
    const int physicalSlot = engine65 ? 0 : EffectiveSaveSlot(nativeSlot);
    if (physicalSlot < 0) { InterlockedExchange(&gSaving, FALSE); return; }
    const std::wstring sourceBase = engine65 ? gSaveRoot + L"\\SV\\XA65" : SaveBaseForSlot(physicalSlot);
    // Remove this transaction's staging files first.  A failed native save
    // must never relabel the previous rotation entry as a fresh save.
    for (const wchar_t* extension : {L".E5S", L".EXS"}) {
        if (gTransactionalHooks) break;
        if (!DeleteFileW((sourceBase + extension).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            InterlockedExchange(&gSaving, FALSE);
            return;
        }
    }
    gPrivateSaveRawSlot = nativeSlot;
    gPrivateSaveSucceeded = false;
    const std::wstring privateRoot = PatchPrivatePath(gSaveRoot, CompiledEngineProfile());
    const std::wstring folder = privateRoot + L"\\autosave";
    wchar_t privateName[64] = {};
    swprintf_s(privateName, L"\\%s-%03d", atStart ? L"start" : L"end", rotating);
    const std::wstring targetBase = folder + privateName;
    gPrivateTargetBase=targetBase;
    gPrivateSaveActive = true;
    const bool saved = NativeSaveSafe(game, nativeSlot);
    gPrivateSaveActive = false;
    gPrivateSaveRawSlot = -1;
    if (!saved || !gPrivateSaveSucceeded) { InterlockedExchange(&gSaving, FALSE); return; }
    if (!EnsureFolder(gSaveRoot + L"\\patch") || !EnsureFolder(privateRoot) || !EnsureFolder(folder)) {
        InterlockedExchange(&gSaving, FALSE);
        return;
    }
    if (!gTransactionalHooks && !MoveSavedFile(sourceBase + L".E5S", targetBase + L".E5S")) {
        InterlockedExchange(&gSaving, FALSE);
        return;
    }
    if (!gTransactionalHooks && !MoveSavedFile(sourceBase + L".EXS", targetBase + L".EXS"))
        DeleteFileW((targetBase + L".EXS").c_str());
    BattleStatsPersistedState state;
    std::wstring statsError;
    if (!gTransactionalHooks && (!CaptureBattleStatsState(state, GetTickCount64()) ||
        !SaveBattleStatsSidecar(targetBase + L".E5S", targetBase + L".stats", state, statsError))
        ) DeleteFileW((targetBase + L".stats").c_str());
    std::wstring scenario;
    BattleStatsPersistedState savedState;
    CaptureBattleStatsState(savedState,GetTickCount64());
    scenario=savedState.scenario;
    std::wstring hash, ignored;
    XiaoAnInstall::Sha256File(targetBase + L".E5S", hash, ignored);
    const std::wstring index = targetBase + L".ini";
    WritePrivateProfileStringW(L"Save", L"Kind", atStart ? L"TurnStart" : L"TurnEnd", index.c_str());
    WritePrivateProfileStringW(L"Save", L"Turn", std::to_wstring(turn).c_str(), index.c_str());
    WritePrivateProfileStringW(L"Save", L"Scenario", scenario.c_str(), index.c_str());
    WritePrivateProfileStringW(L"Save", L"Hash", hash.c_str(), index.c_str());
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    WritePrivateProfileStringW(L"Save", L"Time", FileTimeText(now).c_str(), index.c_str());
    if (gFeatureSettings.saveScreenshots) CaptureScreenshot(targetBase + L".bmp");
    InterlockedExchange(&gSaving, FALSE);
}

void __cdecl TurnStartPrivateHelper(void* game) {
    try { if (gFeatureSettings.autoSaveTurnStart) SavePrivate(game, true); }
    catch (...) {
        AbortSaveTransaction(); gPrivateSaveActive=false; gPrivateTargetBase.clear();
        InterlockedExchange(&gSaving,FALSE);
        StatsFailureNoThrow(L"回合开始自动存档未完成。");
    }
}

void __cdecl TurnEndPrivateHelper(void* game) {
    try { if (gFeatureSettings.autoSaveTurnEnd) SavePrivate(game, false); }
    catch (...) {
        AbortSaveTransaction(); gPrivateSaveActive=false; gPrivateTargetBase.clear();
        InterlockedExchange(&gSaving,FALSE);
        StatsFailureNoThrow(L"回合结束自动存档未完成。");
    }
}

__declspec(naked) void TurnEndBridge() {
    __asm {
        push ebp
        mov ebp, esp
        push ecx
        // The verified standard-6.5 disabled branch jumps into unmatched
        // POPs.  A disabled native autosaver is a no-op; bypass that broken
        // branch without changing game bytes or its enabled behavior.
        pushfd
        push eax
        mov eax, dword ptr [gSaveNativeEndAutoEnable]
        test eax, eax
        jz invokeNative
        cmp byte ptr [eax], 0
        je skipNative
    invokeNative:
        pop eax
        popfd
        call dword ptr [gSaveNativeEndAuto]
        jmp nativeFinished
    skipNative:
        pop eax
        popfd
    nativeFinished:
        pushfd
        pushad
        push dword ptr [ebp - 4]
        call TurnEndPrivateHelper
        add esp, 4
        popad
        popfd
        mov esp, ebp
        pop ebp
        ret
    }
}

__declspec(naked) void TurnEndSaveBridge() {
    __asm {
        // The native call sees the original ECX owner and DWORD slot.
        // Keep its output registers and flags while writing the private copy.
        push ebp
        mov ebp, esp
        push ecx
        push dword ptr [ebp + 8]
        call dword ptr [gSaveNativeSave]
        pushfd
        pushad
        mov eax, dword ptr [ebp]
        mov edx, dword ptr [gSaveEndGameFrameOffset]
        push dword ptr [eax + edx]
        call TurnEndPrivateHelper
        add esp, 4
        popad
        popfd
        mov esp, ebp
        pop ebp
        ret 4
    }
}

__declspec(naked) void SaveResultBridge() {
    __asm {
        pushfd
        pushad
        push dword ptr [ebp + 8]
        movzx eax,al
        push eax
        call FinishNativeSaveResult
        add esp, 8
        popad
        popfd
        mov esp, ebp
        pop ebp
        ret 4
    }
}

__declspec(naked) void SaveCommitBridge() {
    __asm {
        pushfd
        pushad
        call CommitSave
        test eax,eax
        jz failed
        popad
        popfd
        push 0
        call dword ptr [gSavePostWrite]
        jmp dword ptr [gSaveCommitContinuation]
    failed:
        popad
        popfd
        xor eax,eax
        jmp dword ptr [gSaveCommitFailure]
    }
}

__declspec(naked) void LoadStartBridge() {
    __asm {
        push ebp
        mov ebp,esp
        push 0
        push 0
        pushfd
        pushad
        push dword ptr [ebp+8]
        call BeginLoadSlot
        add esp,4
        test eax,eax
        jz blocked
        popad
        popfd
        jmp dword ptr [gSaveLoadStartContinuation]
    blocked:
        popad
        popfd
        xor eax,eax
        mov esp,ebp
        pop ebp
        ret 4
    }
}

__declspec(naked) void LoadObserveBridge() {
    __asm {
        pushfd
        pushad
        push dword ptr [ebp + 8]
        call ObserveLoadSlot
        add esp, 4
        popad
        popfd
        jmp dword ptr [gSaveStatsLoadContinuation]
    }
}

__declspec(naked) void LoadResultBridge() {
    __asm {
        pushfd
        pushad
        movzx eax, al
        push eax
        call FinishLoadSlot
        add esp, 4
        popad
        popfd
        mov esp, ebp
        pop ebp
        ret 4
    }
}

__declspec(naked) void TurnBoundaryBridge() {
    __asm {
        push ebp
        mov ebp, esp
        push ecx
        // The verified standard-6.5 disabled branch jumps into unmatched
        // POPs.  A disabled native autosaver is a no-op; bypass that broken
        // branch without changing game bytes or its enabled behavior.
        pushfd
        push eax
        mov eax, dword ptr [gSaveNativeAutoEnable]
        test eax, eax
        jz invokeNative
        cmp byte ptr [eax], 0
        je skipNative
    invokeNative:
        pop eax
        popfd
        call dword ptr [gSaveNativeAuto]
        jmp nativeFinished
    skipNative:
        pop eax
        popfd
    nativeFinished:
        pushfd
        pushad
        push dword ptr [ebp - 4]
        call TurnStartPrivateHelper
        add esp, 4
        popad
        popfd
        mov esp, ebp
        pop ebp
        ret
    }
}

std::wstring ReadIni(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
    wchar_t value[1024] = {};
    GetPrivateProfileStringW(section, key, L"", value, static_cast<DWORD>(_countof(value)), path.c_str());
    return value;
}

void AddAutosaves() {
    const std::wstring pattern = PatchPrivatePath(gSaveRoot, CompiledEngineProfile()) + L"\\autosave\\*.ini";
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        SaveEntry entry;
        entry.automatic = true;
        entry.indexPath = PatchPrivatePath(gSaveRoot, CompiledEngineProfile()) + L"\\autosave\\" + data.cFileName;
        std::wstring base = entry.indexPath.substr(0, entry.indexPath.size() - 4);
        entry.savePath = base + L".E5S";
        entry.extensionPath = base + L".EXS";
        const std::wstring kind = ReadIni(entry.indexPath, L"Save", L"Kind");
        const std::wstring turn = ReadIni(entry.indexPath, L"Save", L"Turn");
        entry.title = std::wstring(kind == L"TurnEnd" ? L"回合结束" : L"回合开始") +
            L" / 第 " + turn + L" 回合";
        entry.scenario = ReadIni(entry.indexPath, L"Save", L"Scenario");
        entry.time = ReadIni(entry.indexPath, L"Save", L"Time");
        entry.note = ReadIni(entry.indexPath, L"User", L"Note");
        gEntries.push_back(std::move(entry));
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

void AddOrdinarySaves() {
    const std::wstring indexFolder = PatchPrivatePath(gSaveRoot, CompiledEngineProfile()) + L"\\save-index";
    EnsureFolder(indexFolder);
    const std::wstring pattern = gSaveRoot + L"\\SV\\SV*.E5S";
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        SaveEntry entry;
        entry.title = std::wstring(L"普通存档 / ") + data.cFileName;
        entry.savePath = gSaveRoot + L"\\SV\\" + data.cFileName;
        std::wstring stem = data.cFileName;
        stem.resize(stem.size() - 4);
        entry.extensionPath = gSaveRoot + L"\\SV\\" + stem + L".EXS";
        entry.indexPath = indexFolder + L"\\" + stem + L".ini";
        entry.time = FileTimeText(data.ftLastWriteTime);
        entry.note = ReadIni(entry.indexPath, L"User", L"Note");
        gEntries.push_back(std::move(entry));
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

void AddBattleRecords() {
    const std::wstring folder = PatchPrivatePath(gSaveRoot, CompiledEngineProfile()) + L"\\records";
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW((folder + L"\\*.txt").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        SaveEntry entry;
        entry.record = true;
        entry.title = std::wstring(L"战斗记录 / ") + data.cFileName;
        entry.savePath = folder + L"\\" + data.cFileName;
        entry.indexPath = entry.savePath + L".ini";
        entry.time = FileTimeText(data.ftLastWriteTime);
        entry.note = ReadIni(entry.indexPath, L"User", L"Note");
        gEntries.push_back(std::move(entry));
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

void RefreshBrowser() {
    gEntries.clear();
    AddAutosaves();
    AddOrdinarySaves();
    AddBattleRecords();
    std::sort(gEntries.begin(), gEntries.end(), [](const SaveEntry& left, const SaveEntry& right) {
        return left.time > right.time;
    });
    SendMessageW(gSaveList, LVM_DELETEALLITEMS, 0, 0);
    for (size_t i = 0; i < gEntries.size(); ++i) {
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = gEntries[i].title.data();
        item.lParam = static_cast<LPARAM>(i);
        SendMessageW(gSaveList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        LVITEMW sub = {};
        sub.iItem = static_cast<int>(i);
        sub.iSubItem = 1;
        sub.pszText = gEntries[i].time.data();
        SendMessageW(gSaveList, LVM_SETITEMTEXTW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&sub));
        sub.iSubItem = 2;
        sub.pszText = gEntries[i].scenario.data();
        SendMessageW(gSaveList, LVM_SETITEMTEXTW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&sub));
    }
    if (!gEntries.empty()) {
        LVITEMW state = {};
        state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        state.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(gSaveList, LVM_SETITEMSTATE, 0, reinterpret_cast<LPARAM>(&state));
    }
}

int SelectedEntry() {
    const int row = static_cast<int>(SendMessageW(gSaveList, LVM_GETNEXTITEM,
        static_cast<WPARAM>(-1), LVNI_SELECTED));
    if (row < 0) return -1;
    LVITEMW item = {};
    item.mask = LVIF_PARAM; item.iItem = row;
    if (!SendMessageW(gSaveList, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) || item.lParam < 0 ||
        static_cast<size_t>(item.lParam) >= gEntries.size()) return -1;
    return static_cast<int>(item.lParam);
}

void ShowEntry() {
    if (gPreviewBitmap) DeleteObject(gPreviewBitmap);
    gPreviewBitmap = nullptr;
    if (gSavePreview) SendMessageW(gSavePreview, STM_SETIMAGE, IMAGE_BITMAP, 0);
    const int index = SelectedEntry();
    if (index < 0) { SetWindowTextW(gSaveDetails, L"没有可显示的存档记录。"); return; }
    const SaveEntry& entry = gEntries[static_cast<size_t>(index)];
    if (entry.automatic) {
        std::wstring preview = entry.savePath;
        if (preview.size() >= 4) preview.replace(preview.size() - 4, 4, L".bmp");
        gPreviewBitmap = static_cast<HBITMAP>(LoadImageW(nullptr, preview.c_str(), IMAGE_BITMAP,
            240, 135, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
        if (gPreviewBitmap && gSavePreview)
            SendMessageW(gSavePreview, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(gPreviewBitmap));
    }
    if (entry.record) {
        HANDLE file = CreateFileW(entry.savePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::wstring text;
        if (file != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size = {};
            if (GetFileSizeEx(file, &size) && size.QuadPart >= 2 && size.QuadPart <= 1024 * 1024) {
                std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));
                DWORD read = 0;
                if (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                    read == bytes.size()) {
                    size_t offset = bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE ? 2 : 0;
                    text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + offset),
                        (bytes.size() - offset) / sizeof(wchar_t));
                }
            }
            CloseHandle(file);
        }
        SetWindowTextW(gSaveDetails, text.empty() ? L"战斗记录无法读取。" : text.c_str());
        SetWindowTextW(gSaveNote, entry.note.c_str());
        return;
    }
    std::wstring hash, error;
    XiaoAnInstall::Sha256File(entry.savePath, hash, error);
    std::wstring text = entry.title + L"\r\n时间：" + entry.time;
    if (!entry.scenario.empty()) text += L"\r\n剧本：" + entry.scenario;
    if (!hash.empty()) text += L"\r\n文件校验：" + hash;
    SetWindowTextW(gSaveDetails, text.c_str());
    SetWindowTextW(gSaveNote, entry.note.c_str());
}

void LayoutBrowser(HWND window) {
    RECT client = {}; GetClientRect(window, &client);
    const int width = client.right, height = client.bottom, margin = 10;
    MoveWindow(gSaveList, margin, margin, std::max(300, width * 58 / 100), height - margin * 2, TRUE);
    const int left = std::max(320, width * 58 / 100) + margin * 2;
    MoveWindow(gSavePreview, left, margin, std::max(180, width - left - margin), 135, TRUE);
    MoveWindow(gSaveDetails, left, 151, std::max(180, width - left - margin), 105, TRUE);
    MoveWindow(gSaveNote, left, 266, std::max(180, width - left - margin), std::max(60, height - 316), TRUE);
    MoveWindow(GetDlgItem(window, 0x6A03), width - 90, height - 40, 80, 28, TRUE);
}

LRESULT CALLBACK BrowserProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        gBrowser = window;
        NONCLIENTMETRICSW metrics = {sizeof(metrics)};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            gSaveFont = CreateFontIndirectW(&metrics.lfMessageFont);
        gSaveList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(0x6A00), gInstance, nullptr);
        SendMessageW(gSaveList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* names[] = {L"存档", L"时间", L"剧本"};
        const int widths[] = {260, 140, 100};
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW column = {}; column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(names[i]); column.cx = widths[i];
            SendMessageW(gSaveList, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&column));
        }
        gSaveDetails = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, nullptr,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(0x6A01), gInstance, nullptr);
        gSavePreview = CreateWindowExW(WS_EX_CLIENTEDGE, WC_STATICW, nullptr,
            WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_CENTERIMAGE,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(0x6A04), gInstance, nullptr);
        gSaveNote = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, nullptr,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | WS_VSCROLL | ES_AUTOVSCROLL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(0x6A02), gInstance, nullptr);
        HWND save = CreateWindowExW(0, WC_BUTTONW, L"保存注释", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(0x6A03), gInstance, nullptr);
        for (HWND child : {gSaveList, gSaveDetails, gSaveNote, save}) if (child && gSaveFont)
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(gSaveFont), TRUE);
        RefreshBrowser(); ShowEntry(); LayoutBrowser(window);
        return 0;
    }
    case WM_SIZE: LayoutBrowser(window); return 0;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->idFrom == 0x6A00 &&
            reinterpret_cast<NMHDR*>(lParam)->code == LVN_ITEMCHANGED) { ShowEntry(); return 0; }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == 0x6A03 && HIWORD(wParam) == BN_CLICKED) {
            const int index = SelectedEntry();
            if (index >= 0) {
                const int length = GetWindowTextLengthW(gSaveNote);
                std::wstring note(static_cast<size_t>(length) + 1, L'\0');
                if (length) GetWindowTextW(gSaveNote, note.data(), length + 1);
                note.resize(static_cast<size_t>(length));
                EnsureFolder(gSaveRoot + L"\\patch");
                EnsureFolder(PatchPrivatePath(gSaveRoot, CompiledEngineProfile()));
                EnsureFolder(PatchPrivatePath(gSaveRoot, CompiledEngineProfile()) + L"\\save-index");
                WritePrivateProfileStringW(L"User", L"Note", note.c_str(),
                    gEntries[static_cast<size_t>(index)].indexPath.c_str());
                gEntries[static_cast<size_t>(index)].note = note;
            }
            return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        if (gPreviewBitmap) DeleteObject(gPreviewBitmap);
        gPreviewBitmap = nullptr;
        if (gSaveFont) DeleteObject(gSaveFont);
        gSaveFont = nullptr; gSaveList = gSaveDetails = gSaveNote = gSavePreview = nullptr; gBrowser = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterBrowser() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW klass = {sizeof(klass)};
    klass.lpfnWndProc = BrowserProc; klass.hInstance = gInstance;
    klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    klass.lpszClassName = L"XiaoAn66SaveBrowser";
    klass.hIcon = static_cast<HICON>(LoadImageW(gInstance, MAKEINTRESOURCEW(IDI_DORO_WHITE),
        IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    registered = true; return true;
}
}

bool InitializeSaveEnhancer(const wchar_t* gameRoot) {
    InterlockedExchange(&gEnhancerReady, FALSE);
    if (!gameRoot || !*gameRoot) return false;
    gSaveRoot = gameRoot;
    SaveLayout layout;
    if (!ResolveSaveLayout(layout)) return false;
    struct PendingPatch {
        BYTE* site;
        BYTE* original;
        BYTE* patched;
        size_t size;
        uintptr_t bridge;
        void** thunk;
        bool call;
    };
    PendingPatch patches[] = {
        {layout.extensionSaveSite,layout.originalExtensionSave,layout.patchedExtensionSave,6,
            reinterpret_cast<uintptr_t>(&SaveExtensionBridge),&layout.extensionSaveThunk,true},
        {layout.commitSite,layout.originalCommit,layout.patchedCommit,7,
            reinterpret_cast<uintptr_t>(&SaveCommitBridge),&layout.commitThunk,false},
        {layout.loadStartSite,layout.originalLoadStart,layout.patchedLoadStart,7,
            reinterpret_cast<uintptr_t>(&LoadStartBridge),&layout.loadStartThunk,false},
        {layout.ioSites[0],layout.originalIo[0].data(),layout.patchedIo[0].data(),5,
            reinterpret_cast<uintptr_t>(&SaveWriteBridge),&layout.ioThunks[0],true},
        {layout.ioSites[1],layout.originalIo[1].data(),layout.patchedIo[1].data(),5,
            reinterpret_cast<uintptr_t>(&SaveWriteBridge),&layout.ioThunks[1],true},
        {layout.ioSites[2],layout.originalIo[2].data(),layout.patchedIo[2].data(),5,
            reinterpret_cast<uintptr_t>(&SaveWriteBridge),&layout.ioThunks[2],true},
        {layout.ioSites[3],layout.originalIo[3].data(),layout.patchedIo[3].data(),5,
            reinterpret_cast<uintptr_t>(&SaveWriteBridge),&layout.ioThunks[3],true},
        {layout.ioSites[4],layout.originalIo[4].data(),layout.patchedIo[4].data(),5,
            reinterpret_cast<uintptr_t>(&SaveCloseBridge),&layout.ioThunks[4],true},
        {layout.callSite, layout.originalCall, layout.patchedCall, sizeof(layout.patchedCall),
            reinterpret_cast<uintptr_t>(&TurnBoundaryBridge), &layout.callThunk, true},
        {layout.saveResultSite, layout.originalSaveResult, layout.patchedSaveResult,
            sizeof(layout.patchedSaveResult), reinterpret_cast<uintptr_t>(&SaveResultBridge),
            &layout.saveResultThunk, false},
        {layout.loadObserveSite, layout.originalLoadObserve, layout.patchedLoadObserve,
            sizeof(layout.patchedLoadObserve), layout.loadObserverIsCall ?
                reinterpret_cast<uintptr_t>(&LoadFilename65Bridge) :
                reinterpret_cast<uintptr_t>(&LoadObserveBridge), &layout.loadObserveThunk,
            layout.loadObserverIsCall},
        {layout.loadResultSite, layout.originalLoadResult, layout.patchedLoadResult,
            sizeof(layout.patchedLoadResult), reinterpret_cast<uintptr_t>(&LoadResultBridge),
            &layout.loadResultThunk, false},
        {layout.saveFilenameSite, layout.originalSaveFilename, layout.patchedSaveFilename,
            sizeof(layout.patchedSaveFilename), reinterpret_cast<uintptr_t>(&SaveFilename65Bridge),
            &layout.saveFilenameThunk, true},
        {layout.endCallSite, layout.originalEndCall, layout.patchedEndCall, sizeof(layout.patchedEndCall),
            layout.endObserverIsSave ? reinterpret_cast<uintptr_t>(&TurnEndSaveBridge) :
                reinterpret_cast<uintptr_t>(&TurnEndBridge), &layout.endCallThunk, true}
    };
    bool prepared = true;
    for (auto& patch : patches) {
        if (!patch.site) continue;
        if (memcmp(patch.site, patch.original, patch.size) ||
            !PrepareJumpPatch(patch.site, patch.size, patch.bridge, patch.patched, *patch.thunk)) {
            prepared = false;
            break;
        }
        if (patch.call) patch.patched[0] = 0xE8;
    }
    if (!prepared) {
        for (auto& patch : patches) if (*patch.thunk) VirtualFree(*patch.thunk, 0, MEM_RELEASE);
        return false;
    }
    gSaveLayout = layout;
    gTransactionalHooks=true;
    gSaveCommitContinuation=reinterpret_cast<uintptr_t>(layout.commitSite+7);
    gSaveCommitFailure=layout.commitFailure;
    gSavePostWrite=layout.postWrite;
    gSaveLoadStartContinuation=reinterpret_cast<uintptr_t>(layout.loadStartSite+7);
    gSaveStatsLoadContinuation = layout.loadContinuation;
    gSaveNativeAuto = reinterpret_cast<uintptr_t>(layout.nativeAuto);
    gSaveNativeEndAuto = reinterpret_cast<uintptr_t>(layout.nativeEndAuto);
    gSaveNativeSave = reinterpret_cast<uintptr_t>(layout.nativeSave);
    gSaveEndGameFrameOffset = layout.endGameFrameOffset;
    gSaveNativeAutoEnable = layout.nativeAutoEnable;
    gSaveNativeEndAutoEnable = layout.nativeEndAutoEnable;
    size_t installed = 0;
    for (; installed < _countof(patches); ++installed) {
        auto& patch = patches[installed];
        if (patch.site && !WriteCode(patch.site, patch.patched, patch.size)) break;
    }
    if (installed != _countof(patches)) {
        RestoreSaveEnhancer();
        return false;
    }
    InterlockedExchange(&gEnhancerReady, TRUE);
    return true;
}

void PollSaveEnhancer() {
    // Native completion restores synchronously. Dispatch polling must never
    // overwrite a newer load or count initialization writes as battle events.
}

void RestoreSaveEnhancer() {
    // A foreign edit may prevent restoring one site.  Residual boundary
    // bridges must not start a private save after dependent hooks are gone.
    InterlockedExchange(&gEnhancerReady, FALSE);
    gLoadContexts.clear();
    EndSaveGroupRead();
    InterlockedExchange(&gPendingLoadSlot, -1);
    AbortSaveTransaction();
    if (!gSaveLayout.saveResultSite) return;
    struct RestorePatch {
        BYTE* site;
        BYTE* original;
        BYTE* patched;
        size_t size;
        void** thunk;
    };
    RestorePatch patches[] = {
        {gSaveLayout.extensionSaveSite,gSaveLayout.originalExtensionSave,gSaveLayout.patchedExtensionSave,6,&gSaveLayout.extensionSaveThunk},
        {gSaveLayout.commitSite,gSaveLayout.originalCommit,gSaveLayout.patchedCommit,7,&gSaveLayout.commitThunk},
        {gSaveLayout.loadStartSite,gSaveLayout.originalLoadStart,gSaveLayout.patchedLoadStart,7,&gSaveLayout.loadStartThunk},
        {gSaveLayout.ioSites[0],gSaveLayout.originalIo[0].data(),gSaveLayout.patchedIo[0].data(),5,&gSaveLayout.ioThunks[0]},
        {gSaveLayout.ioSites[1],gSaveLayout.originalIo[1].data(),gSaveLayout.patchedIo[1].data(),5,&gSaveLayout.ioThunks[1]},
        {gSaveLayout.ioSites[2],gSaveLayout.originalIo[2].data(),gSaveLayout.patchedIo[2].data(),5,&gSaveLayout.ioThunks[2]},
        {gSaveLayout.ioSites[3],gSaveLayout.originalIo[3].data(),gSaveLayout.patchedIo[3].data(),5,&gSaveLayout.ioThunks[3]},
        {gSaveLayout.ioSites[4],gSaveLayout.originalIo[4].data(),gSaveLayout.patchedIo[4].data(),5,&gSaveLayout.ioThunks[4]},
        {gSaveLayout.callSite, gSaveLayout.originalCall, gSaveLayout.patchedCall,
            sizeof(gSaveLayout.originalCall), &gSaveLayout.callThunk},
        {gSaveLayout.saveResultSite, gSaveLayout.originalSaveResult, gSaveLayout.patchedSaveResult,
            sizeof(gSaveLayout.originalSaveResult), &gSaveLayout.saveResultThunk},
        {gSaveLayout.loadObserveSite, gSaveLayout.originalLoadObserve, gSaveLayout.patchedLoadObserve,
            sizeof(gSaveLayout.originalLoadObserve), &gSaveLayout.loadObserveThunk},
        {gSaveLayout.loadResultSite, gSaveLayout.originalLoadResult, gSaveLayout.patchedLoadResult,
            sizeof(gSaveLayout.originalLoadResult), &gSaveLayout.loadResultThunk},
        {gSaveLayout.saveFilenameSite, gSaveLayout.originalSaveFilename, gSaveLayout.patchedSaveFilename,
            sizeof(gSaveLayout.originalSaveFilename), &gSaveLayout.saveFilenameThunk},
        {gSaveLayout.endCallSite, gSaveLayout.originalEndCall, gSaveLayout.patchedEndCall,
            sizeof(gSaveLayout.originalEndCall), &gSaveLayout.endCallThunk}
    };
    bool restored = true;
    for (size_t i = _countof(patches); i > 0; --i) {
        auto& patch = patches[i - 1];
        if (!patch.site) continue;
        if (!RestoreSavePatch(patch.site, patch.original, patch.patched, patch.size)) {
            restored = false;
            continue;
        }
        if (*patch.thunk) { VirtualFree(*patch.thunk, 0, MEM_RELEASE); *patch.thunk = nullptr; }
    }
    if (restored) {
        gTransactionalHooks=false;
        gSaveCommitContinuation=gSaveCommitFailure=gSavePostWrite=gSaveLoadStartContinuation=0;
        gSaveLayout = SaveLayout(); gSaveStatsLoadContinuation = 0;
        gSaveNativeAuto = gSaveNativeEndAuto = gSaveNativeSave = 0;
        gSaveNativeAutoEnable = gSaveNativeEndAutoEnable = nullptr;
    }
    gLoadContexts.clear();
    EndSaveGroupRead();
    InterlockedExchange(&gPendingLoadSlot, -1);
}

bool OpenSaveBrowser(HWND owner, const wchar_t* gameRoot) {
    if (gBrowser && IsWindow(gBrowser)) { ShowWindow(gBrowser, SW_RESTORE); SetForegroundWindow(gBrowser); return true; }
    if (!gameRoot || !*gameRoot || !RegisterBrowser()) return false;
    gSaveRoot = gameRoot;
    RECT ownerRect = {}; GetWindowRect(owner, &ownerRect);
    gBrowser = CreateWindowExW(WS_EX_TOOLWINDOW, L"XiaoAn66SaveBrowser", L"存档浏览",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, ownerRect.left + 30, ownerRect.top + 30,
        850, 520, owner, nullptr, gInstance, nullptr);
    if (!gBrowser) return false;
    ShowWindow(gBrowser, SW_SHOW); UpdateWindow(gBrowser); return true;
}

void CloseSaveBrowser() {
    if (gBrowser && IsWindow(gBrowser)) DestroyWindow(gBrowser);
    gBrowser = nullptr;
}

bool IsSaveBrowserWindow(HWND window) {
    return window && gBrowser && (window == gBrowser || IsChild(gBrowser, window));
}

bool HandleSaveBrowserMessage(MSG* message) {
    if (!message || !IsSaveBrowserWindow(message->hwnd)) return false;
    return IsDialogMessageW(gBrowser, message) != FALSE;
}

bool ResolveSaveEnhancerImageForTest(BYTE* image, uintptr_t virtualBase,
    SaveEnhancerImageLayout& output) {
    output = SaveEnhancerImageLayout();
    SaveLayout layout;
    if (!ResolveSaveLayoutInImage(image, virtualBase, layout)) return false;
    const auto address = [image, virtualBase](const void* pointer) -> uintptr_t {
        return pointer ? virtualBase + (reinterpret_cast<const BYTE*>(pointer) - image) : 0;
    };
    output.nativeAuto = address(reinterpret_cast<const void*>(layout.nativeAuto));
    output.nativeSave = address(reinterpret_cast<const void*>(layout.nativeSave));
    output.nativeEndAuto = address(reinterpret_cast<const void*>(layout.nativeEndAuto));
    output.startCall = address(layout.callSite);
    output.endCall = address(layout.endCallSite);
    output.nativeFilename = address(reinterpret_cast<const void*>(layout.nativeFilename));
    output.saveFilenameCall = address(layout.saveFilenameSite);
    output.saveResult = address(layout.saveResultSite);
    output.loadObserve = address(layout.loadObserveSite);
    output.loadResult = address(layout.loadResultSite);
    output.campaignByte = address(layout.campaignByte);
    output.loadContinuation = address(reinterpret_cast<const void*>(layout.loadContinuation));
    output.commitSite=address(layout.commitSite);
    output.commitContinuation=address(layout.commitSite ? layout.commitSite+7 : nullptr);
    output.commitFailure=address(reinterpret_cast<const void*>(layout.commitFailure));
    output.nativePostWrite=address(reinterpret_cast<const void*>(layout.postWrite));
    output.loadStartSite=address(layout.loadStartSite);
    output.loadStartContinuation=address(layout.loadStartSite ? layout.loadStartSite+7 : nullptr);
    output.nativeWrite=address(reinterpret_cast<const void*>(layout.nativeWrite));
    output.nativeClose=address(reinterpret_cast<const void*>(layout.nativeClose));
    return true;
}

void ConfigureSaveEnhancerForTest(const wchar_t* gameRoot, BYTE* campaign,
    uintptr_t nativeSave, uintptr_t nativeFilename) {
    gTransactionalHooks=false;
    gLoadContexts.clear();
    gSaveRoot = gameRoot ? gameRoot : L"";
    gSaveLayout = SaveLayout();
    gSaveLayout.campaignByte = campaign;
    gSaveLayout.nativeSave = reinterpret_cast<NativeSaveFn>(nativeSave);
    gSaveLayout.nativeFilename = reinterpret_cast<NativeFilenameFn>(nativeFilename);
    InterlockedExchange(&gEnhancerReady, gameRoot && nativeSave ? TRUE : FALSE);
}

void SavePrivateForTest(void* game, bool atStart) { SavePrivate(game, atStart); }
void RouteSaveFilenameForTest(char* output, int rawSlot) { SaveFilename65Bridge(output, rawSlot); }
void CompleteSaveForTest(int rawSlot) { FinishSaveSlot(rawSlot); }
int EffectiveSaveSlotForTest(int rawSlot) { return EffectiveSaveSlot(rawSlot); }

void ConfigureSaveEnhancerBypassForTest(BYTE* first, BYTE* second, WORD firstPatch, WORD secondPatch) {
    gSaveLayout.nativeBypassSites[0] = first;
    gSaveLayout.nativeBypassSites[1] = second;
    gSaveLayout.nativeBypassPatched[0] = firstPatch;
    gSaveLayout.nativeBypassPatched[1] = secondPatch;
}

uintptr_t ConfigureSaveBoundaryBridgeForTest(bool start, bool nativeSaveCall, uintptr_t native) {
    if (start) { gSaveNativeAuto = native; gSaveNativeAutoEnable = nullptr; return reinterpret_cast<uintptr_t>(&TurnBoundaryBridge); }
    if (nativeSaveCall) {
        gSaveNativeSave = native; gSaveEndGameFrameOffset = -8;
        return reinterpret_cast<uintptr_t>(&TurnEndSaveBridge);
    }
    gSaveNativeEndAuto = native;
    gSaveNativeEndAutoEnable = nullptr;
    return reinterpret_cast<uintptr_t>(&TurnEndBridge);
}

void ConfigureSaveBoundaryEnableForTest(bool start, BYTE* enabled) {
    if (start) gSaveNativeAutoEnable = enabled;
    else gSaveNativeEndAutoEnable = enabled;
}

void ConfigureSaveLoadBridgesForTest(uintptr_t continuation, uintptr_t& start, uintptr_t& result) {
    gSaveLoadStartContinuation=continuation;
    start=reinterpret_cast<uintptr_t>(&LoadStartBridge);
    result=reinterpret_cast<uintptr_t>(&LoadResultBridge);
}
void RouteLoadFilenameForTest(char* output, int rawSlot) { LoadFilename65Bridge(output,rawSlot); }
void ObserveSaveLoadForTest(int rawSlot) { BeginLoadSlot(rawSlot); }
void CompleteSaveLoadForTest(bool succeeded) { FinishLoadSlot(succeeded ? 1:0); }

uintptr_t ConfigureSaveCommitBridgeForTest(uintptr_t post,uintptr_t continuation,uintptr_t failure) {
    gSavePostWrite=post; gSaveCommitContinuation=continuation; gSaveCommitFailure=failure;
    return reinterpret_cast<uintptr_t>(&SaveCommitBridge);
}
uintptr_t SaveResultBridgeForTest() { return reinterpret_cast<uintptr_t>(&SaveResultBridge); }
void SetSaveTransactionalHooksForTest(bool enabled) { gTransactionalHooks=enabled; }
#ifdef XA66_TEST
void SetSaveFailureDialogForTest(bool enabled) { gSaveFailureDialogForTest=enabled; }
#endif
