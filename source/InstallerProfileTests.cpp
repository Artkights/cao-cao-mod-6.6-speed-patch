#include "EngineLayout.h"
#include "InstallCommon.h"
#include "ResourceIds.h"

#include <stdio.h>
#include <initializer_list>
#include <cstring>
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
    const WORD proxy = engine.profile == EngineProfileId::Ccz64 ? IDR_INSTALL_PROXY64 :
        engine.profile == EngineProfileId::Ccz65 ? IDR_INSTALL_PROXY65 : IDR_INSTALL_PROXY;
    return XiaoAnInstall::ReadResourceBytes(installer, proxy, payload.proxy, error) &&
        XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_GEAR, payload.gear, error) &&
        XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_CONFIG, payload.defaultConfig, error) &&
        XiaoAnInstall::ReadResourceBytes(installer, IDR_INSTALL_FEATURES, payload.defaultFeatures, error);
}

bool Install(const std::wstring& root, const XiaoAnInstall::PackagePayload& payload, std::wstring& error,
    unsigned mask = XiaoAnInstall::FeatureAll) {
    std::wstring detail, report;
    const auto kind = XiaoAnInstall::DetectTarget(root, detail);
    return XiaoAnInstall::InstallPatch(root, payload, kind, report, error, mask);
}

bool ModifiedIdentity(std::vector<unsigned char>& raw) {
    if (raw.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > raw.size()) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage > 64u * 1024u * 1024u ||
        nt->OptionalHeader.SizeOfHeaders > raw.size()) return false;
    std::vector<unsigned char> mapped(nt->OptionalHeader.SizeOfImage, 0);
    memcpy(mapped.data(), raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        if (section.PointerToRawData > raw.size() || section.SizeOfRawData > raw.size() - section.PointerToRawData ||
            section.VirtualAddress > mapped.size() || section.SizeOfRawData > mapped.size() - section.VirtualAddress)
            return false;
        memcpy(mapped.data() + section.VirtualAddress, raw.data() + section.PointerToRawData, section.SizeOfRawData);
    }
    EngineLayout layout;
    if (!ResolveEngineLayout(mapped.data(), layout, nt->OptionalHeader.ImageBase) || !layout.waitWrapper) return false;
    const uintptr_t rva = layout.waitWrapper - 0x400000;
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        if (rva >= section.VirtualAddress && rva - section.VirtualAddress < section.SizeOfRawData) {
            const size_t offset = section.PointerToRawData + static_cast<size_t>(rva - section.VirtualAddress);
            if (offset >= raw.size()) return false;
            raw[offset] = 0x90;
            return true;
        }
    }
    return false;
}

