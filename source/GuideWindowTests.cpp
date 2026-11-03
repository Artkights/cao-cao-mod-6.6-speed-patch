#include "Guide.h"
#include "EngineProfile.h"

#include <richedit.h>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// Build this console test with production Guide.cpp and BattleRuntime.cpp.
// It creates its own owner window and minimal EEX fixtures; no game is needed.
Settings gSettings;
HMODULE gInstance = nullptr;
uintptr_t gGameBase = 0;
HWND gMainWindow = nullptr;
HWND gDialog = nullptr;

namespace {

constexpr UINT kScenario = 0x6703;
constexpr UINT kExport = 0x6704;
constexpr UINT kFontSize = 0x6705;
constexpr UINT kLineSpacing = 0x6706;
constexpr DWORD kReadTimeout = 5000;
constexpr DWORD kExportTimeout = 10000;
constexpr wchar_t kHandledDialog[] = L"GuideWindowTestsHandled";

enum class ConfirmPolicy { Unexpected, Accept, Cancel };

struct ModalState {
    ConfirmPolicy policy = ConfirmPolicy::Unexpected;
    bool expectFailure = false;
    int confirmations = 0;
    int results = 0;
    int errors = 0;
};

int gFailures = 0;
HWND gGuide = nullptr;
ModalState gModal;

void PrintUiState(const char* reason);

bool Check(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    std::fflush(stdout);
    if (!condition) {
        ++gFailures;
        PrintUiState("Check failure");
    }
    return condition;
}

void AppendU16(std::vector<unsigned char>& bytes, unsigned value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xffu));
    bytes.push_back(static_cast<unsigned char>((value >> 8) & 0xffu));
}

void AppendI32(std::vector<unsigned char>& bytes, int value) {
    const unsigned converted = static_cast<unsigned>(value);
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<unsigned char>((converted >> shift) & 0xffu));
}

std::vector<unsigned char> RewardScenario(int money) {
    std::vector<unsigned char> body;
    AppendU16(body, 0x59);
    const int values[] = {money, 255, 0, 255, 0, 255, 0, 0};
    for (size_t index = 0; index < _countof(values); ++index) {
        AppendU16(body, 0);
        if (index == 0) AppendI32(body, values[index]);
        else AppendU16(body, static_cast<unsigned>(values[index]));
    }
    std::vector<unsigned char> bytes(10, 0);
    bytes[0] = 'E'; bytes[1] = 'E'; bytes[2] = 'X';
    AppendI32(bytes, 14);
    AppendU16(bytes, 1);
    AppendU16(bytes, static_cast<unsigned>(4 + body.size()));
    AppendU16(bytes, 0);
    AppendU16(bytes, static_cast<unsigned>(body.size()));
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

bool WriteBytes(const std::wstring& path, const std::vector<unsigned char>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool okay = bytes.empty() || (WriteFile(file, bytes.data(),
        static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE && written == bytes.size());
    const bool flushed = okay && FlushFileBuffers(file) != FALSE;
    return CloseHandle(file) != FALSE && flushed;
}

bool ReadBytes(const std::wstring& path, std::vector<unsigned char>& bytes) {
    bytes.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length = {};
    bool okay = GetFileSizeEx(file, &length) != FALSE && length.QuadPart >= 0 &&
        length.QuadPart <= 1024 * 1024;
    if (okay) {
        bytes.resize(static_cast<size_t>(length.QuadPart));
        DWORD read = 0;
        okay = bytes.empty() || (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
            &read, nullptr) != FALSE && read == bytes.size());
    }
    CloseHandle(file);
    if (!okay) bytes.clear();
    return okay;
}

std::vector<unsigned char> Utf8Bom(const std::wstring& text) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0 && !text.empty()) return {};
    std::vector<unsigned char> bytes = {0xef, 0xbb, 0xbf};
    bytes.resize(3 + static_cast<size_t>(count));
    if (count > 0 && WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        reinterpret_cast<char*>(bytes.data() + 3), count, nullptr, nullptr) != count) return {};
    return bytes;
}

