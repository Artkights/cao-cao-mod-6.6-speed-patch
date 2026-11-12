#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum class EngineProfileId : unsigned { Unknown = 0, Ccz64 = 64, Ccz65 = 65, Ccz66 = 66 };
enum class EnginePlanStatus : unsigned char { Unresolved, AutoVerified, VersionSelected, Rejected };

struct EngineDetection {
    EngineProfileId profile = EngineProfileId::Unknown;
    std::wstring exeSha256;
    std::wstring evidence;
    std::wstring planReport;
    // Protected220 is selected only by an exact audited disk fingerprint.
    // Its runtime image must pass the normal layout resolver before features
    // are enabled.
    bool requiresRuntimeValidation = false;
    // The user accepted selecting the legacy ABI from the fixed version
    // resource (or an explicit version when that resource is unavailable).
    // This is distinct from successful automatic layout verification.
    bool versionFallback = false;
    std::wstring variant;
    unsigned confidence = 0;
    std::wstring container = L"Standard";
    std::wstring layoutStatus = L"Unresolved";
    bool packedContainer = false;
    EnginePlanStatus planStatus = EnginePlanStatus::Unresolved;
};

inline EngineProfileId CompiledEngineProfile() {
#ifdef CCZ_ENGINE64
    return EngineProfileId::Ccz64;
#elif defined(CCZ_ENGINE65)
    return EngineProfileId::Ccz65;
#else
    return EngineProfileId::Ccz66;
#endif
}

inline const wchar_t* EngineProfileName(EngineProfileId profile) {
    return profile == EngineProfileId::Ccz64 ? L"6.4" :
        profile == EngineProfileId::Ccz65 ? L"6.5" :
        profile == EngineProfileId::Ccz66 ? L"6.6" : L"未知版本";
}

inline const wchar_t* PatchPrivateRelative(EngineProfileId profile) {
    return profile == EngineProfileId::Ccz64 ? L"patch\\xiaoan64" :
        profile == EngineProfileId::Ccz65 ? L"patch\\xiaoan65" :
        profile == EngineProfileId::Ccz66 ? L"patch\\xiaoan66" : L"";
}

inline std::wstring PatchPrivatePath(const std::wstring& root, EngineProfileId profile) {
    const wchar_t* relative = PatchPrivateRelative(profile);
    return *relative ? root + L"\\" + relative : std::wstring();
}

// Both APIs inspect data only; they do not load or execute the target program.
bool DetectEngineProfile(const std::wstring& root, EngineDetection& detection, std::wstring& error);
bool DetectEngineProfileImage(const unsigned char* image, size_t size, uintptr_t virtualBase,
    EngineDetection& detection, std::wstring& error);
// Uses VirtualQuery to capture readable pages and independently verifies live
// permissions. Only this entry point accepts validated RuntimeOnly sites.
bool DetectEngineProfileRuntime(const unsigned char* image, size_t sizeHint,
    EngineDetection& detection, std::wstring& error);

// Select an ABI without requiring code signatures, file-addressable patch
// sites or a protected-file fingerprint. The caller must show the fallback
// warning before installation and preserve backups. A requested profile is
// used only if no supported fixed version can be read; it never silently
// overrides a different version recorded in the executable.
bool DetectVersionEngineProfile(const std::wstring& root, EngineDetection& detection,
    std::wstring& error, EngineProfileId requestedProfile = EngineProfileId::Unknown);