void ExerciseForcedInstallation(EngineProfileId profile, const wchar_t* exe, const wchar_t* originalAudio) {
    using namespace XiaoAnInstall;
    const std::wstring suffix = std::to_wstring(static_cast<unsigned>(profile));
    const std::wstring root = Fixture(L"forced-" + suffix, exe, originalAudio);
    PackagePayload payload;
    std::wstring error, detail, report;
    if (root.empty() || !Payload(root, payload)) { Check(false, "create forced-install fixture"); return; }
    const std::wstring audio = JoinPath(root, L"Koeicda.dll");
    const std::wstring executable = JoinPath(root, L"Ekd5.exe");
    const std::wstring originalAudioHash = Hash(audio);
    std::vector<unsigned char> modified = Read(executable);
    const bool changed = ModifiedIdentity(modified) && Write(executable, modified);
    EngineDetection automatic, forced;
    Check(changed && !DetectEngineProfile(root, automatic, error) &&
        DetectTarget(root, detail) == TargetKind::Invalid && Hash(audio) == originalAudioHash,
        "modified wait-wrapper identity is rejected by automatic installation without file changes", error);
    const bool resolved = DetectForcedEngineProfile(root, profile, forced, error);
    Check(resolved && forced.forced && forced.profile == profile && forced.exeSha256 == Hash(executable) &&
        !forced.planReport.empty(), "explicit family resolves an auditable forced-install plan", error);
    if (!resolved) return;
    payload.forced = true;
    payload.expectedExeSha256 = forced.exeSha256;
    const TargetKind kind = DetectTarget(root, detail, profile);
    Check(kind == TargetKind::Clean && InstallPatch(root, payload, kind, report, error, FeatureAll),
        "forced installation accepts only the explicitly selected family and matching executable fingerprint", error);
    Manifest manifest;
    const std::wstring privateRoot = PatchPrivatePath(root, profile);
    const std::wstring state = JoinPath(privateRoot, L"feature-state.ini");
    wchar_t mode[32] = {}, stateExe[80] = {};
    GetPrivateProfileStringW(L"Install", L"EngineMode", L"", mode, static_cast<DWORD>(_countof(mode)), state.c_str());
    GetPrivateProfileStringW(L"Install", L"ExecutableSha256", L"", stateExe, static_cast<DWORD>(_countof(stateExe)), state.c_str());
    Check(LoadManifest(root, manifest, error) && manifest.forced && manifest.profile == profile &&
        manifest.executableSha256 == forced.exeSha256 && manifest.planSha256.size() == 64 &&
        manifest.planSha256 == Hash(JoinPath(privateRoot, L"engine-plan.txt")) &&
        !_wcsicmp(mode, L"Forced") && !_wcsicmp(stateExe, forced.exeSha256.c_str()) &&
        GetPrivateProfileIntW(L"Install", L"EngineProfile", 0, state.c_str()) == static_cast<unsigned>(profile) &&
        Read(executable) == modified,
        "forced manifest and runtime state retain family, executable and plan hashes without modifying the executable", error);
    Check(RestorePatch(root, report, error, true) && Hash(audio) == originalAudioHash &&
        Read(executable) == modified && !FileExists(ManifestPath(root, profile)) &&
        !FileExists(JoinPath(root, L"Koeicda_org.dll")) && !FileExists(JoinPath(root, L"GearNtKe.dll")),
        "forced uninstall restores the original DLL baseline and leaves the executable byte-identical", error);

    EngineDetection rejected;
    Check(!DetectForcedEngineProfile(root, EngineProfileId::Unknown, rejected, error),
        "forced detection rejects an unspecified template family", error);
    const std::wstring boundary = Fixture(L"forced-boundary-" + suffix, exe, originalAudio);
    if (boundary.empty()) { Check(false, "create forced boundary fixture"); return; }
    const std::wstring boundaryExe = JoinPath(boundary, L"Ekd5.exe");
    std::vector<unsigned char> shortImage(64, 0);
    Check(Write(boundaryExe, shortImage) && !DetectForcedEngineProfile(boundary, profile, rejected, error) &&
        DetectTarget(boundary, detail, profile) == TargetKind::Invalid &&
        Hash(JoinPath(boundary, L"Koeicda.dll")) == originalAudioHash &&
        !FileExists(ManifestPath(boundary, profile)),
        "forced mode rejects truncated PE boundaries before creating a manifest or changing DLLs", error);
    std::vector<unsigned char> nonX86 = Read(exe);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(nonX86.data());
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(nonX86.data() + dos->e_lfanew);
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    Check(Write(boundaryExe, nonX86) && !DetectForcedEngineProfile(boundary, profile, rejected, error) &&
        Hash(JoinPath(boundary, L"Koeicda.dll")) == originalAudioHash &&
        !FileExists(ManifestPath(boundary, profile)),
        "forced mode rejects non-x86 PE targets without changing the audio baseline", error);
}

