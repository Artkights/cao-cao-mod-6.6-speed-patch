#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum class EngineProfileId : unsigned { Unknown = 0, Ccz64 = 64, Ccz65 = 65, Ccz66 = 66 };

struct EngineDetection {
    EngineProfileId profile = EngineProfileId::Unknown;
    std::wstring exeSha256;
    std::wstring evidence;
    std::wstring planReport;
    // A valid PE with an unrecognized call graph may be shown to the user as
    // a force-injection candidate.  This flag never authorizes a write by
    // itself; the caller still needs an explicit EnginePatchPlan.
    bool forceRequired = false;
    bool forced = false;
    unsigned confidence = 0;
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

bool DetectForcedEngineProfile(const std::wstring& root, EngineProfileId selectedProfile,
    EngineDetection& detection, std::wstring& error);
bool DetectForcedEngineProfileImage(const unsigned char* image, size_t size, uintptr_t virtualBase,
    EngineProfileId selectedProfile, EngineDetection& detection, std::wstring& error);
