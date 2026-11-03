#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <cwctype>
#include <string>
#include <vector>

namespace {

struct SearchContext {
    DWORD process = 0;
    std::wstring title;
    bool requireMenu = false;
    HWND found = nullptr;
};

BOOL CALLBACK FindWindowForProcess(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<SearchContext*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != context->process || !IsWindowVisible(window)) return TRUE;
    if (context->requireMenu && !GetMenu(window)) return TRUE;
    wchar_t title[256] = {};
    GetWindowTextW(window, title, static_cast<int>(_countof(title)));
    if (!context->title.empty() && context->title != title) return TRUE;
    context->found = window;
    return FALSE;
}

HWND FindWindowForProcess(DWORD process, const wchar_t* title, bool requireMenu = false) {
    SearchContext context;
    context.process = process;
    if (title) context.title = title;
    context.requireMenu = requireMenu;
    EnumWindows(FindWindowForProcess, reinterpret_cast<LPARAM>(&context));
    return context.found;
}

std::wstring MenuText(HMENU menu, int position) {
    wchar_t text[128] = {};
    GetMenuStringW(menu, static_cast<UINT>(position), text, static_cast<int>(_countof(text)), MF_BYPOSITION);
    return text;
}

BOOL CALLBACK CountChild(HWND window, LPARAM parameter) {
    auto* counts = reinterpret_cast<int*>(parameter);
    wchar_t className[64] = {};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    if (!_wcsicmp(className, L"ComboBox")) ++counts[0];
    else if (!_wcsicmp(className, L"Button")) ++counts[1];
    else if (!_wcsicmp(className, L"Edit") || !_wcsicmp(className, RICHEDIT_CLASSW)) ++counts[2];
    return TRUE;
}

HWND FindChildByClass(HWND parent, const wchar_t* className) {
    return FindWindowExW(parent, nullptr, className, nullptr);
}

struct ChildTextSearch {
    const wchar_t* className = nullptr;
    const wchar_t* textPrefix = nullptr;
    HWND found = nullptr;
};

BOOL CALLBACK FindChildByClassAndTextPrefixCallback(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<ChildTextSearch*>(parameter);
    wchar_t className[64] = {};
    if (GetClassNameW(window, className, static_cast<int>(_countof(className))) <= 0 ||
        _wcsicmp(className, search->className) != 0) return TRUE;
    wchar_t text[256] = {};
    GetWindowTextW(window, text, static_cast<int>(_countof(text)));
    if (wcsncmp(text, search->textPrefix, wcslen(search->textPrefix)) != 0) return TRUE;
    search->found = window;
    return FALSE;
}

HWND FindChildByClassAndTextPrefix(HWND parent, const wchar_t* className, const wchar_t* textPrefix) {
    ChildTextSearch search;
    search.className = className;
    search.textPrefix = textPrefix;
    EnumChildWindows(parent, FindChildByClassAndTextPrefixCallback, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

std::wstring ReadControlText(HWND control) {
    DWORD_PTR length = 0;
    if (!SendMessageTimeoutW(control, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 1000, &length)) return {};
    std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
    DWORD_PTR ignored = 0;
    if (!SendMessageTimeoutW(control, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()),
        SMTO_ABORTIFHUNG, 1000, &ignored)) return {};
    return text.data();
}

int FindComboItemPrefix(HWND combo, const wchar_t* prefix) {
    const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < count; ++i) {
        const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0);
        if (length < 0 || length > 1024) continue;
        std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
        if (SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(i),
            reinterpret_cast<LPARAM>(text.data())) < 0) continue;
        if (wcsncmp(text.data(), prefix, wcslen(prefix)) == 0) return static_cast<int>(i);
    }
    return -1;
}

std::wstring RemoveWhitespace(const std::wstring& text);

bool SelectScenario(HWND guide, HWND combo, HWND textControl, int index, const wchar_t* expectedTitle) {
    if (index < 0 || SendMessageW(combo, CB_SETCURSEL, index, 0) == CB_ERR) return false;
    SendMessageW(guide, WM_COMMAND, MAKEWPARAM(0x6703, CBN_SELCHANGE),
        reinterpret_cast<LPARAM>(combo));
    for (int i = 0; i < 200; ++i) {
        const std::wstring text = ReadControlText(textControl);
        if (text.find(L"正在整理") == std::wstring::npos &&
            RemoveWhitespace(text).find(RemoveWhitespace(expectedTitle)) != std::wstring::npos)
            return true;
        Sleep(25);
    }
    return false;
}