std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length < 0 || length > 1024 * 1024) return {};
    std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
    GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    return text.data();
}

bool OwnedByGuide(HWND window) {
    for (HWND owner = GetWindow(window, GW_OWNER); owner; owner = GetWindow(owner, GW_OWNER))
        if (owner == gGuide) return true;
    return false;
}

std::string DiagnosticUtf8(const std::wstring& text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        utf8.data(), length, nullptr, nullptr);
    return utf8;
}

BOOL CALLBACK VisibleModalButton(HWND window, LPARAM parameter) {
    wchar_t className[64] = {};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    if (!_wcsicmp(className, L"Button") && IsWindowVisible(window))
        reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(window);
    return TRUE;
}

void PrintUiState(const char* reason) {
    const HWND fontSize = gGuide ? GetDlgItem(gGuide, kFontSize) : nullptr;
    const HWND lineSpacing = gGuide ? GetDlgItem(gGuide, kLineSpacing) : nullptr;
    const HWND exportButton = gGuide ? GetDlgItem(gGuide, kExport) : nullptr;
    HWND text = gGuide ? FindWindowExW(gGuide, nullptr, RICHEDIT_CLASSW, nullptr) : nullptr;
    if (!text && gGuide) text = FindWindowExW(gGuide, nullptr, L"Edit", nullptr);
    std::printf("DIAG %s: policy=%d expectFailure=%d confirmations=%d results=%d errors=%d guideEnabled=%d "
        "exportEnabled=%d fontSize='%s' lineSpacing='%s' exportText='%s' body='%s'\n",
        reason, static_cast<int>(gModal.policy), gModal.expectFailure ? 1 : 0,
        gModal.confirmations, gModal.results, gModal.errors,
        IsWindowEnabled(gGuide), IsWindowEnabled(exportButton),
        DiagnosticUtf8(WindowText(fontSize)).c_str(), DiagnosticUtf8(WindowText(lineSpacing)).c_str(),
        DiagnosticUtf8(WindowText(exportButton)).c_str(), DiagnosticUtf8(WindowText(text)).c_str());
    std::fflush(stdout);
}

BOOL CALLBACK HandleModal(HWND window, LPARAM) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !IsWindowVisible(window) || !OwnedByGuide(window)) return TRUE;
    wchar_t className[64] = {};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    if (wcscmp(className, L"#32770") != 0 || GetPropW(window, kHandledDialog)) return TRUE;
    SetPropW(window, kHandledDialog, reinterpret_cast<HANDLE>(&gModal));
    const HWND cancel = GetDlgItem(window, IDCANCEL);
    const HWND okay = GetDlgItem(window, IDOK);
    std::vector<HWND> buttons;
    EnumChildWindows(window, VisibleModalButton, reinterpret_cast<LPARAM>(&buttons));
    // A native one-button MessageBox may expose its dismissal button as
    // IDCANCEL (or alias it through GetDlgItem). Classify actual visible
    // buttons rather than treating the existence of IDCANCEL as confirmation.
    const bool confirmation = buttons.size() > 1;
    std::printf("DIAG modal title='%s' hwnd=%p policy=%d confirmation=%d visibleButtons=%zu "
        "IDOK=%p visible=%d enabled=%d IDCANCEL=%p visible=%d enabled=%d\n",
        DiagnosticUtf8(WindowText(window)).c_str(), static_cast<void*>(window),
        static_cast<int>(gModal.policy), confirmation ? 1 : 0, buttons.size(),
        static_cast<void*>(okay), IsWindowVisible(okay), IsWindowEnabled(okay),
        static_cast<void*>(cancel), IsWindowVisible(cancel), IsWindowEnabled(cancel));
    for (HWND button : buttons) {
        std::printf("DIAG modal button hwnd=%p id=%d text='%s' visible=%d enabled=%d\n",
            static_cast<void*>(button), GetDlgCtrlID(button), DiagnosticUtf8(WindowText(button)).c_str(),
            IsWindowVisible(button), IsWindowEnabled(button));
    }
    HWND responseButton = buttons.empty() ? nullptr : buttons.front();
    if (confirmation) {
        ++gModal.confirmations;
        responseButton = gModal.policy == ConfirmPolicy::Accept ? okay : cancel;
        if (gModal.policy == ConfirmPolicy::Unexpected) ++gModal.errors;
    } else {
        ++gModal.results;
        const wchar_t* expectedTitle = gModal.expectFailure ? L"导出攻略失败" : L"导出攻略";
        if (WindowText(window) != expectedTitle || gModal.policy == ConfirmPolicy::Unexpected)
            ++gModal.errors;
    }
    if (!responseButton || !IsWindowVisible(responseButton)) {
        ++gModal.errors;
        std::printf("DIAG modal error: no visible response button\n");
        PostMessageW(window, WM_CLOSE, 0, 0);
    } else {
        const int response = GetDlgCtrlID(responseButton);
        const BOOL posted = PostMessageW(window, WM_COMMAND, MAKEWPARAM(response, BN_CLICKED),
            reinterpret_cast<LPARAM>(responseButton));
        std::printf("DIAG modal response id=%d posted=%d confirmations=%d results=%d errors=%d\n",
            response, posted, gModal.confirmations, gModal.results, gModal.errors);
        if (!posted) ++gModal.errors;
    }
    std::fflush(stdout);
    return TRUE;
}

