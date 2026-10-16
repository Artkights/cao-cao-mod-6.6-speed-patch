#include "SaveEnhancer.h"

#include "Features.h"
#include "Guide.h"
#include "InstallCommon.h"
#include "ResourceIds.h"

#include <algorithm>
#include <commctrl.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using NativeAutoFn = void (__thiscall*)(void*);
using NativeSaveFn = void (__thiscall*)(void*, int);

struct SaveLayout {
    BYTE* callSite = nullptr;
    NativeAutoFn nativeAuto = nullptr;
    NativeSaveFn nativeSave = nullptr;
    BYTE originalCall[5] = {};
    BYTE patchedCall[5] = {};
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

bool ResolveSaveLayout(SaveLayout& result) {
    BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
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
            p[9] == 0x74 && p[10] == 0x20 && p[11] == 0xB8 && p[16] == 0x50 &&
            p[17] == 0xFF && p[18] == 0x30 && p[19] == 0xC6 && p[20] == 0x00 && p[21] == 0x2C &&
            p[22] == 0x0F && p[23] == 0xB6 && p[24] == 0x41 && p[25] == 0x02 &&
            p[26] == 0x6A && p[27] == 0x14 && p[28] == 0xE8 && p[33] == 0x52 && p[34] == 0xE8) {
            nativeAuto = p;
            ++autoMatches;
        }
    }
    if (autoMatches != 1 || !nativeAuto) return false;
    const uintptr_t nativeSave = CallTarget(nativeAuto + 34);
    if (!nativeSave || nativeSave < reinterpret_cast<uintptr_t>(base) ||
        nativeSave >= reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage) return false;
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
    result = SaveLayout();
    result.callSite = callSite;
    result.nativeAuto = reinterpret_cast<NativeAutoFn>(nativeAuto);
    result.nativeSave = reinterpret_cast<NativeSaveFn>(nativeSave);
    memcpy(result.originalCall, callSite, sizeof(result.originalCall));
    return true;
}

bool WriteCode(BYTE* address, const BYTE* bytes, size_t size) {
    DWORD protection = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &protection)) return false;
    memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD ignored = 0;
    return VirtualProtect(address, size, protection, &ignored) != FALSE;
}

void* NearTarget(BYTE* instruction, uintptr_t target) {
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
            return thunk;
        }
    }
    return nullptr;
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

void MoveSavedFile(const std::wstring& source, const std::wstring& target) {
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool NativeSaveSafe(void* game, int slot) {
    bool saved = false;
    __try { gSaveLayout.nativeSave(game, slot); saved = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { saved = false; }
    return saved;
}

void SavePrivate(void* game, bool atStart) {
    if (!game || !gSaveLayout.nativeSave || InterlockedCompareExchange(&gSaving, TRUE, FALSE)) return;
    BYTE turn = 0;
    if (!ReadSafe(static_cast<const BYTE*>(game) + 2, &turn, 1)) {
        InterlockedExchange(&gSaving, FALSE);
        return;
    }
    const int slots = Bound(gFeatureSettings.autoSaveSlots, 1, 100);
    const int rotating = turn % slots;
    const int nativeSlot = (atStart ? 800 : 900) + rotating;
    if (!NativeSaveSafe(game, nativeSlot)) { InterlockedExchange(&gSaving, FALSE); return; }
    const std::wstring privateRoot = gSaveRoot + L"\\patch\\xiaoan66";
    const std::wstring folder = privateRoot + L"\\autosave";
    EnsureFolder(gSaveRoot + L"\\patch"); EnsureFolder(privateRoot); EnsureFolder(folder);
    wchar_t nativeName[64] = {}, privateName[64] = {};
    swprintf_s(nativeName, L"\\SV\\SV%03d", nativeSlot);
    swprintf_s(privateName, L"\\%s-%03d", atStart ? L"start" : L"end", rotating);
    const std::wstring sourceBase = gSaveRoot + nativeName;
    const std::wstring targetBase = folder + privateName;
    MoveSavedFile(sourceBase + L".E5S", targetBase + L".E5S");
    MoveSavedFile(sourceBase + L".EXS", targetBase + L".EXS");
    if (GetFileAttributesW((targetBase + L".E5S").c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring scenario;
        GetObservedGuideScenario(scenario);
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
    }
    InterlockedExchange(&gSaving, FALSE);
}

void __cdecl TurnBoundaryHelper(void* game) {
    if (gFeatureSettings.autoSaveTurnEnd) SavePrivate(game, false);
    gSaveLayout.nativeAuto(game);
    if (gFeatureSettings.autoSaveTurnStart) SavePrivate(game, true);
}

__declspec(naked) void TurnBoundaryBridge() {
    __asm {
        push ecx
        call TurnBoundaryHelper
        add esp, 4
        ret
    }
}

std::wstring ReadIni(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
    wchar_t value[1024] = {};
    GetPrivateProfileStringW(section, key, L"", value, static_cast<DWORD>(_countof(value)), path.c_str());
    return value;
}

void AddAutosaves() {
    const std::wstring pattern = gSaveRoot + L"\\patch\\xiaoan66\\autosave\\*.ini";
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        SaveEntry entry;
        entry.automatic = true;
        entry.indexPath = gSaveRoot + L"\\patch\\xiaoan66\\autosave\\" + data.cFileName;
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
    const std::wstring indexFolder = gSaveRoot + L"\\patch\\xiaoan66\\save-index";
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
    const std::wstring folder = gSaveRoot + L"\\patch\\xiaoan66\\records";
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
                EnsureFolder(gSaveRoot + L"\\patch\\xiaoan66");
                EnsureFolder(gSaveRoot + L"\\patch\\xiaoan66\\save-index");
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
    if (!gameRoot || !*gameRoot) return false;
    gSaveRoot = gameRoot;
    SaveLayout layout;
    if (!ResolveSaveLayout(layout)) return false;
    void* target = NearTarget(layout.callSite, reinterpret_cast<uintptr_t>(&TurnBoundaryBridge));
    if (!target) return false;
    layout.patchedCall[0] = 0xE8;
    const int32_t displacement = static_cast<int32_t>(reinterpret_cast<BYTE*>(target) - (layout.callSite + 5));
    memcpy(layout.patchedCall + 1, &displacement, sizeof(displacement));
    if (memcmp(layout.callSite, layout.originalCall, sizeof(layout.originalCall)) ||
        !WriteCode(layout.callSite, layout.patchedCall, sizeof(layout.patchedCall))) return false;
    gSaveLayout = layout;
    return true;
}

void RestoreSaveEnhancer() {
    if (!gSaveLayout.callSite) return;
    if (!memcmp(gSaveLayout.callSite, gSaveLayout.patchedCall, sizeof(gSaveLayout.patchedCall)))
        WriteCode(gSaveLayout.callSite, gSaveLayout.originalCall, sizeof(gSaveLayout.originalCall));
    gSaveLayout = SaveLayout();
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
