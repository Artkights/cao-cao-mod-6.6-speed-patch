#include <windows.h>
#include <stdio.h>
#include "ResourceIds.h"

#pragma pack(push, 2)
struct GroupIconDirectory {
    WORD reserved;
    WORD type;
    WORD count;
};

struct GroupIconEntry {
    BYTE width;
    BYTE height;
    BYTE colorCount;
    BYTE reserved;
    WORD planes;
    WORD bitCount;
    DWORD bytesInResource;
    WORD resourceId;
};
#pragma pack(pop)

namespace {
int failures = 0;

void Check(bool condition, const wchar_t* file, const wchar_t* description) {
    if (condition) return;
    ++failures;
    fwprintf(stderr, L"FAIL [%ls]: %ls (error %lu)\n", file, description, GetLastError());
}

void CheckIconHandle(HMODULE module, const wchar_t* file, int width, int height) {
    SetLastError(ERROR_SUCCESS);
    HICON icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(IDI_DORO_WHITE),
        IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
    Check(icon != nullptr, file, L"LoadImageW could not load the embedded icon");
    if (!icon) return;

    ICONINFO info = {};
    SetLastError(ERROR_SUCCESS);
    Check(GetIconInfo(icon, &info) != FALSE, file, L"GetIconInfo rejected the loaded icon");
    if (info.hbmMask) DeleteObject(info.hbmMask);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    DestroyIcon(icon);
}

void CheckModule(const wchar_t* file) {
    HMODULE module = LoadLibraryExW(file, nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    Check(module != nullptr, file, L"LoadLibraryExW failed");
    if (!module) return;

    SetLastError(ERROR_SUCCESS);
    HRSRC groupResource = FindResourceW(module, MAKEINTRESOURCEW(IDI_DORO_WHITE), RT_GROUP_ICON);
    Check(groupResource != nullptr, file, L"RT_GROUP_ICON resource 101 is missing");
    if (groupResource) {
        HGLOBAL loaded = LoadResource(module, groupResource);
        const DWORD resourceSize = SizeofResource(module, groupResource);
        const auto* directory = static_cast<const GroupIconDirectory*>(LockResource(loaded));
        Check(directory != nullptr, file, L"icon group data could not be locked");
        if (directory) {
            Check(directory->reserved == 0 && directory->type == 1,
                file, L"icon group header is invalid");
            Check(directory->count == 6, file, L"icon group does not contain all six images");
            const size_t requiredSize = sizeof(GroupIconDirectory) +
                static_cast<size_t>(directory->count) * sizeof(GroupIconEntry);
            Check(resourceSize >= requiredSize, file, L"icon group resource is truncated");
            if (resourceSize >= requiredSize) {
                const auto* entries = reinterpret_cast<const GroupIconEntry*>(directory + 1);
                for (WORD i = 0; i < directory->count; ++i) {
                    Check(entries[i].resourceId != 0 &&
                        FindResourceW(module, MAKEINTRESOURCEW(entries[i].resourceId), RT_ICON) != nullptr,
                        file, L"icon group references a missing RT_ICON image");
                }
            }
        }
    }

    CheckIconHandle(module, file, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    CheckIconHandle(module, file, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    FreeLibrary(module);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        fwprintf(stderr, L"Usage: IconResourceTests <installer> <proxy>\n");
        return 2;
    }
    CheckModule(argv[1]);
    CheckModule(argv[2]);
    wprintf(L"IconResourceTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
