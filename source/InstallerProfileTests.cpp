#include "InstallCommon.h"
#include "ResourceIds.h"

#include <stdio.h>
#include <string>
#include <vector>

namespace {
int failures = 0;
HMODULE installer = nullptr;
std::wstring workspace;

void Check(bool ok, const char* message, const std::wstring& error = {}) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) {
        ++failures;
        if (!error.empty()) fwprintf(stderr, L"  %s\n", error.c_str());
    }
}

std::wstring Hash(const std::wstring& path) {
    std::wstring value, error;
    return XiaoAnInstall::Sha256File(path, value, error) ? value : std::wstring();
}

std::vector<unsigned char> Read(const std::wstring& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") || !file) return {};
    std::vector<unsigned char> bytes;
    unsigned char buffer[4096];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
        bytes.insert(bytes.end(), buffer, buffer + count);
    fclose(file);
    return bytes;
}

bool Write(const std::wstring& path, const std::vector<unsigned char>& bytes) {
    std::wstring error;
    return XiaoAnInstall::WriteBytes(path, bytes, error);
}

std::wstring Fixture(const std::wstring& name, const wchar_t* exe, const wchar_t* audio) {
    const std::wstring root = XiaoAnInstall::JoinPath(workspace, name);
    if (!CreateDirectoryW(root.c_str(), nullptr) ||
        !CopyFileW(exe, XiaoAnInstall::JoinPath(root, L"Ekd5.exe").c_str(), TRUE) ||
        !CopyFileW(audio, XiaoAnInstall::JoinPath(root, L"Koeicda.dll").c_str(), TRUE)) return {};
    return root;
}

bool Payload(const std::wstring& root, XiaoAnInstall::PackagePayload& payload) {
    std::wstring error;
    EngineDetection engine;
    if (!DetectEngineProfile(root, engine, error)) return false;
    payload.profile = engine.profile;
    payload.expectedExeSha256 = engine.exeSha256;
    const WORD proxy = engine.profile == EngineProfileId::Ccz65 ? IDR_INSTALL_PROXY65 : IDR_INSTALL_PROXY;
    return XiaoAnInstall::ReadResourceBytes(installer, proxy, payload.proxy, error) &&
        XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_GEAR, payload.gear, error) &&
        XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_CONFIG, payload.defaultConfig, error) &&
        XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_FEATURES, payload.defaultFeatures, error);
}

bool Install(const std::wstring& root, const XiaoAnInstall::PackagePayload& payload, std::wstring& error) {
    std::wstring detail, report;
    const auto kind = XiaoAnInstall::DetectTarget(root, detail);
    return XiaoAnInstall::InstallPatch(root, payload, kind, report, error);
}

