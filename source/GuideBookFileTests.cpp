#include "Guide.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

Settings gSettings;
HMODULE gInstance = nullptr;
uintptr_t gGameBase = 0;
HWND gMainWindow = nullptr;
HWND gDialog = nullptr;

namespace {

int gFailures = 0;

void Check(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    std::fflush(stdout);
    if (!condition) ++gFailures;
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

std::vector<unsigned char> Scenario(const std::vector<unsigned char>& body) {
    std::vector<unsigned char> bytes(10, 0);
    bytes[0] = 'E'; bytes[1] = 'E'; bytes[2] = 'X';
    AppendI32(bytes, 14); // One scene starts immediately after its offset table.
    AppendU16(bytes, 1); // One section.
    AppendU16(bytes, static_cast<unsigned>(4 + body.size()));
    AppendU16(bytes, 0); // End the event header and start its child block.
    AppendU16(bytes, static_cast<unsigned>(body.size()));
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::vector<unsigned char> RewardScenario(int money) {
    std::vector<unsigned char> body;
    AppendU16(body, 0x59);
    const int values[] = {money, 255, 0, 255, 0, 255, 0, 0};
    for (size_t index = 0; index < _countof(values); ++index) {
        AppendU16(body, 0); // Constant parameter tag.
        if (index == 0) AppendI32(body, values[index]);
        else AppendU16(body, static_cast<unsigned>(values[index]));
    }
    return Scenario(body);
}

std::vector<unsigned char> JumpScenario(int battlefieldNumber) {
    std::vector<unsigned char> body;
    AppendU16(body, 0x11);
    AppendU16(body, 0);
    AppendU16(body, static_cast<unsigned>(battlefieldNumber * 2));
    return Scenario(body);
}

std::vector<unsigned char> NoGuideScenario() {
    std::vector<unsigned char> body;
    AppendU16(body, 0x09); // A real delay command produces no guide entry.
    AppendU16(body, 0);
    AppendI32(body, 1);
    return Scenario(body);
}

bool WriteBytes(const std::wstring& path, const std::vector<unsigned char>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool wrote = bytes.empty() || (WriteFile(file, bytes.data(),
        static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE && written == bytes.size());
    const bool flushed = wrote && FlushFileBuffers(file) != FALSE;
    return CloseHandle(file) != FALSE && flushed;
}

bool WriteSized(const std::wstring& path, LONGLONG size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER offset = {};
    offset.QuadPart = size;
    const bool okay = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) != FALSE &&
        SetEndOfFile(file) != FALSE && FlushFileBuffers(file) != FALSE;
    return CloseHandle(file) != FALSE && okay;
}

bool ReadBytes(const std::wstring& path, std::vector<unsigned char>& bytes) {
    bytes.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length = {};
    bool okay = GetFileSizeEx(file, &length) != FALSE && length.QuadPart >= 0 &&
        length.QuadPart <= 64 * 1024 * 1024;
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

std::vector<unsigned char> Encode(const std::wstring& text, UINT codePage, bool bom) {
    if (codePage == 1200) {
        std::vector<unsigned char> bytes;
        if (bom) { bytes.push_back(0xff); bytes.push_back(0xfe); }
        for (wchar_t ch : text) AppendU16(bytes, static_cast<unsigned>(ch));
        return bytes;
    }
    const int count = WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0 && !text.empty()) return {};
    std::vector<unsigned char> bytes;
    if (bom && codePage == CP_UTF8) {
        bytes.push_back(0xef); bytes.push_back(0xbb); bytes.push_back(0xbf);
    }
    const size_t offset = bytes.size();
    bytes.resize(offset + static_cast<size_t>(count));
    if (count > 0 && WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()),
        reinterpret_cast<char*>(bytes.data() + offset), count, nullptr, nullptr) != count) return {};
    return bytes;
}

bool HasOnlyCrlf(const std::wstring& text) {
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r' && (index + 1 == text.size() || text[index + 1] != L'\n')) return false;
        if (text[index] == L'\n' && (index == 0 || text[index - 1] != L'\r')) return false;
    }
    return true;
}

size_t Occurrences(const std::wstring& text, const std::wstring& needle) {
    size_t count = 0;
    for (size_t at = 0; (at = text.find(needle, at)) != std::wstring::npos; at += needle.size()) ++count;
    return count;
}

