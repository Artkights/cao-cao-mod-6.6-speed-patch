#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum class EngineProfileId : unsigned { Unknown = 0, Ccz65 = 65, Ccz66 = 66 };

struct EngineDetection {
    EngineProfileId profile = EngineProfileId::Unknown;
    std::wstring exeSha256;
    std::wstring evidence;
};

inline EngineProfileId CompiledEngineProfile() {
#ifdef CCZ_ENGINE65
    return EngineProfileId::Ccz65;
#else
    return EngineProfileId::Ccz66;
#endif
}

inline const wchar_t* EngineProfileName(EngineProfileId profile) {
    return profile == EngineProfileId::Ccz65 ? L"6.5" :
        profile == EngineProfileId::Ccz66 ? L"6.6" : L"未知版本";
}

inline const wchar_t* PatchPrivateRelative(EngineProfileId profile) {
    return profile == EngineProfileId::Ccz65 ? L"patch\\xiaoan65" :
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