void ExerciseFeatureMatrix(EngineProfileId profile, const wchar_t* exe, const wchar_t* originalAudio) {
    using namespace XiaoAnInstall;
    const std::wstring suffix = std::to_wstring(static_cast<unsigned>(profile));
    const std::vector<unsigned char> gearSentinel = {'u','s','e','r','-','g','e','a','r'};
    const std::vector<unsigned char> userSentinel = {'u','s','e','r','-','d','a','t','a'};
    std::wstring error, report;
    for (unsigned mask = FeatureNone; mask <= FeatureAll; ++mask) {
        const std::wstring root = Fixture(L"mask-" + suffix + L"-" + std::to_wstring(mask), exe, originalAudio);
        Check(!root.empty(), "create feature-mask matrix fixture");
        if (root.empty()) continue;
        PackagePayload payload;
        if (!Payload(root, payload)) { Check(false, "load feature-mask matrix payload"); continue; }
        const std::wstring audio = JoinPath(root, L"Koeicda.dll");
        const std::wstring originalHash = Hash(audio);
        const std::wstring gear = JoinPath(root, L"GearNtKe.dll");
        Check(Write(gear, gearSentinel), "seed an unrelated original gear file");
        if (mask == FeatureNone) {
            Check(!Install(root, payload, error, mask) && Hash(audio) == originalHash &&
                Read(gear) == gearSentinel && !FileExists(ManifestPath(root, profile)),
                "zero-feature installation is rejected without changing original files", error);
            continue;
        }
        const bool installed = Install(root, payload, error, mask);
        Check(installed, "install each of the seven nonzero feature combinations", error);
        if (!installed) continue;
        Manifest manifest;
        const std::wstring privateRoot = PatchPrivatePath(root, profile);
        const std::wstring config = JoinPath(privateRoot, L"pl_06.ini");
        const std::wstring state = JoinPath(privateRoot, L"feature-state.ini");
        Check(LoadManifest(root, manifest, error) && manifest.schemaVersion == kManifestSchema &&
            manifest.featureMask == mask &&
            GetPrivateProfileIntW(L"Install", L"FeatureMask", 0, state.c_str()) == mask &&
            ((mask & FeatureAccelerator) ? (Read(gear) == payload.gear && FileExists(config)) :
                (Read(gear) == gearSentinel && !FileExists(config))),
            "manifest, runtime feature state, and accelerator files agree for every mask", error);
        const std::wstring installId = manifest.installId;
        const std::wstring backupHash = Hash(JoinPath(root, manifest.files[0].backup));
        const std::wstring addedFile = JoinPath(privateRoot, L"user-notes\\keep.dat");
        Check(Write(addedFile, userSentinel), "create a user-owned private file after installation");
        unsigned remaining = mask;
        for (unsigned bit : {static_cast<unsigned>(FeatureAccelerator), static_cast<unsigned>(FeatureGuide),
            static_cast<unsigned>(FeatureBattleStats)}) {
            if (!(remaining & bit)) continue;
            remaining &= ~bit;
            const bool removed = RestorePatch(root, report, error, true, bit);
            Check(removed, "uninstall one selected feature from every combination", error);
            if (!removed) break;
            if (remaining) {
                Check(LoadManifest(root, manifest, error) && manifest.featureMask == remaining &&
                    manifest.installId == installId && Hash(JoinPath(root, manifest.files[0].backup)) == backupHash &&
                    Hash(audio) == manifest.proxySha256 &&
                    ((remaining & FeatureAccelerator) ? Read(gear) == payload.gear : Read(gear) == gearSentinel),
                    "partial removal preserves the baseline and all remaining feature dependencies", error);
            } else {
                Check(Hash(audio) == originalHash && Read(gear) == gearSentinel &&
                    !FileExists(ManifestPath(root, profile)) && !FileExists(JoinPath(root, L"Koeicda_org.dll")) &&
                    Read(addedFile) == userSentinel,
                    "last-feature removal restores original DLLs and preserves user-owned private files", error);
            }
        }
    }

    const std::wstring historical = Fixture(L"schema1-" + suffix, exe, originalAudio);
    PackagePayload historicalPayload;
    Manifest upgraded;
    const bool historicalInstalled = !historical.empty() && Payload(historical, historicalPayload) &&
        Install(historical, historicalPayload, error);
    Check(historicalInstalled, "create a historical schema-one migration fixture", error);
    if (historicalInstalled) {
        const std::wstring manifest = ManifestPath(historical, profile);
        const bool modified = WritePrivateProfileStringW(L"Install", L"SchemaVersion", L"1", manifest.c_str()) &&
            WritePrivateProfileStringW(L"Install", L"FeatureMask", nullptr, manifest.c_str()) &&
            DeleteFileW(JoinPath(PatchPrivatePath(historical, profile), L"feature-state.ini").c_str());
        Check(modified && LoadManifest(historical, upgraded, error) && upgraded.featureMask == FeatureAll &&
            Install(historical, historicalPayload, error) && LoadManifest(historical, upgraded, error) &&
            upgraded.schemaVersion == kManifestSchema && upgraded.featureMask == FeatureAll,
            "schema-one installation migrates to an explicit all-feature schema-two manifest", error);
        Check(RestorePatch(historical, report, error, true), "uninstall migrated schema-one fixture", error);
    }

    const std::wstring conflict = Fixture(L"replaced-owned-dll-" + suffix, exe, originalAudio);
    PackagePayload conflictPayload;
    const bool conflictInstalled = !conflict.empty() && Payload(conflict, conflictPayload) &&
        Install(conflict, conflictPayload, error);
    Check(conflictInstalled, "create replacement-conflict fixture", error);
    if (conflictInstalled) {
        const std::wstring audio = JoinPath(conflict, L"Koeicda.dll");
        const std::wstring proxyHash = Hash(audio);
        const std::wstring manifestHash = Hash(ManifestPath(conflict, profile));
        for (const wchar_t* name : {L"GearNtKe.dll", L"Koeicda_org.dll"}) {
            const std::wstring path = JoinPath(conflict, name);
            const std::vector<unsigned char> deployed = Read(path);
            Check(Write(path, userSentinel) && !RestorePatch(conflict, report, error, true) &&
                Hash(audio) == proxyHash && Hash(ManifestPath(conflict, profile)) == manifestHash &&
                Read(path) == userSentinel,
                "manual replacement of an owned DLL prevents uninstall without overwriting user changes", error);
            Check(Write(path, deployed), "restore owned DLL fixture after conflict check");
        }
        Check(RestorePatch(conflict, report, error, true), "uninstall conflict fixture after restoring owned DLLs", error);
    }
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
          "write a version-specific manifest", error);
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

    Check(RestorePatch(root, report, error, true, FeatureGuide),
        "partially uninstall the selected guide feature", error);
    Manifest reduced;
    Check(LoadManifest(root, reduced, error) &&
        reduced.featureMask == (FeatureAll & ~FeatureGuide) &&
        FileExists(JoinPath(root, L"GearNtKe.dll")) && FileExists(configPath) &&
        Read(JoinPath(privateRoot, L"feature-state.ini")).size() > 0,
        "partial uninstall keeps the shared proxy and accelerator dependencies", error);
    Check(Install(root, payload, error), "reinstall a previously removed feature", error);

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
    Check(Payload(legacy, legacyPayload), "load the loose legacy fixture payload");
    std::vector<unsigned char> legacyProxy = legacyPayload.proxy;
    std::vector<unsigned char> legacyGear = legacyPayload.gear;
    const std::vector<unsigned char> legacyMarker = {'l', 'e', 'g', 'a', 'c', 'y'};
    legacyProxy.insert(legacyProxy.end(), legacyMarker.begin(), legacyMarker.end());
    legacyGear.insert(legacyGear.end(), legacyMarker.begin(), legacyMarker.end());
    Check(!legacyProxy.empty() && !legacyGear.empty() &&
        CopyFileW(originalAudio, JoinPath(legacy, L"Koeicda_org.dll").c_str(), TRUE) &&
        Write(JoinPath(legacy, L"Koeicda.dll"), legacyProxy) &&
        Write(JoinPath(legacy, L"GearNtKe.dll"), legacyGear) &&
        Write(JoinPath(legacy, L"patch\\pl_06.ini"), legacyPayload.defaultConfig),
        "create a loose legacy installation fixture");
    const std::wstring legacyProxyHash = Hash(JoinPath(legacy, L"Koeicda.dll"));
    const std::wstring legacyOrgHash = Hash(JoinPath(legacy, L"Koeicda_org.dll"));
    const std::wstring legacyGearHash = Hash(JoinPath(legacy, L"GearNtKe.dll"));
    Check(DetectTarget(legacy, detail) == TargetKind::Legacy && Install(legacy, legacyPayload, error),
          "snapshot a matching legacy loose installation", error);
    Manifest legacyManifest;
    Check(Read(JoinPath(PatchPrivatePath(legacy, profile), L"pl_06.ini")) == legacyPayload.defaultConfig &&
        LoadManifest(legacy, legacyManifest, error) && legacyManifest.mode == InstallMode::LegacySnapshot &&
        legacyManifest.files[0].existed && legacyManifest.files[0].sha256 == legacyProxyHash &&
        legacyManifest.files[1].existed && legacyManifest.files[1].sha256 == legacyOrgHash &&
        legacyManifest.files[2].existed && legacyManifest.files[2].sha256 == legacyGearHash &&
        legacyManifest.originalCopySha256 == legacyOrgHash &&
        Hash(JoinPath(legacy, L"Koeicda_org.dll")) == legacyOrgHash,
        "legacy migration records all three pre-upgrade DLLs and forwards audio through the original audio DLL", error);
    Check(Install(legacy, legacyPayload, error) && RestorePatch(legacy, report, error, true) &&
        Hash(JoinPath(legacy, L"Koeicda.dll")) == legacyProxyHash &&
        Hash(JoinPath(legacy, L"Koeicda_org.dll")) == legacyOrgHash &&
        Hash(JoinPath(legacy, L"GearNtKe.dll")) == legacyGearHash &&
        Read(JoinPath(legacy, L"patch\\pl_06.ini")) == legacyPayload.defaultConfig,
        "legacy update and uninstall restore all three DLL hashes from immediately before the upgrade", error);

    const std::wstring historicalLegacy = Fixture(L"historical-legacy-" + suffix, exe, originalAudio);
    PackagePayload historicalLegacyPayload;
    const bool historicalLegacyInstalled = !historicalLegacy.empty() && Payload(historicalLegacy, historicalLegacyPayload) &&
        Install(historicalLegacy, historicalLegacyPayload, error);
    Check(historicalLegacyInstalled, "create a historical adopted-legacy manifest fixture", error);
    if (historicalLegacyInstalled) {
        const std::wstring historicalManifest = ManifestPath(historicalLegacy, profile);
        const std::wstring historicalState = JoinPath(PatchPrivatePath(historicalLegacy, profile), L"feature-state.ini");
        Check(WritePrivateProfileStringW(L"Install", L"SchemaVersion", L"1", historicalManifest.c_str()) &&
            WritePrivateProfileStringW(L"Install", L"InstallMode", L"LegacyAdopted", historicalManifest.c_str()) &&
            WritePrivateProfileStringW(L"Install", L"FeatureMask", nullptr, historicalManifest.c_str()) &&
            DeleteFileW(historicalState.c_str()) && LoadManifest(historicalLegacy, legacyManifest, error) &&
            legacyManifest.mode == InstallMode::LegacyAdopted && legacyManifest.featureMask == FeatureAll &&
            Install(historicalLegacy, historicalLegacyPayload, error) &&
            LoadManifest(historicalLegacy, legacyManifest, error) && legacyManifest.mode == InstallMode::LegacyAdopted &&
            RestorePatch(historicalLegacy, report, error, true) &&
            Hash(JoinPath(historicalLegacy, L"Koeicda.dll")) == originalHash &&
            !FileExists(JoinPath(historicalLegacy, L"Koeicda_org.dll")) &&
            !FileExists(JoinPath(historicalLegacy, L"GearNtKe.dll")),
            "historical LegacyAdopted manifests retain their recorded original-audio baseline through update and uninstall", error);
    }

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
    if (argc != 7) {
        fprintf(stderr, "Usage: InstallerProfileTests <installer> <exe64> <exe65> <exe66> <original-audio> <new-work-directory>\n");
        return 2;
    }
    workspace = XiaoAnInstall::CanonicalRoot(argv[6]);
    if (workspace.empty() || XiaoAnInstall::DirectoryExists(workspace) || XiaoAnInstall::FileExists(workspace) ||
        !CreateDirectoryW(workspace.c_str(), nullptr)) {
        fprintf(stderr, "The test work directory must be new and its parent must exist.\n");
        return 2;
    }
    installer = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE);
    Check(installer != nullptr, "open installer resources without executing them");
    if (!installer) return 1;
    ExerciseProfile(EngineProfileId::Ccz64, argv[2], argv[3], argv[5]);
    ExerciseProfile(EngineProfileId::Ccz65, argv[3], argv[4], argv[5]);
    ExerciseProfile(EngineProfileId::Ccz66, argv[4], argv[3], argv[5]);
    ExerciseFeatureMatrix(EngineProfileId::Ccz64, argv[2], argv[5]);
    ExerciseFeatureMatrix(EngineProfileId::Ccz65, argv[3], argv[5]);
    ExerciseFeatureMatrix(EngineProfileId::Ccz66, argv[4], argv[5]);
    ExerciseForcedInstallation(EngineProfileId::Ccz64, argv[2], argv[5]);
    ExerciseForcedInstallation(EngineProfileId::Ccz65, argv[3], argv[5]);
    ExerciseForcedInstallation(EngineProfileId::Ccz66, argv[4], argv[5]);
    const std::wstring unknown = Fixture(L"unknown-engine", argv[1], argv[4]);
    std::wstring detail;
    Check(!unknown.empty() && XiaoAnInstall::DetectTarget(unknown, detail) == XiaoAnInstall::TargetKind::Invalid &&
        !XiaoAnInstall::DirectoryExists(XiaoAnInstall::JoinPath(unknown, L"patch")),
        "reject a valid x86 PE32 with no supported engine identity before writing");
    FreeLibrary(installer);
    printf("InstallerProfileTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