bool ContainsForbiddenText(const std::wstring& text) {
    static const wchar_t* words[] = {
        L"来源", L"未确认", L"未完全解析", L"命令编号", L"偏移", L"[loose@", L".eex"
    };
    for (const wchar_t* word : words) if (text.find(word) != std::wstring::npos) return true;
    return false;
}

uintptr_t ProcessImageBase(DWORD process) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W module = {sizeof(module)};
    const uintptr_t result = Module32FirstW(snapshot, &module) ?
        reinterpret_cast<uintptr_t>(module.modBaseAddr) : 0;
    CloseHandle(snapshot);
    return result;
}

bool ReadBytes(HANDLE process, uintptr_t address, BYTE (&bytes)[3]) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), bytes,
        sizeof(bytes), &read) != FALSE && read == sizeof(bytes);
}

bool ReadParagraphFormat(HANDLE process, HWND control, PARAFORMAT2& format) {
    void* remote = VirtualAllocEx(process, nullptr, sizeof(format), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    SIZE_T transferred = 0;
    DWORD_PTR messageResult = 0;
    const bool result = WriteProcessMemory(process, remote, &format, sizeof(format), &transferred) != FALSE &&
        transferred == sizeof(format) &&
        SendMessageTimeoutW(control, EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(remote),
            SMTO_ABORTIFHUNG, 1000, &messageResult) != 0 &&
        ReadProcessMemory(process, remote, &format, sizeof(format), &transferred) != FALSE &&
        transferred == sizeof(format);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return result;
}

bool ReadCharacterFormat(HANDLE process, HWND control, CHARFORMAT2W& format) {
    void* remote = VirtualAllocEx(process, nullptr, sizeof(format), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    SIZE_T transferred = 0;
    DWORD_PTR messageResult = 0;
    const bool result = WriteProcessMemory(process, remote, &format, sizeof(format), &transferred) != FALSE &&
        transferred == sizeof(format) &&
        SendMessageTimeoutW(control, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(remote),
            SMTO_ABORTIFHUNG, 1000, &messageResult) != 0 &&
        ReadProcessMemory(process, remote, &format, sizeof(format), &transferred) != FALSE &&
        transferred == sizeof(format);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return result;
}

bool ReadTextFile(const std::wstring& path, std::wstring& text) {
    text.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    const bool sized = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 && size.QuadPart <= 16 * 1024 * 1024;
    if (!sized) { CloseHandle(file); return false; }
    std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool okay = bytes.empty() || ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!okay || read != bytes.size()) return false;
    if (bytes.empty()) return true;
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        if ((bytes.size() - 2) % 2 != 0) return false;
        text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
        while (!text.empty() && text.back() == 0) text.pop_back();
        return true;
    }
    while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
    if (bytes.empty()) return true;
    size_t offset = 0;
    UINT codePage = CP_UTF8;
    const DWORD flags = MB_ERR_INVALID_CHARS;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        offset = 3;
    }
    if (bytes.size() - offset > static_cast<size_t>(INT_MAX)) return false;
    if (bytes.size() == offset) return true;
    int length = MultiByteToWideChar(codePage, flags,
        reinterpret_cast<const char*>(bytes.data() + offset), static_cast<int>(bytes.size() - offset),
        nullptr, 0);
    if (length <= 0 && offset == 0) {
        codePage = 936;
        length = MultiByteToWideChar(codePage, flags,
            reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (length <= 0) return false;
    text.resize(static_cast<size_t>(length));
    return MultiByteToWideChar(codePage, flags,
        reinterpret_cast<const char*>(bytes.data() + offset), static_cast<int>(bytes.size() - offset),
        text.data(), length) == length;
}

bool GetProcessDirectory(HANDLE process, std::wstring& directory) {
    directory.clear();
    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &length) || length == 0) return false;
    std::wstring path(buffer.data(), length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return false;
    directory = path.substr(0, slash);
    return true;
}

