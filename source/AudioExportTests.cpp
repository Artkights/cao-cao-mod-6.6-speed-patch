#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static_assert(sizeof(void*) == 4, "Audio export verification must run in x86.");

namespace {
const char* names[] = {
    "CDAudioClose", "CDAudioEjectMedia", "CDAudioGetDriveLetter", "CDAudioGetNumDrives",
    "CDAudioGetNumTracks", "CDAudioGetPlayingPos", "CDAudioGetStartPos", "CDAudioGetTrackLength",
    "CDAudioInitialize", "CDAudioIsInserted", "CDAudioIsPlaying", "CDAudioNextPlay",
    "CDAudioNextPlayTrack", "CDAudioOpen", "CDAudioPause", "CDAudioPlay", "CDAudioPlayTrack",
    "CDAudioResume", "CDAudioStop", "CDAudioTerminate"
};

bool InspectExports(HMODULE module, bool forwarded, const char* label) {
    const BYTE* image = reinterpret_cast<const BYTE*>(module);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const IMAGE_NT_HEADERS32* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        printf("FAIL %s: expected x86 PE32 image\n", label);
        return false;
    }
    const IMAGE_DATA_DIRECTORY directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || !directory.Size) {
        printf("FAIL %s: missing export directory\n", label);
        return false;
    }
    const IMAGE_EXPORT_DIRECTORY* exports =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image + directory.VirtualAddress);
    if (exports->Base != 1 || exports->NumberOfFunctions != 20 || exports->NumberOfNames != 20) {
        printf("FAIL %s: base=%lu functions=%lu names=%lu\n", label,
            exports->Base, exports->NumberOfFunctions, exports->NumberOfNames);
        return false;
    }
    const DWORD* nameOffsets = reinterpret_cast<const DWORD*>(image + exports->AddressOfNames);
    const WORD* nameOrdinals = reinterpret_cast<const WORD*>(image + exports->AddressOfNameOrdinals);
    const DWORD* functions = reinterpret_cast<const DWORD*>(image + exports->AddressOfFunctions);
    for (size_t i = 0; i < ARRAYSIZE(names); ++i) {
        bool found = false;
        for (DWORD n = 0; n < exports->NumberOfNames; ++n) {
            if (strcmp(reinterpret_cast<const char*>(image + nameOffsets[n]), names[i])) continue;
            if (nameOrdinals[n] + exports->Base != i + 1) {
                printf("FAIL %s: %s has ordinal %lu\n", label, names[i], nameOrdinals[n] + exports->Base);
                return false;
            }
            found = true;
            break;
        }
        if (!found) {
            printf("FAIL %s: missing %s\n", label, names[i]);
            return false;
        }
        const DWORD entry = functions[i];
        const bool isForwarder = entry >= directory.VirtualAddress &&
            entry - directory.VirtualAddress < directory.Size;
        if (forwarded) {
            char expected[128];
            sprintf_s(expected, "Koeicda_org.%s", names[i]);
            if (!isForwarder || strcmp(reinterpret_cast<const char*>(image + entry), expected)) {
                printf("FAIL %s: %s is not the expected forwarder\n", label, names[i]);
                return false;
            }
        } else if (!entry || isForwarder) {
            printf("FAIL %s: %s is not an original implementation\n", label, names[i]);
            return false;
        }
    }
    printf("PASS %s: x86 PE32, exact 20 names and ordinals 1..20\n", label);
    return true;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: AudioExportTests.exe <patch directory>\n");
        return 2;
    }
    wchar_t originalPath[32768], proxyPath[32768];
    swprintf_s(originalPath, L"%s\\Koeicda_org.dll", argv[1]);
    swprintf_s(proxyPath, L"%s\\Koeicda.dll", argv[1]);
    HMODULE original = LoadLibraryExW(originalPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!original) {
        printf("FAIL: loading packaged original, Win32 error %lu\n", GetLastError());
        return 1;
    }
    HMODULE proxy = LoadLibraryExW(proxyPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!proxy) {
        printf("FAIL: loading packaged proxy, Win32 error %lu\n", GetLastError());
        return 1;
    }
    // Retain both modules until process exit; the proxy starts an initialization worker.
    Sleep(300);
    if (!InspectExports(original, false, "Koeicda_org") || !InspectExports(proxy, true, "Koeicda")) return 1;
    for (size_t i = 0; i < ARRAYSIZE(names); ++i) {
        FARPROC expected = GetProcAddress(original, names[i]);
        FARPROC byName = GetProcAddress(proxy, names[i]);
        FARPROC byOrdinal = GetProcAddress(proxy, MAKEINTRESOURCEA(static_cast<WORD>(i + 1)));
        FARPROC originalOrdinal = GetProcAddress(original, MAKEINTRESOURCEA(static_cast<WORD>(i + 1)));
        if (!expected || expected != byName || expected != byOrdinal || expected != originalOrdinal) {
            printf("FAIL ordinal %u %s: original=%p name=%p ordinal=%p originalOrdinal=%p\n",
                static_cast<unsigned>(i + 1), names[i], expected, byName, byOrdinal, originalOrdinal);
            return 1;
        }
        printf("PASS ordinal %02u %-24s name/ordinal resolve to original %p\n",
            static_cast<unsigned>(i + 1), names[i], expected);
    }
    printf("PASS: all 20 proxy audio entries resolve to the packaged original implementation.\n");
    return 0;
}