size_t TemporaryFiles(const std::wstring& root) {
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW((root + L"\\简易攻略.txt.tmp-*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return 0;
    size_t count = 0;
    do { if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++count; }
    while (FindNextFileW(find, &data));
    FindClose(find);
    return count;
}

bool Display(const std::wstring& root, const wchar_t* scenario, std::wstring& text, bool& authored) {
    return BuildGuideDisplayTextForTest((root + L"\\RS\\" + scenario).c_str(),
        root.c_str(), text, authored);
}

void ValidateEncoding() {
    const std::wstring source = L"第00关简易攻略\n作者攻略。\r第二行\r\n\r\n末行";
    const std::wstring expected = L"第00关简易攻略\r\n作者攻略。\r\n第二行\r\n\r\n末行";
    const UINT codePages[] = {CP_UTF8, CP_UTF8, 936, 1200};
    const bool boms[] = {true, false, false, true};
    const char* labels[] = {
        "decode UTF-8 BOM and preserve authored paragraphs",
        "decode UTF-8 without BOM independently of system locale",
        "decode CP936 authored text independently of system locale",
        "decode UTF-16LE BOM and normalize CR/LF/CRLF"
    };
    for (size_t index = 0; index < _countof(codePages); ++index) {
        std::wstring decoded;
        Check(DecodeGuideDocumentForTest(Encode(source, codePages[index], boms[index]), decoded) &&
            decoded == expected && HasOnlyCrlf(decoded), labels[index]);
    }
    const std::vector<std::vector<unsigned char>> invalid = {
        {0xef, 0xbb, 0xbf, 0xff},
        {0xff, 0xfe, 0x41},
        {0xff, 0xfe, 0x00, 0xd8},
        {0xff, 0xfe, 0x00, 0xdc},
        {0xef, 0xbb, 0xbf, 'A', 0, 'B'}
    };
    for (size_t index = 0; index < invalid.size(); ++index) {
        std::wstring decoded;
        const bool rejected = !DecodeGuideDocumentForTest(invalid[index], decoded);
        std::printf("%s reject malformed document encoding %zu\n", rejected ? "PASS" : "FAIL", index);
        if (!rejected) ++gFailures;
    }
}

void ValidateDisplay(const std::wstring& root) {
    const std::wstring document = root + L"\\简易攻略.txt";
    const std::wstring authoredText =
        L"第00关简易攻略\n作者零关攻略。\n\n保留作者空行。\n"
        L"第02关简易攻略\n作者二关攻略。";
    std::wstring text;
    bool authored = true;
    Check(Display(root, L"S_00.eex", text, authored) && !authored &&
        text.find(L"100 金钱") != std::wstring::npos, "missing document falls back to generated current battlefield");
    Check(WriteBytes(document, Encode(authoredText, CP_UTF8, true)), "write isolated author guide fixture");
    Check(Display(root, L"S_00.eex", text, authored) && authored &&
        text.find(L"作者零关攻略。\r\n\r\n保留作者空行。") != std::wstring::npos &&
        text.find(L"100 金钱") == std::wstring::npos, "matching author section wins and preserves free-form paragraphs");
    Check(Display(root, L"R_00.eex", text, authored) && authored &&
        text.find(L"第 02 关简易攻略") != std::wstring::npos &&
        text.find(L"作者二关攻略。") != std::wstring::npos,
        "R script selects authored section for its resolved S battlefield");
    Check(Display(root, L"S_06.eex", text, authored) && !authored &&
        text.find(L"本关暂无可整理的攻略信息。") != std::wstring::npos,
        "missing author section keeps generated fallback for that battlefield");

    HANDLE locked = CreateFileW(document.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock author document against reads");
    Check(Display(root, L"S_00.eex", text, authored) && !authored &&
        text.find(L"100 金钱") != std::wstring::npos, "unreadable author document falls back without losing automatic guide");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);

    Check(WriteBytes(document, {}), "write empty author document");
    Check(Display(root, L"S_00.eex", text, authored) && !authored &&
        text.find(L"100 金钱") != std::wstring::npos, "empty document falls back to automatic guide");
    Check(WriteBytes(document, Encode(L" \t\r\n\r\n", CP_UTF8, true)) &&
        Display(root, L"S_00.eex", text, authored) && !authored,
        "whitespace-only author document falls back");
    Check(WriteBytes(document, Encode(L"第00关简易攻略\r\n \t\r\n第02关简易攻略\r\n二关正文。", CP_UTF8, true)) &&
        Display(root, L"S_00.eex", text, authored) && !authored &&
        text.find(L"100 金钱") != std::wstring::npos, "empty matching section falls back even when another section has text");
    Check(WriteBytes(document, {0xff, 0xfe, 0x41}) &&
        Display(root, L"S_00.eex", text, authored) && !authored,
        "malformed encoded author document falls back");
    Check(WriteSized(document, 64ll * 1024 * 1024 + 1) &&
        Display(root, L"S_00.eex", text, authored) && !authored &&
        text.find(L"100 金钱") != std::wstring::npos,
        "oversized authored document is rejected before automatic fallback");

    const UINT encodings[] = {CP_UTF8, 936, 1200};
    for (UINT encoding : encodings) {
        Check(WriteBytes(document, Encode(authoredText, encoding, encoding != 936)) &&
            Display(root, L"S_00.eex", text, authored) && authored &&
            text.find(L"作者零关攻略。") != std::wstring::npos,
            "display reads authored UTF-8/CP936/UTF-16LE files through production file path");
    }
}

void ValidateExport(const std::wstring& root) {
    const std::wstring document = root + L"\\简易攻略.txt";
    const auto original = Encode(L"第00关简易攻略\r\n必须保留的作者原稿。", CP_UTF8, true);
    Check(WriteBytes(document, original), "prepare old author document before atomic replacement failure");
    HANDLE locked = CreateFileW(document.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock existing document against replacement");
    size_t success = 0;
    std::vector<int> skipped;
    std::wstring error;
    Check(!ExportGuideBookForTest(root.c_str(), success, skipped, error) && !error.empty(),
        "export reports atomic replacement failure");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    std::vector<unsigned char> preserved;
    Check(ReadBytes(document, preserved) && preserved == original && TemporaryFiles(root) == 0,
        "failed replacement preserves original bytes and removes temporary file");

    Check(ExportGuideBookForTest(root.c_str(), success, skipped, error) && error.empty() && success == 3,
        "export succeeds for each distinct valid nonempty S script");
    Check(skipped == std::vector<int>({3, 4}),
        "empty S files are silent while corrupt or command-empty nonempty files are reported");
    std::vector<unsigned char> bytes;
    std::wstring decoded;
    Check(ReadBytes(document, bytes) && bytes.size() > 3 && bytes[0] == 0xef && bytes[1] == 0xbb &&
        bytes[2] == 0xbf && DecodeGuideDocumentForTest(bytes, decoded) && HasOnlyCrlf(decoded),
        "export writes UTF-8 BOM with CRLF line endings");
    const size_t first = decoded.find(L"第 00 关简易攻略");
    const size_t second = decoded.find(L"第 02 关简易攻略");
    const size_t third = decoded.find(L"第 06 关简易攻略");
    Check(first == 0 && first < second && second < third &&
        Occurrences(decoded, L"第 02 关简易攻略") == 1 &&
        decoded.find(L"200 金钱") != std::wstring::npos && decoded.find(L"999 金钱") == std::wstring::npos,
        "export sorts actual S numbers and prefers canonical name over valid alias");
    Check(decoded.find(L"第 01 关简易攻略") == std::wstring::npos &&
        decoded.find(L"第 03 关简易攻略") == std::wstring::npos &&
        decoded.find(L"第 04 关简易攻略") == std::wstring::npos &&
        decoded.find(L"作者原稿") == std::wstring::npos &&
        decoded.find(L"本关暂无可整理的攻略信息。") != std::wstring::npos,
        "export uses current S instructions and keeps placeholder for valid script without useful entries");
    bool authored = false;
    std::wstring displayed;
    Check(Display(root, L"S_02.eex", displayed, authored) && authored &&
        displayed.find(L"200 金钱") != std::wstring::npos && TemporaryFiles(root) == 0,
        "exported document is immediately readable through authored display path");
}

void ValidateNoUsableScripts(const std::wstring& root) {
    std::error_code errorCode;
    std::filesystem::create_directories(root + L"\\RS", errorCode);
    const std::wstring document = root + L"\\简易攻略.txt";
    const auto original = Encode(L"第00关简易攻略\r\n作者原稿必须保留。", CP_UTF8, true);
    Check(!errorCode && WriteBytes(root + L"\\RS\\S_00.eex", {}) &&
        WriteBytes(document, original), "prepare isolated root containing only an empty S script");
    size_t success = 99;
    std::vector<int> skipped;
    std::wstring error;
    std::vector<unsigned char> after;
    Check(!ExportGuideBookForTest(root.c_str(), success, skipped, error) && success == 0 &&
        skipped.empty() && !error.empty() && ReadBytes(document, after) && after == original &&
        TemporaryFiles(root) == 0, "no usable S script leaves existing author document unchanged");
}

bool Prepare(const std::wstring& root) {
    std::error_code error;
    std::filesystem::create_directories(root + L"\\RS", error);
    if (error) return false;
    std::vector<unsigned char> emptyScene(10, 0);
    emptyScene[0] = 'E'; emptyScene[1] = 'E'; emptyScene[2] = 'X';
    AppendI32(emptyScene, 14);
    AppendU16(emptyScene, 0);
    return WriteBytes(root + L"\\RS\\S_00.eex", RewardScenario(100)) &&
        WriteBytes(root + L"\\RS\\S_01.eex", {}) &&
        WriteBytes(root + L"\\RS\\S_02.eex", RewardScenario(200)) &&
        WriteBytes(root + L"\\RS\\S_2.eex", RewardScenario(999)) &&
        WriteBytes(root + L"\\RS\\S_002.eex", {'b', 'a', 'd'}) &&
        WriteBytes(root + L"\\RS\\S_03.eex", {'b', 'a', 'd'}) &&
        WriteBytes(root + L"\\RS\\S_04.eex", emptyScene) &&
        WriteBytes(root + L"\\RS\\S_06.eex", NoGuideScenario()) &&
        WriteBytes(root + L"\\RS\\R_00.eex", JumpScenario(2)) &&
        WriteBytes(root + L"\\RS\\S_backup.eex", RewardScenario(999));
}

void PrintUtf8(const std::wstring& text) {
    const auto bytes = Encode(text, CP_UTF8, false);
    if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
}

int ExportRoot(const wchar_t* root) {
    size_t success = 0;
    std::vector<int> skipped;
    std::wstring error;
    const bool exported = ExportGuideBookForTest(root, success, skipped, error);
    PrintUtf8(L"Export root: " + std::wstring(root) + L"\nSuccess: " +
        std::to_wstring(success) + L"\nSkipped: " + std::to_wstring(skipped.size()) + L"\n");
    for (int number : skipped) {
        wchar_t name[32] = {};
        swprintf_s(name, L"S_%02d.eex", number);
        PrintUtf8(L"Skipped path: " + (std::filesystem::path(root) / L"RS" / name).wstring() + L"\n");
    }
    if (!exported) PrintUtf8(L"Error: " + error + L"\n");
    return exported ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 3 && std::wcscmp(argv[1], L"--export-root") == 0) return ExportRoot(argv[2]);
    if (argc != 1) {
        std::printf("Usage: GuideBookFileTests.exe [--export-root <isolated-game-root>]\n");
        return 2;
    }
    wchar_t temporary[32768] = {};
    const DWORD length = GetTempPathW(static_cast<DWORD>(_countof(temporary)), temporary);
    if (!length || length >= _countof(temporary)) return 2;
    const std::wstring root = std::wstring(temporary) + L"CczGuideBookFileTests-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    gInstance = GetModuleHandleW(nullptr);
    Check(Prepare(root), "create self-contained minimal EEX fixtures in a new temporary directory");
    if (!gFailures) {
        ValidateEncoding();
        ValidateDisplay(root);
        ValidateExport(root);
        ValidateNoUsableScripts(root + L"\\empty-game");
    }
    std::printf("GuideBookFileTests: %d failures\n", gFailures);
    std::wprintf(L"Temporary test evidence: %ls\n", root.c_str());
    return gFailures ? 1 : 0;
}