bool HasNonWhitespaceText(const std::wstring& text) {
    for (wchar_t ch : text) if (!iswspace(ch)) return true;
    return false;
}

std::wstring FirstMeaningfulLine(const std::wstring& text) {
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find_first_of(L"\r\n", at);
        std::wstring line = text.substr(at, end == std::wstring::npos ? text.size() - at : end - at);
        size_t first = 0;
        while (first < line.size() && iswspace(line[first])) ++first;
        size_t last = line.size();
        while (last > first && iswspace(line[last - 1])) --last;
        if (last > first) return line.substr(first, last - first);
        if (end == std::wstring::npos) break;
        at = end + 1;
        while (at < text.size() && (text[at] == L'\r' || text[at] == L'\n')) ++at;
    }
    return {};
}

std::wstring RemoveWhitespace(const std::wstring& text) {
    std::wstring compact;
    compact.reserve(text.size());
    for (wchar_t ch : text) if (!iswspace(ch)) compact.push_back(ch);
    return compact;
}

bool IsGuideSectionHeader(const std::wstring& line) {
    const std::wstring compact = RemoveWhitespace(line);
    if (compact.size() < 7 || compact.compare(0, 1, L"第") != 0) return false;
    size_t at = 1;
    const size_t digitsAt = at;
    while (at < compact.size() && compact[at] >= L'0' && compact[at] <= L'9') ++at;
    return at > digitsAt && compact.compare(at, std::wstring::npos, L"关简易攻略") == 0;
}

bool ReadGuideFileBody(const std::wstring& guideFile, const std::wstring& title, std::wstring& body) {
    body.clear();
    std::wstring contents;
    if (!ReadTextFile(guideFile, contents) || !HasNonWhitespaceText(contents)) return false;
    size_t titleAt = std::wstring::npos;
    const std::wstring compactTitle = RemoveWhitespace(title);
    for (size_t at = 0; at < contents.size();) {
        const size_t lineEnd = contents.find_first_of(L"\r\n", at);
        const size_t length = lineEnd == std::wstring::npos ? contents.size() - at : lineEnd - at;
        if (RemoveWhitespace(contents.substr(at, length)) == compactTitle) {
            // Match production's last-section-wins rule, including a final
            // empty section which deliberately suppresses an earlier body.
            titleAt = at;
        }
        if (lineEnd == std::wstring::npos) break;
        at = lineEnd + 1;
        while (at < contents.size() && (contents[at] == L'\r' || contents[at] == L'\n')) ++at;
    }
    if (titleAt == std::wstring::npos) return false;
    size_t bodyStart = contents.find_first_of(L"\r\n", titleAt);
    if (bodyStart == std::wstring::npos) return false;
    while (bodyStart < contents.size() && (contents[bodyStart] == L'\r' || contents[bodyStart] == L'\n')) ++bodyStart;
    size_t bodyEnd = contents.size();
    for (size_t at = bodyStart; at < contents.size();) {
        const size_t lineEnd = contents.find_first_of(L"\r\n", at);
        const size_t length = lineEnd == std::wstring::npos ? contents.size() - at : lineEnd - at;
        if (IsGuideSectionHeader(contents.substr(at, length))) {
            bodyEnd = at;
            break;
        }
        if (lineEnd == std::wstring::npos) break;
        at = lineEnd + 1;
        while (at < contents.size() && (contents[at] == L'\r' || contents[at] == L'\n')) ++at;
    }
    body.assign(contents, bodyStart, bodyEnd - bodyStart);
    return HasNonWhitespaceText(body);
}

struct ExportDialogSearch {
    DWORD process = 0;
    HWND guide = nullptr;
    HWND dialog = nullptr;
};

BOOL CALLBACK FindExportDialogCallback(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<ExportDialogSearch*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != search->process || !IsWindowVisible(window)) return TRUE;
    wchar_t className[64] = {};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    if (wcscmp(className, L"#32770") != 0) return TRUE;
    HWND owner = GetWindow(window, GW_OWNER);
    while (owner && owner != search->guide) owner = GetWindow(owner, GW_OWNER);
    if (owner != search->guide) return TRUE;
    search->dialog = window;
    return FALSE;
}