void CALLBACK ModalTimer(HWND, UINT, UINT_PTR, DWORD) {
    // A thread timer is dispatched by MessageBox's nested loop as well as by
    // our outer loop, so a synchronous modal dialog cannot block the test.
    EnumWindows(HandleModal, 0);
}

bool WaitUntil(const std::function<bool()>& condition, DWORD timeout = kReadTimeout) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    for (;;) {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { PrintUiState("WaitUntil received WM_QUIT"); return false; }
            if (!HandleGuideMessage(&message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (gModal.errors) { PrintUiState("WaitUntil observed modal errors"); return false; }
        if (condition()) return true;
        if (GetTickCount64() >= deadline) { PrintUiState("WaitUntil timeout"); return false; }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

BOOL CALLBACK FindGuide(HWND window, LPARAM parameter) {
    wchar_t className[64] = {};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    if (!wcscmp(className, L"XiaoAn66GuideWindow") && GetWindow(window, GW_OWNER) == gMainWindow) {
        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK CountControls(HWND window, LPARAM parameter) {
    auto* counts = reinterpret_cast<int*>(parameter);
    wchar_t className[64] = {};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    if (!_wcsicmp(className, L"ComboBox")) ++counts[0];
    else if (!_wcsicmp(className, L"Button")) ++counts[1];
    else if (!_wcsicmp(className, RICHEDIT_CLASSW) || !_wcsicmp(className, L"Edit")) ++counts[2];
    return TRUE;
}

int ScenarioIndex(HWND combo, const wchar_t* prefix) {
    const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT index = 0; index < count; ++index) {
        const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(index), 0);
        if (length < 0 || length > 1024) continue;
        std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
        SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(text.data()));
        if (wcsncmp(text.data(), prefix, wcslen(prefix)) == 0) return static_cast<int>(index);
    }
    return -1;
}

bool SelectScenario(HWND combo, int index) {
    if (index < 0 || SendMessageW(combo, CB_SETCURSEL, index, 0) == CB_ERR) return false;
    return PostMessageW(gGuide, WM_COMMAND, MAKEWPARAM(kScenario, CBN_SELCHANGE),
        reinterpret_cast<LPARAM>(combo)) != FALSE;
}

bool Click(HWND button) {
    return button && IsWindowEnabled(button) && PostMessageW(button, BM_CLICK, 0, 0) != FALSE;
}

bool Contains(HWND text, const wchar_t* marker) {
    const std::wstring value = WindowText(text);
    return value.find(L"正在整理") == std::wstring::npos && value.find(marker) != std::wstring::npos;
}

bool HasDisplayFormat(HWND text, LONG fontSize, LONG lineSpacing) {
    CHARFORMAT2W character = {};
    character.cbSize = sizeof(character);
    SendMessageW(text, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&character));
    PARAFORMAT2 paragraph = {};
    paragraph.cbSize = sizeof(paragraph);
    SendMessageW(text, EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&paragraph));
    return (character.dwMask & CFM_SIZE) && character.yHeight == fontSize &&
        (paragraph.dwMask & PFM_SPACEAFTER) && paragraph.dySpaceAfter == lineSpacing;
}

