#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "EngineProfile.h"
#include "Version.h"

namespace XiaoAnInstall {

constexpr unsigned kManifestSchema = 2;
constexpr wchar_t kPackageVersion[] = XIAOAN_VERSION_WSTRING;

// The proxy is shared by all optional functions.  The mask describes which
// functions the installer should activate; the audio forwarding core remains
// present while any bit is selected.
enum FeatureMask : unsigned {
    FeatureNone = 0,
    FeatureAccelerator = 1u << 0,
    FeatureGuide = 1u << 1,
    FeatureBattleStats = 1u << 2,
    FeatureAll = FeatureAccelerator | FeatureGuide | FeatureBattleStats
};

inline bool IsValidFeatureMask(unsigned mask) {
    return (mask & ~static_cast<unsigned>(FeatureAll)) == 0;
}

enum class InstallMode {
    Clean,
    LegacyAdopted,
    LegacySnapshot
};

enum class TargetKind {
    Invalid,
    Clean,
    Legacy,
    Installed
};

struct FileRecord {
    std::wstring path;
    bool existed = false;
    std::wstring backup;
    uint64_t size = 0;
    std::wstring sha256;
    DWORD attributes = 0;
    uint64_t creationTime = 0;
    uint64_t accessTime = 0;
    uint64_t writeTime = 0;
};

// Installation, engine layout and live module readiness are independent.
// These additive fields do not change installation schema 2 or stats schema 4.
struct RuntimeStatus {
    std::wstring container = L"Unknown";
    std::wstring layout = L"Unknown";
    std::wstring startup = L"Pending";
    std::wstring accelerator = L"Pending";
    std::wstring guide = L"Pending";
    std::wstring stats = L"Pending";
    std::wstring detail;
};

struct Manifest {
    unsigned schemaVersion = kManifestSchema;
    EngineProfileId profile = EngineProfileId::Unknown;
    std::wstring packageVersion;
    std::wstring installId;
    InstallMode mode = InstallMode::Clean;
    std::wstring root;
    unsigned featureMask = FeatureAll;
    bool forced = false;
    // Version-selected installations are explicitly acknowledged fallbacks.
    // They still use the same transactional backup and ownership checks as
    // automatic installations; this bit only records the user's choice.
    bool versionFallback = false;
    std::wstring engineVariant = L"Standard";
    std::wstring runtimeValidation = L"NotRequired";
    RuntimeStatus status;
    std::wstring executableSha256;
    std::wstring planSha256;
    bool patchDirExisted = false;
    bool privateDirExisted = false;
    FileRecord files[3];
    // Additional originals for a version-selected install. Ekd5.exe is a
    // copy-only recovery snapshot; private configuration records are restored.
    std::vector<FileRecord> versionSnapshots;
    bool restoreVersionConfigs = false;
    std::wstring proxySha256;
    std::wstring gearSha256;
    std::wstring originalCopySha256;
};

struct PackagePayload {
    EngineProfileId profile = EngineProfileId::Unknown;
    std::wstring expectedExeSha256;
    std::wstring engineVariant = L"Standard";
    std::wstring runtimeValidation = L"NotRequired";
    RuntimeStatus status;
    bool versionFallback = false;
    std::vector<unsigned char> proxy;
    std::vector<unsigned char> gear;
    std::vector<unsigned char> defaultConfig;
    std::vector<unsigned char> defaultFeatures;
};

std::wstring JoinPath(const std::wstring& left, const std::wstring& right);
std::wstring CanonicalRoot(const std::wstring& path);
std::wstring ManifestPath(const std::wstring& root);
std::wstring ManifestPath(const std::wstring& root, EngineProfileId profile);
bool FileExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
bool IsGameRunning(const std::wstring& root, std::wstring& error);

bool Sha256File(const std::wstring& path, std::wstring& hash, std::wstring& error);
bool Sha256Bytes(const std::vector<unsigned char>& bytes, std::wstring& hash, std::wstring& error);
bool ValidatePe32Executable(const std::wstring& path, std::wstring& error);
bool ValidateOriginalAudio(const std::wstring& path, std::wstring& error);
bool ValidateProxyAudio(const std::wstring& path, std::wstring& error);
bool ValidateGear(const std::wstring& path, std::wstring& error);
bool ValidateProxyAudioBytes(const std::vector<unsigned char>& bytes, std::wstring& error);
bool ValidateGearBytes(const std::vector<unsigned char>& bytes, std::wstring& error);
bool ValidateGearPayloadBytes(const std::vector<unsigned char>& bytes, std::wstring& error);

TargetKind DetectTarget(const std::wstring& root, std::wstring& detail,
                        bool allowVersionFallback = false,
                        EngineProfileId requestedProfile = EngineProfileId::Unknown,
                        const EngineDetection* verifiedDetection = nullptr,
                        EngineDetection* resolvedDetection = nullptr);
bool LoadManifest(const std::wstring& root, Manifest& manifest, std::wstring& error);
bool UpdateRuntimeValidation(const std::wstring& root, EngineProfileId profile,
    const std::wstring& state, std::wstring& error);
bool UpdateRuntimeStatus(const std::wstring& root, EngineProfileId profile,
    const RuntimeStatus& status, std::wstring& error);
bool ValidateUninstall(const std::wstring& root, Manifest& manifest, std::wstring& error,
                       bool requireInstalledProxy = true);
bool InstallPatch(const std::wstring& root, const PackagePayload& payload,
                  TargetKind requestedKind, std::wstring& report, std::wstring& error,
                  unsigned featureMask = FeatureAll,
                  const EngineDetection* verifiedDetection = nullptr);
bool RestorePatch(const std::wstring& root, std::wstring& report, std::wstring& error,
                  bool requireInstalledProxy = true,
                  unsigned removeFeatures = FeatureAll);

bool ReadResourceBytes(HMODULE module, WORD id, std::vector<unsigned char>& bytes,
                       std::wstring& error);
bool WriteBytes(const std::wstring& path, const std::vector<unsigned char>& bytes,
                std::wstring& error);

} // namespace XiaoAnInstall