HWND FindExportDialog(DWORD process, HWND guide) {
    ExportDialogSearch search;
    search.process = process;
    search.guide = guide;
    EnumWindows(FindExportDialogCallback, reinterpret_cast<LPARAM>(&search));
    return search.dialog;
}

BOOL CALLBACK FindVisibleExportButtons(HWND window, LPARAM parameter) {
    wchar_t className[64] = {};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    if (!_wcsicmp(className, L"Button") && IsWindowVisible(window))
        reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(window);
    return TRUE;
}

bool HandleExportDialog(HWND dialog, bool& successDialog) {
    std::wstring message = ReadControlText(dialog);
    for (HWND control = GetWindow(dialog, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT))
        message += L"\n" + ReadControlText(control);
    const bool failed = message.find(L"失败") != std::wstring::npos ||
        message.find(L"无法") != std::wstring::npos || message.find(L"错误") != std::wstring::npos;
    std::vector<HWND> buttons;
    EnumChildWindows(dialog, FindVisibleExportButtons, reinterpret_cast<LPARAM>(&buttons));
    if (buttons.empty()) return false;
    const bool confirmation = buttons.size() > 1;
    const HWND yes = GetDlgItem(dialog, IDYES);
    const HWND okay = GetDlgItem(dialog, IDOK);
    // Native one-button MessageBox controls can use IDCANCEL even though the
    // displayed action is OK. Only a multi-button dialog is a confirmation.
    const HWND response = confirmation ? (yes && IsWindowVisible(yes) ? yes : okay) : buttons.front();
    if (!response || !IsWindowVisible(response)) return false;
    if (!confirmation && !failed) successDialog = true;
    const BOOL posted = PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(response), BN_CLICKED),
        reinterpret_cast<LPARAM>(response));
    return posted != FALSE && (confirmation || !failed);
}