void ExerciseProfile(EngineProfileId profile, const wchar_t* exe, const wchar_t* otherExe,
                     const wchar_t* originalAudio) {
    using namespace XiaoAnInstall;
    const std::wstring suffix = std::to_wstring(static_cast<unsigned>(profile));
    const std::wstring root = Fixture(L"clean-" + suffix, exe, originalAudio);
    Check(!root.empty(), "create isolated game fixture");
    if (root.empty()) return;
    const std::wstring audioPath = JoinPath(root, L"Koeicda.dll");
    const std::wstring originalHash = Hash(audioPath);
    const std::vector<unsigned char> shared = {'s', 'h', 'a', 'r', 'e', 'd', '\n'};
    Check(Write(JoinPath(root, L"patch\\pl_06.ini"), shared), "create shared configuration sentinel");
    const FILETIME fixedTime = {0x12340000, 0x01D90000};
    HANDLE metadata = CreateFileW(audioPath.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool timestamps = metadata != INVALID_HANDLE_VALUE &&
        SetFileTime(metadata, &fixedTime, &fixedTime, &fixedTime);
    if (metadata != INVALID_HANDLE_VALUE) CloseHandle(metadata);
    Check(timestamps && SetFileAttributesW(audioPath.c_str(), FILE_ATTRIBUTE_READONLY),
          "set original file metadata for restoration checks");
    WIN32_FILE_ATTRIBUTE_DATA originalMetadata = {};
    GetFileAttributesExW(audioPath.c_str(), GetFileExInfoStandard, &originalMetadata);

    std::wstring error, detail, report;
    Check(DetectTarget(root, detail) == TargetKind::Clean, "detect a clean supported version", detail);
    PackagePayload payload;
    Check(Payload(root, payload) && payload.profile == profile, "select the matching embedded proxy");
    if (payload.proxy.empty()) return;
    PackagePayload wrong = payload;
    wrong.profile = profile == EngineProfileId::Ccz65 ? EngineProfileId::Ccz66 : EngineProfileId::Ccz65;
    Check(!Install(root, wrong, error) && Hash(audioPath) == originalHash &&
        !FileExists(ManifestPath(root, profile)), "reject a payload from the other engine before writing");
    wrong = payload;
    wrong.expectedExeSha256.assign(64, L'0');
    Check(!Install(root, wrong, error) && Hash(audioPath) == originalHash,
          "reject a changed executable fingerprint before writing");

    const std::wstring preexistingGear = JoinPath(root, L"GearNtKe.dll");
    const std::vector<unsigned char> gearSentinel = {'o', 'l', 'd'};
    Check(Write(preexistingGear, gearSentinel), "create a preexisting DLL for a fresh-install rollback");
    HANDLE lockedOriginalGear = CreateFileW(preexistingGear.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(lockedOriginalGear != INVALID_HANDLE_VALUE && !Install(root, payload, error) &&
        Hash(audioPath) == originalHash &&
        GetFileAttributesW(audioPath.c_str()) == originalMetadata.dwFileAttributes &&
        Read(preexistingGear) == gearSentinel && !FileExists(ManifestPath(root, profile)),
        "a failed fresh install restores both original bytes and read-only attributes");
    if (lockedOriginalGear != INVALID_HANDLE_VALUE) CloseHandle(lockedOriginalGear);
    DeleteFileW(preexistingGear.c_str());

    Check(Install(root, payload, error), "install the matching version transactionally", error);
    Manifest first;
    Check(LoadManifest(root, first, error) && first.profile == profile,
          "write a version-specific schema 1 manifest", error);
    const std::wstring privateRoot = PatchPrivatePath(root, profile);
    const std::wstring configPath = JoinPath(privateRoot, L"pl_06.ini");
    const std::wstring featuresPath = JoinPath(privateRoot, L"features.ini");
    std::vector<unsigned char> customConfig = payload.defaultConfig;
    if (!customConfig.empty()) customConfig[0] = customConfig[0] == '0' ? '1' : '0';
    const std::vector<unsigned char> customFeatures = {'[', 'U', 's', 'e', 'r', ']', '\n'};
    Check(Write(configPath, customConfig) && Write(featuresPath, customFeatures),
          "customize private user configuration");
    const std::wstring backupHash = Hash(JoinPath(root, first.files[0].backup));
    Check(Install(root, payload, error), "update an installed matching version", error);
    Manifest updated;
    Check(LoadManifest(root, updated, error) && updated.installId == first.installId &&
        Hash(JoinPath(root, updated.files[0].backup)) == backupHash &&
        Read(configPath) == customConfig && Read(featuresPath) == customFeatures &&
        Read(JoinPath(root, L"patch\\pl_06.ini")) == shared,
        "update preserves first backup, install identity, and all user configuration", error);

    const std::wstring manifestBeforeFailure = Hash(ManifestPath(root, profile));
    const std::wstring proxyBeforeFailure = Hash(audioPath);
    const std::wstring gearPath = JoinPath(root, L"GearNtKe.dll");
    HANDLE lockedGear = CreateFileW(gearPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(lockedGear != INVALID_HANDLE_VALUE && !Install(root, payload, error) &&
        Hash(audioPath) == proxyBeforeFailure &&
        Hash(ManifestPath(root, profile)) == manifestBeforeFailure && Read(configPath) == customConfig,
        "a locked final DLL rolls back earlier replacements and preserves the manifest");
    if (lockedGear != INVALID_HANDLE_VALUE) CloseHandle(lockedGear);

    if (profile == EngineProfileId::Ccz66) {
        Check(WritePrivateProfileStringW(L"Install", L"EngineProfile", nullptr,
            ManifestPath(root, profile).c_str()) && LoadManifest(root, updated, error) &&
            updated.profile == EngineProfileId::Ccz66 && Install(root, payload, error),
            "upgrade a historical 6.6 schema 1 manifest without an engine field", error);
    }

    const std::wstring backupPath = JoinPath(root, first.files[0].backup);
    const std::vector<unsigned char> originalBackup = Read(backupPath);
    std::vector<unsigned char> damagedBackup = originalBackup;
    if (!damagedBackup.empty()) damagedBackup.back() ^= 1;
    Check(SetFileAttributesW(backupPath.c_str(), FILE_ATTRIBUTE_NORMAL) &&
        Write(backupPath, damagedBackup) && !RestorePatch(root, report, error, true) &&
        Hash(audioPath) == proxyBeforeFailure && FileExists(ManifestPath(root, profile)),
        "a damaged original backup prevents uninstall without replacing runtime files");
    Check(Write(backupPath, originalBackup), "restore the isolated backup fixture for subsequent checks");

    const EngineProfileId other = profile == EngineProfileId::Ccz65 ? EngineProfileId::Ccz66 : EngineProfileId::Ccz65;
    Check(Write(ManifestPath(root, other), Read(ManifestPath(root, profile))) &&
        !LoadManifest(root, updated, error) && DetectTarget(root, detail) == TargetKind::Invalid,
        "reject simultaneous 6.5 and 6.6 manifests");
    DeleteFileW(ManifestPath(root, other).c_str());

    const std::wstring deployedProxyHash = Hash(audioPath);
    Check(CopyFileW(otherExe, JoinPath(root, L"Ekd5.exe").c_str(), FALSE) != FALSE,
          "replace only the fixture executable to simulate a version conflict");
    PackagePayload otherPayload;
    Check(Payload(root, otherPayload) && !Install(root, otherPayload, error) &&
        Hash(audioPath) == deployedProxyHash,
        "refuse an update when the game family differs from its original manifest");
    Check(DeleteFileW(JoinPath(root, L"Ekd5.exe").c_str()) &&
        DetectTarget(root, detail) == TargetKind::Installed,
        "keep the emergency uninstall available without Ekd5.exe");
    Check(RestorePatch(root, report, error, true), "uninstall without a game executable", error);
    WIN32_FILE_ATTRIBUTE_DATA restored = {};
    GetFileAttributesExW(audioPath.c_str(), GetFileExInfoStandard, &restored);
    Check(Hash(audioPath) == originalHash &&
        restored.dwFileAttributes == originalMetadata.dwFileAttributes &&
        CompareFileTime(&restored.ftCreationTime, &originalMetadata.ftCreationTime) == 0 &&
        CompareFileTime(&restored.ftLastWriteTime, &originalMetadata.ftLastWriteTime) == 0 &&
        !FileExists(JoinPath(root, L"Koeicda_org.dll")) && !FileExists(JoinPath(root, L"GearNtKe.dll")) &&
        !FileExists(ManifestPath(root, profile)) && Read(JoinPath(root, L"patch\\pl_06.ini")) == shared,
        "uninstall restores original bytes, metadata, and original file existence");

    const std::wstring legacy = Fixture(L"legacy-" + suffix, exe, originalAudio);
    if (legacy.empty()) { Check(false, "create legacy fixture"); return; }
    PackagePayload legacyPayload;
    Check(Payload(legacy, legacyPayload) &&
        CopyFileW(originalAudio, JoinPath(legacy, L"Koeicda_org.dll").c_str(), TRUE) &&
        Write(JoinPath(legacy, L"Koeicda.dll"), legacyPayload.proxy) &&
        Write(JoinPath(legacy, L"GearNtKe.dll"), legacyPayload.gear) &&
        Write(JoinPath(legacy, L"patch\\pl_06.ini"), legacyPayload.defaultConfig),
        "create a loose legacy installation fixture");
    Check(DetectTarget(legacy, detail) == TargetKind::Legacy && Install(legacy, legacyPayload, error),
          "adopt a matching legacy loose installation", error);
    Check(Read(JoinPath(PatchPrivatePath(legacy, profile), L"pl_06.ini")) == legacyPayload.defaultConfig &&
        RestorePatch(legacy, report, error, true) && Hash(JoinPath(legacy, L"Koeicda.dll")) == originalHash,
        "legacy migration imports shared configuration and restores original audio", error);

    const std::wstring existing = Fixture(L"preexisting-dlls-" + suffix, exe, originalAudio);
    if (existing.empty()) { Check(false, "create preexisting DLL fixture"); return; }
    const std::vector<unsigned char> existingOrg = {'u', 's', 'e', 'r', '-', 'o', 'r', 'g'};
    const std::vector<unsigned char> existingGear = {'u', 's', 'e', 'r', '-', 'g', 'e', 'a', 'r'};
    PackagePayload existingPayload;
    Check(Payload(existing, existingPayload) &&
        Write(JoinPath(existing, L"Koeicda_org.dll"), existingOrg) &&
        Write(JoinPath(existing, L"GearNtKe.dll"), existingGear) && Install(existing, existingPayload, error),
        "install a clean game that already has auxiliary DLL files", error);
    Check(RestorePatch(existing, report, error, true) &&
        Read(JoinPath(existing, L"Koeicda_org.dll")) == existingOrg &&
        Read(JoinPath(existing, L"GearNtKe.dll")) == existingGear &&
        Hash(JoinPath(existing, L"Koeicda.dll")) == originalHash,
        "uninstall preserves all three original DLL files when they existed before installation", error);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 6) {
        fprintf(stderr, "Usage: InstallerProfileTests <installer> <exe65> <exe66> <original-audio> <new-work-directory>\n");
        return 2;
    }
    workspace = XiaoAnInstall::CanonicalRoot(argv[5]);
    if (workspace.empty() || XiaoAnInstall::DirectoryExists(workspace) || XiaoAnInstall::FileExists(workspace) ||
        !CreateDirectoryW(workspace.c_str(), nullptr)) {
        fprintf(stderr, "The test work directory must be new and its parent must exist.\n");
        return 2;
    }
    installer = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE);
    Check(installer != nullptr, "open installer resources without executing them");
    if (!installer) return 1;
    ExerciseProfile(EngineProfileId::Ccz65, argv[2], argv[3], argv[4]);
    ExerciseProfile(EngineProfileId::Ccz66, argv[3], argv[2], argv[4]);
    const std::wstring unknown = Fixture(L"unknown-engine", argv[1], argv[4]);
    std::wstring detail;
    Check(!unknown.empty() && XiaoAnInstall::DetectTarget(unknown, detail) == XiaoAnInstall::TargetKind::Invalid &&
        !XiaoAnInstall::DirectoryExists(XiaoAnInstall::JoinPath(unknown, L"patch")),
        "reject a valid x86 PE32 with no supported engine identity before writing");
    FreeLibrary(installer);
    printf("InstallerProfileTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