bool FitsMinimumWidth(HWND scenario, HWND fontSize, HWND lineSpacing,
    HWND exportButton, HWND text) {
    MINMAXINFO limits = {};
    SendMessageW(gGuide, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
    if (limits.ptMinTrackSize.x != 700 || limits.ptMinTrackSize.y != 240) return false;
    if (!SetWindowPos(gGuide, nullptr, 0, 0, limits.ptMinTrackSize.x, limits.ptMinTrackSize.y,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) return false;
    RECT client = {};
    GetClientRect(gGuide, &client);
    const HWND controls[] = {scenario, fontSize, lineSpacing, exportButton};
    LONG previousRight = 0;
    for (HWND control : controls) {
        RECT bounds = {};
        if (!GetWindowRect(control, &bounds)) return false;
        MapWindowPoints(nullptr, gGuide, reinterpret_cast<POINT*>(&bounds), 2);
        if (bounds.left < previousRight || bounds.top < 0 || bounds.right > client.right ||
            bounds.bottom > client.bottom || bounds.right <= bounds.left) return false;
        previousRight = bounds.right;
    }
    RECT fontBounds = {}, spacingBounds = {}, scenarioBounds = {};
    if (!GetWindowRect(fontSize, &fontBounds) || !GetWindowRect(lineSpacing, &spacingBounds) ||
        !GetWindowRect(scenario, &scenarioBounds) || fontBounds.top != scenarioBounds.top ||
        spacingBounds.top != fontBounds.top || spacingBounds.left <= fontBounds.right) return false;
    RECT body = {};
    if (!GetWindowRect(text, &body)) return false;
    MapWindowPoints(nullptr, gGuide, reinterpret_cast<POINT*>(&body), 2);
    return body.left >= 0 && body.top >= 44 && body.right <= client.right &&
        body.bottom <= client.bottom && body.bottom - body.top >= 80;
}

bool IsBomGuide(const std::wstring& document, std::vector<unsigned char>& bytes, std::wstring& decoded) {
    return ReadBytes(document, bytes) && bytes.size() > 3 && bytes[0] == 0xef && bytes[1] == 0xbb &&
        bytes[2] == 0xbf && DecodeGuideDocumentForTest(bytes, decoded) &&
        decoded.find(L"第 00 关简易攻略") != std::wstring::npos &&
        decoded.find(L"第 02 关简易攻略") != std::wstring::npos;
}

bool Prepare(const std::wstring& root) {
    std::error_code error;
    std::filesystem::create_directories(root + L"\\RS", error);
    return !error && WriteBytes(root + L"\\RS\\S_00.eex", RewardScenario(100)) &&
        WriteBytes(root + L"\\RS\\S_02.eex", RewardScenario(200)) &&
        WriteBytes(root + L"\\简易攻略.txt", Utf8Bom(L"第00关简易攻略\r\n作者零关正文。\r\n\r\n自由段落。"));
}

void ValidateUi(const std::wstring& root) {
    const std::wstring document = root + L"\\简易攻略.txt";
    ObserveGuideScenarioPathA("RS\\S_00.eex");
    if (!Check(OpenGuide(gMainWindow, root.c_str()), "open production guide with a real modeless window")) return;
    EnumThreadWindows(GetCurrentThreadId(), FindGuide, reinterpret_cast<LPARAM>(&gGuide));
    if (!Check(gGuide && GetWindow(gGuide, GW_OWNER) == gMainWindow && IsWindowEnabled(gMainWindow),
        "guide belongs to the fake owner without disabling it")) return;

    const HWND scenario = GetDlgItem(gGuide, kScenario);
    const HWND fontSize = GetDlgItem(gGuide, kFontSize);
    const HWND lineSpacing = GetDlgItem(gGuide, kLineSpacing);
    const HWND exportButton = GetDlgItem(gGuide, kExport);
    HWND text = FindWindowExW(gGuide, nullptr, RICHEDIT_CLASSW, nullptr);
    if (!text) text = FindWindowExW(gGuide, nullptr, L"Edit", nullptr);
    int controls[3] = {};
    EnumChildWindows(gGuide, CountControls, reinterpret_cast<LPARAM>(controls));
    if (!Check(scenario && fontSize && lineSpacing && exportButton && text && controls[0] == 1 &&
        controls[1] == 1 && controls[2] == 3 && WindowText(fontSize) == L"220" &&
        WindowText(lineSpacing) == L"45" && !GetDlgItem(gGuide, 0x6701) &&
        WindowText(exportButton) == L"导出攻略", "guide contains one scenario selector, two display inputs and only the export action")) return;
    Check(FitsMinimumWidth(scenario, fontSize, lineSpacing, exportButton, text),
        "700 by 240 minimum size keeps all toolbar controls in one row and text visible");
    if (!Check(WaitUntil([&]() { return Contains(text, L"作者零关正文。"); }),
        "authored section displays free paragraphs without a category filter")) return;
    Check(HasDisplayFormat(text, 220, 45), "default display inputs match the production RichEdit format");

    SetWindowTextW(fontSize, L"300");
    SetWindowTextW(lineSpacing, L"90");
    const std::wstring displaySettings = PatchPrivatePath(root, CompiledEngineProfile()) + L"\\guide-ui.ini";
    Check(HasDisplayFormat(text, 300, 90) &&
        GetPrivateProfileIntW(L"Guide", L"FontHeightTwips", -1, displaySettings.c_str()) == 300 &&
        GetPrivateProfileIntW(L"Guide", L"LineSpacingTwips", -1, displaySettings.c_str()) == 90,
        "native font and line-spacing values apply immediately and persist independently of the patch manifest");
    SetWindowTextW(fontSize, L"999999");
    SendMessageW(gGuide, WM_COMMAND, MAKEWPARAM(kFontSize, EN_KILLFOCUS),
        reinterpret_cast<LPARAM>(fontSize));
    Check(WindowText(fontSize) == L"300" && HasDisplayFormat(text, 300, 90),
        "out-of-range font input reverts to the last valid renderer setting");
    SetWindowTextW(lineSpacing, L"");
    SendMessageW(gGuide, WM_COMMAND, MAKEWPARAM(kLineSpacing, EN_KILLFOCUS),
        reinterpret_cast<LPARAM>(lineSpacing));
    Check(WindowText(lineSpacing) == L"90" && HasDisplayFormat(text, 300, 90),
        "empty line-spacing input reverts without changing renderer format");
    std::vector<unsigned char> settingsBeforeLock, settingsAfterLock;
    HANDLE settingsLock = CreateFileW(displaySettings.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(settingsLock != INVALID_HANDLE_VALUE && ReadBytes(displaySettings, settingsBeforeLock),
        "lock only the temporary display settings file before atomic-save verification");
    if (settingsLock != INVALID_HANDLE_VALUE) {
        SetWindowTextW(fontSize, L"320");
        Check(ReadBytes(displaySettings, settingsAfterLock) && settingsAfterLock == settingsBeforeLock &&
            HasDisplayFormat(text, 300, 90) && WindowText(fontSize) == L"300" &&
            WindowText(lineSpacing) == L"90",
            "failed settings replacement restores previous valid controls and format and preserves original file bytes");
        CloseHandle(settingsLock);
        SetWindowTextW(fontSize, L"300");
    }

    const int zero = ScenarioIndex(scenario, L"S_00 ");
    const int two = ScenarioIndex(scenario, L"S_02 ");
    if (!Check(SelectScenario(scenario, two) && WaitUntil([&]() {
        return Contains(text, L"200 金钱"); }),
        "missing authored section restores automatic generation")) return;
    if (!Check(SelectScenario(scenario, zero) && WaitUntil([&]() {
        return Contains(text, L"作者零关正文。"); }),
        "selecting author text keeps the complete guide visible")) return;
    if (!Check(SelectScenario(scenario, two) && WaitUntil([&]() {
        return Contains(text, L"200 金钱"); }),
        "switching back to an uncovered battle restores automatic guide text")) return;

    if (!Check(WriteBytes(document, Utf8Bom(L"第02关简易攻略\r\n外部修改后的二关正文。")) &&
        SelectScenario(scenario, two) && WaitUntil([&]() {
            return SendMessageW(scenario, CB_GETCURSEL, 0, 0) == two &&
                Contains(text, L"外部修改后的二关正文。");
        }), "selecting the scenario again reads external authored changes")) return;

    if (!Check(DeleteFileW(document.c_str()) != FALSE, "remove only the temporary guide before first export")) return;
    gModal = {};
    gModal.policy = ConfirmPolicy::Accept;
    if (!Check(Click(exportButton) && WaitUntil([&]() {
        return gModal.results == 1 && IsWindowEnabled(gGuide) && IsWindowEnabled(exportButton) &&
            Contains(text, L"200 金钱");
        }, kExportTimeout), "export without an existing file closes result dialog and refreshes current authored display")) return;
    std::vector<unsigned char> bytes;
    std::wstring decoded;
    Check(gModal.confirmations == 0 && gModal.errors == 0 && IsBomGuide(document, bytes, decoded) &&
        decoded.find(L"100 金钱") != std::wstring::npos && decoded.find(L"200 金钱") != std::wstring::npos,
        "first export writes all actual S scripts as a UTF-8 BOM guide document");

    const auto authorOriginal = Utf8Bom(L"第02关简易攻略\r\n取消覆盖必须保留的作者正文。");
    if (!Check(WriteBytes(document, authorOriginal) && SelectScenario(scenario, two) && WaitUntil([&]() {
        return Contains(text, L"取消覆盖必须保留的作者正文。"); }),
        "load nonempty author document before overwrite tests")) return;
    gModal = {};
    gModal.policy = ConfirmPolicy::Cancel;
    if (!Check(Click(exportButton) && WaitUntil([&]() {
        return gModal.confirmations == 1 && IsWindowEnabled(gGuide) && IsWindowEnabled(exportButton);
        }), "cancel nonempty-file overwrite confirmation without blocking the UI")) return;
    Check(gModal.results == 0 && ReadBytes(document, bytes) && bytes == authorOriginal &&
        Contains(text, L"取消覆盖必须保留的作者正文。"),
        "cancelled overwrite preserves original file bytes and displayed author content");

    gModal = {};
    gModal.policy = ConfirmPolicy::Accept;
    if (!Check(Click(exportButton) && WaitUntil([&]() {
        return gModal.confirmations == 1 && gModal.results == 1 && IsWindowEnabled(gGuide) &&
            IsWindowEnabled(exportButton) && Contains(text, L"200 金钱");
        }, kExportTimeout), "confirmed overwrite finishes asynchronously and closes its result dialog")) return;
    Check(IsBomGuide(document, bytes, decoded) && bytes != authorOriginal &&
        decoded.find(L"取消覆盖必须保留的作者正文。") == std::wstring::npos &&
        SendMessageW(scenario, CB_GETCURSEL, 0, 0) == two,
        "confirmed overwrite replaces temporary author file and preserves current selected battle");

    const auto lockedOriginal = Utf8Bom(L"第02关简易攻略\r\n目标文件锁定时必须保留的作者正文。");
    if (!Check(WriteBytes(document, lockedOriginal) && SelectScenario(scenario, two) && WaitUntil([&]() {
        return Contains(text, L"目标文件锁定时必须保留的作者正文。");
        }), "prepare authored page before locking its target file against replacement")) return;
    const std::wstring pageBeforeFailure = WindowText(text);
    HANDLE locked = CreateFileW(document.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!Check(locked != INVALID_HANDLE_VALUE, "lock only the temporary guide against writes and deletion")) return;
    gModal = {};
    gModal.policy = ConfirmPolicy::Accept;
    gModal.expectFailure = true;
    const bool failureReported = Click(exportButton) && WaitUntil([&]() {
        return gModal.confirmations == 1 && gModal.results == 1 && IsWindowEnabled(gGuide) &&
            IsWindowEnabled(scenario) && IsWindowEnabled(exportButton) &&
            WindowText(exportButton) == L"导出攻略" && WindowText(text) == pageBeforeFailure &&
            WindowText(fontSize) == L"300" && WindowText(lineSpacing) == L"90";
        }, kExportTimeout);
    CloseHandle(locked);
    if (!Check(failureReported,
        "locked-target export closes expected failure result and restores prior page and controls")) return;
    Check(gModal.errors == 0 && ReadBytes(document, bytes) && bytes == lockedOriginal &&
        SendMessageW(scenario, CB_GETCURSEL, 0, 0) == two && WindowText(text) == pageBeforeFailure,
        "failed export preserves original bytes, selected battle, and display settings");

    CloseGuide();
    gGuide = nullptr;
    ObserveGuideScenarioPathA("RS\\S_02.eex");
    Check(OpenGuide(gMainWindow, root.c_str()), "reopen guide after closing the first window");
    EnumThreadWindows(GetCurrentThreadId(), FindGuide, reinterpret_cast<LPARAM>(&gGuide));
    HWND reopenedText = gGuide ? FindWindowExW(gGuide, nullptr, RICHEDIT_CLASSW, nullptr) : nullptr;
    Check(gGuide && WindowText(GetDlgItem(gGuide, kFontSize)) == L"300" &&
        WindowText(GetDlgItem(gGuide, kLineSpacing)) == L"90" &&
        WaitUntil([&]() { return Contains(reopenedText, L"作者正文") &&
            HasDisplayFormat(reopenedText, 300, 90); }),
        "closing and reopening restores the saved guide display settings");
}

} // namespace

int wmain() {
    wchar_t temporary[32768] = {};
    const DWORD length = GetTempPathW(static_cast<DWORD>(_countof(temporary)), temporary);
    if (!length || length >= _countof(temporary)) return 2;
    const std::wstring root = std::wstring(temporary) + L"CczGuideWindowTests-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE stopWatchdog = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopWatchdog) return 2;
    std::thread watchdog([stopWatchdog]() {
        if (WaitForSingleObject(stopWatchdog, 45000) != WAIT_OBJECT_0) {
            std::fputs("FAIL GuideWindowTests exceeded the 45 second process timeout\n", stderr);
            std::fflush(stderr);
            ExitProcess(124);
        }
    });

    gInstance = GetModuleHandleW(nullptr);
    WNDCLASSW ownerClass = {};
    ownerClass.hInstance = gInstance;
    ownerClass.lpfnWndProc = DefWindowProcW;
    ownerClass.lpszClassName = L"CczGuideWindowTestsOwner";
    const bool registered = RegisterClassW(&ownerClass) != 0;
    if (registered) gMainWindow = CreateWindowExW(0, ownerClass.lpszClassName,
        L"Guide UI test owner", WS_OVERLAPPEDWINDOW, 20, 20, 320, 240, nullptr, nullptr, gInstance, nullptr);
    const UINT_PTR timer = SetTimer(nullptr, 0, 25, ModalTimer);
    if (Check(registered && gMainWindow && timer && Prepare(root),
        "prepare owner window, modal watchdog timer, and isolated temporary EEX fixtures")) {
        ShowWindow(gMainWindow, SW_SHOWNOACTIVATE);
        ValidateUi(root);
    }
    CloseGuide();
    gGuide = nullptr;
    if (timer) KillTimer(nullptr, timer);
    if (gMainWindow) DestroyWindow(gMainWindow);
    gMainWindow = nullptr;
    if (registered) UnregisterClassW(ownerClass.lpszClassName, gInstance);
    SetEvent(stopWatchdog);
    watchdog.join();
    CloseHandle(stopWatchdog);
    std::printf("GuideWindowTests: %d failures\n", gFailures);
    std::wprintf(L"Temporary test evidence: %ls\n", root.c_str());
    return gFailures ? 1 : 0;
}