bool ReadFileStamp(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& stamp) {
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &stamp) != FALSE &&
        (stamp.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool WaitForGuideExport(DWORD process, HWND guide, HWND exportButton, const std::wstring& path,
    const WIN32_FILE_ATTRIBUTE_DATA* before, std::wstring& contents) {
    bool successDialog = false;
    int completePasses = 0;
    for (int attempt = 0; attempt < 400; ++attempt) {
        const HWND dialog = FindExportDialog(process, guide);
        if (dialog && !HandleExportDialog(dialog, successDialog)) return false;
        WIN32_FILE_ATTRIBUTE_DATA after = {};
        const bool changed = ReadFileStamp(path, after) && (!before ||
            CompareFileTime(&before->ftLastWriteTime, &after.ftLastWriteTime) != 0 ||
            before->nFileSizeHigh != after.nFileSizeHigh || before->nFileSizeLow != after.nFileSizeLow);
        if (!dialog && IsWindowEnabled(guide) && IsWindowEnabled(exportButton) &&
            (changed || successDialog) && ReadTextFile(path, contents) && HasNonWhitespaceText(contents)) {
            if (++completePasses >= 5) return true;
        } else completePasses = 0;
        Sleep(50);
    }
    contents.clear();
    return false;
}

bool FocusWindow(HWND window) {
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    for (int i = 0; i < 40; ++i) {
        if (GetForegroundWindow() == window) return true;
        Sleep(25);
    }
    return false;
}

bool PressZ() {
    INPUT input[2] = {};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = 'Z';
    input[1] = input[0];
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2, input, sizeof(INPUT)) == 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if ((argc != 2 && argc != 3) || (argc == 3 && wcscmp(argv[2], L"--export") != 0)) {
        fwprintf(stderr, L"Usage: GuideWindowHarness <game-pid> [--export]\n");
        return 2;
    }
    const bool exerciseExport = argc == 3;
    const DWORD process = wcstoul(argv[1], nullptr, 10);
    HWND mainWindow = FindWindowForProcess(process, nullptr, true);
    if (!mainWindow) return 3;
    HANDLE gameProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, process);
    const uintptr_t imageBase = ProcessImageBase(process);
    if (!gameProcess || !imageBase) {
        if (gameProcess) CloseHandle(gameProcess);
        return 3;
    }
    const uintptr_t dialogSite = imageBase + (0x42D5B0u - 0x400000u);
    BYTE originalDialog[3] = {};
    if (!ReadBytes(gameProcess, dialogSite, originalDialog)) {
        CloseHandle(gameProcess);
        return 3;
    }
    HMENU menu = GetMenu(mainWindow);
    const int count = menu ? GetMenuItemCount(menu) : 0;
    int patchPosition = -1, guidePosition = -1, statisticsPosition = -1;
    for (int i = 0; i < count; ++i) {
        const std::wstring text = MenuText(menu, i);
        if (text == L"加速补丁") patchPosition = i;
        if (text == L"简易攻略") guidePosition = i;
        if (text == L"战斗统计") statisticsPosition = i;
    }
    if (patchPosition < 0 || guidePosition != patchPosition + 1 ||
        statisticsPosition != guidePosition + 1) return 4;
    if (!PostMessageW(mainWindow, WM_COMMAND, 0xEE73, 0)) return 5;

    HWND guide = nullptr;
    for (int i = 0; i < 100 && !guide; ++i) {
        Sleep(50);
        guide = FindWindowForProcess(process, L"简易攻略");
    }
    if (!guide || GetWindow(guide, GW_OWNER) != mainWindow || !IsWindowEnabled(mainWindow)) return 6;
    const LONG_PTR style = GetWindowLongPtrW(guide, GWL_STYLE);
    if (!(style & WS_THICKFRAME) || !(style & WS_MINIMIZEBOX)) return 7;

    int childCounts[3] = {};
    EnumChildWindows(guide, CountChild, reinterpret_cast<LPARAM>(childCounts));
    if (childCounts[0] != 1 || childCounts[1] != 1 || childCounts[2] != 3 ||
        FindChildByClass(guide, WC_TABCONTROLW) ||
        FindChildByClass(guide, L"XiaoAn66GuideMap")) return 8;
    HWND scenarioCombo = FindChildByClass(guide, L"ComboBox");
    HWND fontSize = FindChildByClass(guide, L"Edit");
    HWND lineSpacing = fontSize ? FindWindowExW(guide, fontSize, L"Edit", nullptr) : nullptr;
    HWND exportButton = FindChildByClassAndTextPrefix(guide, L"Button", L"导出攻略");
    HWND textControl = FindChildByClass(guide, RICHEDIT_CLASSW);
    if (!textControl) textControl = FindChildByClass(guide, L"Edit");
    if (!scenarioCombo || !fontSize || !lineSpacing || !exportButton || !textControl) return 8;
    const long fontHeightTwips = wcstol(ReadControlText(fontSize).c_str(), nullptr, 10);
    const long lineSpacingTwips = wcstol(ReadControlText(lineSpacing).c_str(), nullptr, 10);
    if (fontHeightTwips < 80 || fontHeightTwips > 640 ||
        lineSpacingTwips < 0 || lineSpacingTwips > 400) return 18;
    wchar_t textClass[64] = {};
    GetClassNameW(textControl, textClass, static_cast<int>(_countof(textClass)));
    if (_wcsicmp(textClass, RICHEDIT_CLASSW)) return 15;
    const LONG_PTR textStyle = GetWindowLongPtrW(textControl, GWL_STYLE);
    if (textStyle & (WS_HSCROLL | ES_AUTOHSCROLL)) return 16;
    RECT clientRect = {};
    RECT textRect = {};
    GetClientRect(textControl, &clientRect);
    SendMessageW(textControl, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&textRect));
    if (textRect.left < 8 || clientRect.right - textRect.right < 8) return 17;
    PARAFORMAT2 paragraph = {};
    paragraph.cbSize = sizeof(paragraph);
    paragraph.dwMask = PFM_SPACEAFTER;
    if (!ReadParagraphFormat(gameProcess, textControl, paragraph) ||
        !(paragraph.dwMask & PFM_SPACEAFTER) || paragraph.dySpaceAfter != lineSpacingTwips) return 18;
    CHARFORMAT2W character = {};
    character.cbSize = sizeof(character);
    if (!ReadCharacterFormat(gameProcess, textControl, character) ||
        !(character.dwMask & CFM_SIZE) || character.yHeight != fontHeightTwips) return 19;
    std::wstring text;
    for (int i = 0; i < 200; ++i) {
        text = ReadControlText(textControl);
        if (text.find(L"正在整理") == std::wstring::npos) break;
        Sleep(50);
    }
    if (text.find(L"简易攻略") == std::wstring::npos) return 9;
    const LRESULT scenarioCount = SendMessageW(scenarioCombo, CB_GETCOUNT, 0, 0);
    if (scenarioCount < 2) return 9;
    const int s10 = FindComboItemPrefix(scenarioCombo, L"S_10 ");
    const int r10 = FindComboItemPrefix(scenarioCombo, L"R_10 ");
    if (!SelectScenario(guide, scenarioCombo, textControl, s10, L"第 10 关简易攻略") ||
        !SelectScenario(guide, scenarioCombo, textControl, r10, L"第 10 关简易攻略")) return 9;

    // Validate the custom guide before exercising export when the fixture already
    // contains one. This checks that author-provided text wins during display;
    // export itself is intentionally allowed to replace the generated file.
    std::wstring gameRoot;
    const std::wstring guideFileName = L"简易攻略.txt";
    const bool haveGameRoot = GetProcessDirectory(gameProcess, gameRoot);
    const std::wstring guideFile = haveGameRoot ? gameRoot + L"\\" + guideFileName : std::wstring();
    std::wstring authorBody;
    bool hasAuthorBody = false;
    if (haveGameRoot) {
        text = ReadControlText(textControl);
        const size_t titleEnd = text.find_first_of(L"\r\n");
        const std::wstring title = titleEnd == std::wstring::npos ? text : text.substr(0, titleEnd);
        hasAuthorBody = ReadGuideFileBody(guideFile, title, authorBody);
        if (hasAuthorBody) {
            const std::wstring marker = FirstMeaningfulLine(authorBody);
            if (marker.empty() || text.find(marker) == std::wstring::npos) return 20;
        }
    }
    if (!hasAuthorBody && (ContainsForbiddenText(text) || text.find(L"\r\n\r\n") != std::wstring::npos))
        return 9;

    // Export may overwrite an author's file, so only an explicit --export run
    // clicks this button and acknowledges dialogs owned by the guide window.
    if (exerciseExport) {
        if (!haveGameRoot || !IsWindowEnabled(exportButton)) return 21;
        WIN32_FILE_ATTRIBUTE_DATA before = {};
        const bool hadGuideFile = ReadFileStamp(guideFile, before);
        if (!PostMessageW(exportButton, BM_CLICK, 0, 0)) return 22;
        std::wstring exported;
        if (!WaitForGuideExport(process, guide, exportButton, guideFile,
            hadGuideFile ? &before : nullptr, exported)) return 23;
        if (exported.find(L"第 ") == std::wstring::npos && exported.find(L"第") == std::wstring::npos)
            return 24;
    }
    DWORD_PTR response = 0;
    if (!SendMessageTimeoutW(mainWindow, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &response)) return 10;

    BYTE guideFocusedDialog[3] = {};
    if (!FocusWindow(guide) || !PressZ()) return 12;
    Sleep(300);
    if (!ReadBytes(gameProcess, dialogSite, guideFocusedDialog) ||
        memcmp(originalDialog, guideFocusedDialog, sizeof(originalDialog)) != 0) return 12;

    PostMessageW(guide, WM_CLOSE, 0, 0);
    for (int i = 0; i < 100 && IsWindow(guide); ++i) Sleep(25);
    if (IsWindow(guide) || !IsWindow(mainWindow)) return 11;
    BYTE enabledDialog[3] = {};
    BYTE restoredDialog[3] = {};
    if (!FocusWindow(mainWindow) || !PressZ()) return 13;
    Sleep(300);
    if (!ReadBytes(gameProcess, dialogSite, enabledDialog) ||
        memcmp(originalDialog, enabledDialog, sizeof(originalDialog)) == 0) return 13;
    if (!PressZ()) return 14;
    Sleep(300);
    if (!ReadBytes(gameProcess, dialogSite, restoredDialog) ||
        memcmp(originalDialog, restoredDialog, sizeof(originalDialog)) != 0) return 14;
    CloseHandle(gameProcess);
    wprintf(L"GuideWindowHarness: PASS\n");
    return 0;
}
