#include "InstallCommon.h"

#include <bcrypt.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <sstream>
#include <stdio.h>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")

namespace XiaoAnInstall {
namespace {

constexpr const wchar_t* kPrivateRelative = L"patch\\xiaoan66";
constexpr const wchar_t* kBackupRelative = L"patch\\xiaoan66\\backup";
constexpr const wchar_t* kManifestRelative = L"patch\\xiaoan66\\install.ini";
constexpr const wchar_t* kConfigRelative = L"patch\\xiaoan66\\pl_06.ini";
constexpr const wchar_t* kFeaturesRelative = L"patch\\xiaoan66\\features.ini";
constexpr const wchar_t* kAudioNames[] = {
    L"CDAudioClose", L"CDAudioEjectMedia", L"CDAudioGetDriveLetter", L"CDAudioGetNumDrives",
    L"CDAudioGetNumTracks", L"CDAudioGetPlayingPos", L"CDAudioGetStartPos", L"CDAudioGetTrackLength",
    L"CDAudioInitialize", L"CDAudioIsInserted", L"CDAudioIsPlaying", L"CDAudioNextPlay",
    L"CDAudioNextPlayTrack", L"CDAudioOpen", L"CDAudioPause", L"CDAudioPlay", L"CDAudioPlayTrack",
    L"CDAudioResume", L"CDAudioStop", L"CDAudioTerminate"
};

std::wstring WinError(DWORD value) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, value, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result = text ? text : L"未知错误";
    if (text) LocalFree(text);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
        result.pop_back();
    return result;
}

std::wstring Failure(const wchar_t* action, const std::wstring& path, DWORD code) {
    std::wstringstream stream;
    stream << action << L"：" << path << L"\r\nWindows 错误 " << code << L"：" << WinError(code);
    return stream.str();
}

bool ReadWholeFile(const std::wstring& path, std::vector<unsigned char>& bytes, std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = Failure(L"无法读取文件", path, GetLastError());
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 512LL * 1024 * 1024) {
        const DWORD code = GetLastError();
        CloseHandle(file);
        error = Failure(L"无法取得文件大小", path, code ? code : ERROR_FILE_TOO_LARGE);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        const DWORD request = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        if (!ReadFile(file, bytes.data() + offset, request, &read, nullptr) || !read) {
            const DWORD code = GetLastError();
            CloseHandle(file);
            error = Failure(L"读取文件失败", path, code ? code : ERROR_READ_FAULT);
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

bool EnsureDirectory(const std::wstring& path, std::wstring& error) {
    if (DirectoryExists(path)) return true;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2 && !EnsureDirectory(path.substr(0, slash), error)) return false;
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    error = Failure(L"无法创建目录", path, GetLastError());
    return false;
}

bool DirectoryEmpty(const std::wstring& path) {
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(JoinPath(path, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool empty = true;
    do {
        if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L"..")) {
            empty = false;
            break;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return empty;
}

bool RemoveKnownTree(const std::wstring& path) {
    if (!DirectoryExists(path)) return true;
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(JoinPath(path, L"*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            const std::wstring child = JoinPath(path, data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveKnownTree(child);
            else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(path.c_str()) != 0 || GetLastError() == ERROR_PATH_NOT_FOUND;
}

uint64_t FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result = {};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

FILETIME ToFileTime(uint64_t value) {
    ULARGE_INTEGER raw = {};
    raw.QuadPart = value;
    FILETIME result = {raw.LowPart, raw.HighPart};
    return result;
}

bool CaptureFile(const std::wstring& source, const std::wstring& relativeTarget,
                 const std::wstring& backup, FileRecord& record, std::wstring& error) {
    record = {};
    record.path = relativeTarget;
    record.backup = backup;
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &data)) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
            record.existed = false;
            return true;
        }
        error = Failure(L"无法读取文件属性", source, GetLastError());
        return false;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        error = L"预期文件却发现目录：" + source;
        return false;
    }
    record.existed = true;
    record.attributes = data.dwFileAttributes;
    record.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    record.creationTime = FileTimeValue(data.ftCreationTime);
    record.accessTime = FileTimeValue(data.ftLastAccessTime);
    record.writeTime = FileTimeValue(data.ftLastWriteTime);
    return Sha256File(source, record.sha256, error);
}

bool CopyPreserving(const std::wstring& source, const std::wstring& target, std::wstring& error) {
    const size_t slash = target.find_last_of(L"\\/");
    if (slash != std::wstring::npos && !EnsureDirectory(target.substr(0, slash), error)) return false;
    if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
        error = Failure(L"无法复制文件", source + L" -> " + target, GetLastError());
        return false;
    }
    return true;
}

bool ApplyMetadata(const std::wstring& path, const FileRecord& record, std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = Failure(L"无法恢复文件时间", path, GetLastError());
        return false;
    }
    const FILETIME creation = ToFileTime(record.creationTime);
    const FILETIME access = ToFileTime(record.accessTime);
    const FILETIME write = ToFileTime(record.writeTime);
    const bool times = SetFileTime(file, &creation, &access, &write) != 0;
    const DWORD timeError = times ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!times) {
        error = Failure(L"无法恢复文件时间", path, timeError);
        return false;
    }
    if (!SetFileAttributesW(path.c_str(), record.attributes)) {
        error = Failure(L"无法恢复文件属性", path, GetLastError());
        return false;
    }
    return true;
}

bool HashBytes(const unsigned char* data, size_t size, std::wstring& hash, std::wstring& error) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE object = nullptr;
    DWORD objectSize = 0, hashSize = 0, resultSize = 0;
    std::vector<unsigned char> objectBuffer, digest;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) goto failed;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                               sizeof(objectSize), &resultSize, 0);
    if (status < 0) goto failed;
    status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize),
                               sizeof(hashSize), &resultSize, 0);
    if (status < 0) goto failed;
    objectBuffer.resize(objectSize);
    digest.resize(hashSize);
    status = BCryptCreateHash(algorithm, &object, objectBuffer.data(), objectSize, nullptr, 0, 0);
    if (status < 0) goto failed;
    for (size_t offset = 0; offset < size;) {
        const ULONG chunk = static_cast<ULONG>(std::min<size_t>(size - offset, 1u << 20));
        status = BCryptHashData(object, const_cast<PUCHAR>(data + offset), chunk, 0);
        if (status < 0) goto failed;
        offset += chunk;
    }
    status = BCryptFinishHash(object, digest.data(), hashSize, 0);
    if (status < 0) goto failed;
    {
        static constexpr wchar_t digits[] = L"0123456789ABCDEF";
        hash.clear();
        hash.reserve(digest.size() * 2);
        for (unsigned char byte : digest) {
            hash.push_back(digits[byte >> 4]);
            hash.push_back(digits[byte & 15]);
        }
    }
    BCryptDestroyHash(object);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return true;
failed:
    if (object) BCryptDestroyHash(object);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    {
        std::wstringstream stream;
        stream << L"SHA-256 计算失败，NTSTATUS=0x" << std::hex << static_cast<unsigned long>(status);
        error = stream.str();
    }
    return false;
}

struct PeImage {
    const std::vector<unsigned char>* bytes = nullptr;
    const IMAGE_NT_HEADERS32* nt = nullptr;
    IMAGE_DATA_DIRECTORY exports = {};

    const unsigned char* At(size_t offset, size_t length = 1) const {
        if (!bytes || offset > bytes->size() || length > bytes->size() - offset) return nullptr;
        return bytes->data() + offset;
    }

    bool Rva(DWORD rva, size_t length, size_t& offset) const {
        if (rva < nt->OptionalHeader.SizeOfHeaders) {
            offset = rva;
            return At(offset, length) != nullptr;
        }
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const DWORD span = std::max(section[i].Misc.VirtualSize, section[i].SizeOfRawData);
            if (rva < section[i].VirtualAddress || rva - section[i].VirtualAddress >= span) continue;
            const uint64_t raw = static_cast<uint64_t>(section[i].PointerToRawData) +
                                 (rva - section[i].VirtualAddress);
            if (raw > SIZE_MAX) return false;
            offset = static_cast<size_t>(raw);
            return At(offset, length) != nullptr;
        }
        return false;
    }

    const char* StringAtRva(DWORD rva) const {
        size_t offset = 0;
        if (!Rva(rva, 1, offset)) return nullptr;
        const char* value = reinterpret_cast<const char*>(bytes->data() + offset);
        return memchr(value, 0, bytes->size() - offset) ? value : nullptr;
    }
};

bool ParsePe(const std::vector<unsigned char>& bytes, PeImage& image, std::wstring& error) {
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) {
        error = L"文件过小，不是有效的 PE 文件。";
        return false;
    }
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) > bytes.size() - sizeof(IMAGE_NT_HEADERS32)) {
        error = L"DOS/PE 头无效。";
        return false;
    }
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        error = L"目标不是 x86 PE32 文件。";
        return false;
    }
    const size_t sectionEnd = reinterpret_cast<const unsigned char*>(IMAGE_FIRST_SECTION(nt) +
                              nt->FileHeader.NumberOfSections) - bytes.data();
    if (sectionEnd > bytes.size()) {
        error = L"PE 节表越界。";
        return false;
    }
    image.bytes = &bytes;
    image.nt = nt;
    image.exports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    return true;
}

bool ValidateAudioBytes(const std::vector<unsigned char>& bytes, bool proxy, std::wstring& error) {
    PeImage image;
    if (!ParsePe(bytes, image, error)) return false;
    if (!image.exports.VirtualAddress || !image.exports.Size) {
        error = L"音频 DLL 缺少导出表。";
        return false;
    }
    size_t exportOffset = 0;
    if (!image.Rva(image.exports.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), exportOffset)) {
        error = L"音频 DLL 导出目录越界。";
        return false;
    }
    const auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image.At(exportOffset));
    if (exports->Base != 1 || exports->NumberOfFunctions != 20 || exports->NumberOfNames != 20) {
        error = L"音频 DLL 必须正好包含序号 1 至 20 的 20 个导出。";
        return false;
    }
    size_t namesOffset = 0, ordinalsOffset = 0, functionsOffset = 0;
    if (!image.Rva(exports->AddressOfNames, 20 * sizeof(DWORD), namesOffset) ||
        !image.Rva(exports->AddressOfNameOrdinals, 20 * sizeof(WORD), ordinalsOffset) ||
        !image.Rva(exports->AddressOfFunctions, 20 * sizeof(DWORD), functionsOffset)) {
        error = L"音频 DLL 导出数组越界。";
        return false;
    }
    const auto names = reinterpret_cast<const DWORD*>(image.At(namesOffset));
    const auto ordinals = reinterpret_cast<const WORD*>(image.At(ordinalsOffset));
    const auto functions = reinterpret_cast<const DWORD*>(image.At(functionsOffset));
    for (size_t expected = 0; expected < 20; ++expected) {
        std::string wanted;
        for (const wchar_t* p = kAudioNames[expected]; *p; ++p) wanted.push_back(static_cast<char>(*p));
        bool found = false;
        for (DWORD n = 0; n < exports->NumberOfNames; ++n) {
            const char* name = image.StringAtRva(names[n]);
            if (name && wanted == name) {
                if (ordinals[n] != expected) {
                    error = L"音频 DLL 的导出序号与名称不匹配。";
                    return false;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            error = L"音频 DLL 缺少导出：" + std::wstring(kAudioNames[expected]);
            return false;
        }
        const DWORD function = functions[expected];
        const bool forwarded = function >= image.exports.VirtualAddress &&
                               function - image.exports.VirtualAddress < image.exports.Size;
        if (proxy) {
            const char* forwarder = image.StringAtRva(function);
            const std::string expectedForwarder = "Koeicda_org." + wanted;
            if (!forwarded || !forwarder || expectedForwarder != forwarder) {
                error = L"代理 DLL 的转发目标不正确：" + std::wstring(kAudioNames[expected]);
                return false;
            }
        } else if (!function || forwarded) {
            error = L"原始音频 DLL 含转发导出，不能作为恢复基线。";
            return false;
        }
    }
    return true;
}

bool ValidateGearBytesInternal(const std::vector<unsigned char>& bytes, std::wstring& error) {
    PeImage image;
    if (!ParsePe(bytes, image, error)) return false;
    if (!image.exports.VirtualAddress || !image.exports.Size) {
        error = L"齿轮 DLL 缺少导出表。";
        return false;
    }
    size_t exportOffset = 0;
    if (!image.Rva(image.exports.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), exportOffset)) {
        error = L"齿轮 DLL 导出目录越界。";
        return false;
    }
    const auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image.At(exportOffset));
    if (exports->Base != 1 || exports->NumberOfFunctions != 5 || exports->NumberOfNames != 5) {
        error = L"齿轮 DLL 必须正好包含 5 个导出。";
        return false;
    }
    constexpr const char* expected[] = {"AddProcess", "GetSpeed", "OldSetTimer", "SetSpeed", "UnIntercept"};
    size_t namesOffset = 0, ordinalsOffset = 0;
    if (!image.Rva(exports->AddressOfNames, 5 * sizeof(DWORD), namesOffset) ||
        !image.Rva(exports->AddressOfNameOrdinals, 5 * sizeof(WORD), ordinalsOffset)) {
        error = L"齿轮 DLL 导出数组越界。";
        return false;
    }
    const auto names = reinterpret_cast<const DWORD*>(image.At(namesOffset));
    const auto ordinals = reinterpret_cast<const WORD*>(image.At(ordinalsOffset));
    for (size_t i = 0; i < 5; ++i) {
        bool found = false;
        for (DWORD n = 0; n < exports->NumberOfNames; ++n) {
            const char* name = image.StringAtRva(names[n]);
            if (name && !strcmp(name, expected[i])) {
                if (ordinals[n] != i) {
                    error = L"齿轮 DLL 的导出序号不正确。";
                    return false;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            error = L"齿轮 DLL 导出身份不完整。";
            return false;
        }
    }
    return true;
}

std::wstring HexId() {
    GUID guid = {};
    if (FAILED(CoCreateGuid(&guid))) {
        wchar_t fallback[64];
        swprintf_s(fallback, L"%08lX-%08lX", GetCurrentProcessId(), GetTickCount());
        return fallback;
    }
    wchar_t value[64] = {};
    StringFromGUID2(guid, value, static_cast<int>(_countof(value)));
    std::wstring result = value;
    result.erase(std::remove(result.begin(), result.end(), L'{'), result.end());
    result.erase(std::remove(result.begin(), result.end(), L'}'), result.end());
    return result;
}

std::wstring BoolText(bool value) { return value ? L"1" : L"0"; }

void AddRecord(std::wstringstream& output, const wchar_t* section, const FileRecord& record) {
    output << L"\r\n[" << section << L"]\r\n";
    output << L"Path=" << record.path << L"\r\n";
    output << L"Existed=" << BoolText(record.existed) << L"\r\n";
    output << L"Backup=" << record.backup << L"\r\n";
    output << L"Size=" << record.size << L"\r\n";
    output << L"Sha256=" << record.sha256 << L"\r\n";
    output << L"Attributes=" << record.attributes << L"\r\n";
    output << L"CreationTime=" << record.creationTime << L"\r\n";
    output << L"LastAccessTime=" << record.accessTime << L"\r\n";
    output << L"LastWriteTime=" << record.writeTime << L"\r\n";
}

bool SaveManifestFile(const std::wstring& path, const Manifest& manifest, std::wstring& error) {
    std::wstringstream output;
    output << L"[Install]\r\n";
    output << L"SchemaVersion=" << manifest.schemaVersion << L"\r\n";
    output << L"PackageVersion=" << manifest.packageVersion << L"\r\n";
    output << L"InstallId=" << manifest.installId << L"\r\n";
    output << L"InstallMode=" << (manifest.mode == InstallMode::Clean ? L"Clean" : L"LegacyAdopted") << L"\r\n";
    output << L"Root=" << manifest.root << L"\r\n";
    output << L"PatchDirExisted=" << BoolText(manifest.patchDirExisted) << L"\r\n";
    output << L"PrivateDirExisted=" << BoolText(manifest.privateDirExisted) << L"\r\n";
    AddRecord(output, L"File.Koeicda", manifest.files[0]);
    AddRecord(output, L"File.KoeicdaOrg", manifest.files[1]);
    AddRecord(output, L"File.Gear", manifest.files[2]);
    output << L"\r\n[Installed]\r\n";
    output << L"ProxySha256=" << manifest.proxySha256 << L"\r\n";
    output << L"GearSha256=" << manifest.gearSha256 << L"\r\n";
    output << L"OriginalCopySha256=" << manifest.originalCopySha256 << L"\r\n";
    const std::wstring text = output.str();
    const std::wstring temporary = path + L".tmp";
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && !EnsureDirectory(path.substr(0, slash), error)) return false;
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = Failure(L"无法写入安装清单", temporary, GetLastError());
        return false;
    }
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    if (ok && bytes) ok = WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes;
    if (ok) ok = FlushFileBuffers(file) != 0;
    const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveCode = ok ? GetLastError() : code;
        DeleteFileW(temporary.c_str());
        error = Failure(L"提交安装清单失败", path, moveCode);
        return false;
    }
    return true;
}

bool ParseUnsigned(const std::wstring& text, uint64_t& value) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long parsed = wcstoull(text.c_str(), &end, 10);
    if (errno || !end || *end) return false;
    value = parsed;
    return true;
}

bool ReadManifestMap(const std::wstring& path, std::map<std::wstring, std::wstring>& values,
                     std::wstring& error) {
    std::vector<unsigned char> bytes;
    if (!ReadWholeFile(path, bytes, error)) return false;
    if (bytes.size() < 2 || bytes.size() % 2 || *reinterpret_cast<const wchar_t*>(bytes.data()) != 0xFEFF) {
        error = L"安装清单不是受支持的 UTF-16 格式：" + path;
        return false;
    }
    std::wstring text(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
    std::wstring section;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find_first_of(L"\r\n", start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        while (end < text.size() && (text[end] == L'\r' || text[end] == L'\n')) ++end;
        start = end;
        if (line.empty()) continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos || section.empty()) {
            error = L"安装清单包含无效行：" + line;
            return false;
        }
        values[section + L"/" + line.substr(0, equals)] = line.substr(equals + 1);
    }
    return true;
}

bool GetRequired(const std::map<std::wstring, std::wstring>& values, const std::wstring& key,
                 std::wstring& value, std::wstring& error) {
    const auto found = values.find(key);
    if (found == values.end()) {
        error = L"安装清单缺少字段：" + key;
        return false;
    }
    value = found->second;
    return true;
}

bool LoadRecord(const std::map<std::wstring, std::wstring>& values, const wchar_t* section,
                FileRecord& record, std::wstring& error) {
    const std::wstring prefix = std::wstring(section) + L"/";
    std::wstring existed, size, attributes, creation, access, write;
    if (!GetRequired(values, prefix + L"Path", record.path, error) ||
        !GetRequired(values, prefix + L"Existed", existed, error) ||
        !GetRequired(values, prefix + L"Backup", record.backup, error) ||
        !GetRequired(values, prefix + L"Size", size, error) ||
        !GetRequired(values, prefix + L"Sha256", record.sha256, error) ||
        !GetRequired(values, prefix + L"Attributes", attributes, error) ||
        !GetRequired(values, prefix + L"CreationTime", creation, error) ||
        !GetRequired(values, prefix + L"LastAccessTime", access, error) ||
        !GetRequired(values, prefix + L"LastWriteTime", write, error)) return false;
    uint64_t existedValue = 0, attributesValue = 0;
    if (!ParseUnsigned(existed, existedValue) || existedValue > 1 || !ParseUnsigned(size, record.size) ||
        !ParseUnsigned(attributes, attributesValue) || attributesValue > UINT32_MAX ||
        !ParseUnsigned(creation, record.creationTime) || !ParseUnsigned(access, record.accessTime) ||
        !ParseUnsigned(write, record.writeTime)) {
        error = L"安装清单的数值字段无效：" + prefix;
        return false;
    }
    record.existed = existedValue != 0;
    record.attributes = static_cast<DWORD>(attributesValue);
    return true;
}

bool IsSafeRelative(const std::wstring& value) {
    return !value.empty() && value.find(L":") == std::wstring::npos &&
           value.front() != L'\\' && value.front() != L'/' &&
           value.find(L"..") == std::wstring::npos;
}

bool IsHexHash(const std::wstring& value) {
    if (value.size() != 64) return false;
    for (wchar_t ch : value) if (!((ch >= L'0' && ch <= L'9') ||
        (ch >= L'A' && ch <= L'F') || (ch >= L'a' && ch <= L'f'))) return false;
    return true;
}

bool IsSafeInstallId(const std::wstring& value) {
    if (value.empty() || value.size() > 64) return false;
    for (wchar_t ch : value) if (!((ch >= L'0' && ch <= L'9') ||
        (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') || ch == L'-')) return false;
    return true;
}

struct Change {
    std::wstring target;
    std::wstring payload;
    std::wstring rollback;
    bool hadTarget = false;
    bool applied = false;
};

void RollbackChanges(std::vector<Change>& changes) {
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        if (!it->applied) continue;
        SetFileAttributesW(it->target.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(it->target.c_str());
        if (it->hadTarget) MoveFileExW(it->rollback.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
}

bool BeginChanges(std::vector<Change>& changes, std::wstring& error) {
    for (size_t i = 0; i < changes.size(); ++i) {
        Change& change = changes[i];
        change.hadTarget = FileExists(change.target);
        if (change.hadTarget) {
            SetFileAttributesW(change.target.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!MoveFileExW(change.target.c_str(), change.rollback.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                error = Failure(L"无法暂存现有文件", change.target, GetLastError());
                RollbackChanges(changes);
                return false;
            }
        }
        change.applied = true;
        if (!change.payload.empty() &&
            !MoveFileExW(change.payload.c_str(), change.target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = Failure(L"无法部署文件", change.target, GetLastError());
            RollbackChanges(changes);
            return false;
        }
    }
    return true;
}

void CommitChanges(std::vector<Change>& changes) {
    for (Change& change : changes) if (change.hadTarget) {
        SetFileAttributesW(change.rollback.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(change.rollback.c_str());
    }
}

bool VerifyFile(const std::wstring& path, const std::wstring& expected, uint64_t size,
                std::wstring& error) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        error = Failure(L"验证时找不到文件", path, GetLastError());
        return false;
    }
    const uint64_t actualSize = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    std::wstring actual;
    if (!Sha256File(path, actual, error)) return false;
    if (actualSize != size || _wcsicmp(actual.c_str(), expected.c_str())) {
        std::wstringstream stream;
        stream << L"文件校验失败：" << path << L"\r\n期望 SHA-256：" << expected
               << L"\r\n实际 SHA-256：" << actual;
        error = stream.str();
        return false;
    }
    return true;
}

bool ValidateConfig(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return false;
    size_t lines = 0;
    for (unsigned char value : bytes) if (value == '\n') ++lines;
    if (bytes.back() != '\n') ++lines;
    return lines == 74;
}

bool ImportLegacyConfig(const std::wstring& root, const std::vector<unsigned char>& defaults,
                        std::vector<unsigned char>& output) {
    output = defaults;
    std::vector<unsigned char> shared;
    std::wstring ignored;
    if (ReadWholeFile(JoinPath(root, L"patch\\pl_06.ini"), shared, ignored) && ValidateConfig(shared))
        output = shared;
    return true;
}

} // namespace

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    if (left.back() == L'\\' || left.back() == L'/') return left + right;
    return left + L"\\" + right;
}

std::wstring CanonicalRoot(const std::wstring& path) {
    if (path.empty()) return {};
    DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!required) return {};
    std::vector<wchar_t> buffer(required + 1);
    if (!GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr)) return {};
    std::wstring result = buffer.data();
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    return result;
}

std::wstring ManifestPath(const std::wstring& root) { return JoinPath(root, kManifestRelative); }

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsGameRunning(const std::wstring& root, std::wstring& error) {
    const std::wstring expected = CanonicalRoot(JoinPath(root, L"Ekd5.exe"));
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        error = Failure(L"无法枚举进程", L"Ekd5.exe", GetLastError());
        return true;
    }
    PROCESSENTRY32W entry = {sizeof(entry)};
    bool running = false;
    if (Process32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExeFile, L"Ekd5.exe")) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        std::vector<wchar_t> path(32768);
        DWORD length = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
            path[length] = 0;
            if (!_wcsicmp(CanonicalRoot(path.data()).c_str(), expected.c_str())) running = true;
        }
        CloseHandle(process);
        if (running) break;
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    if (running) error = L"游戏正在运行，请关闭游戏后重新安装";
    return running;
}

bool Sha256Bytes(const std::vector<unsigned char>& bytes, std::wstring& hash, std::wstring& error) {
    return HashBytes(bytes.data(), bytes.size(), hash, error);
}

bool Sha256File(const std::wstring& path, std::wstring& hash, std::wstring& error) {
    std::vector<unsigned char> bytes;
    return ReadWholeFile(path, bytes, error) && Sha256Bytes(bytes, hash, error);
}

bool ValidatePe32Executable(const std::wstring& path, std::wstring& error) {
    std::vector<unsigned char> bytes;
    PeImage image;
    if (!ReadWholeFile(path, bytes, error) || !ParsePe(bytes, image, error)) return false;
    if (image.nt->FileHeader.Characteristics & IMAGE_FILE_DLL) {
        error = L"Ekd5.exe 被识别为 DLL，而不是游戏主程序。";
        return false;
    }
    return true;
}

bool ValidateOriginalAudio(const std::wstring& path, std::wstring& error) {
    std::vector<unsigned char> bytes;
    return ReadWholeFile(path, bytes, error) && ValidateAudioBytes(bytes, false, error);
}

bool ValidateProxyAudio(const std::wstring& path, std::wstring& error) {
    std::vector<unsigned char> bytes;
    return ReadWholeFile(path, bytes, error) && ValidateAudioBytes(bytes, true, error);
}

bool ValidateGear(const std::wstring& path, std::wstring& error) {
    std::vector<unsigned char> bytes;
    return ReadWholeFile(path, bytes, error) && ValidateGearBytesInternal(bytes, error);
}

bool ValidateProxyAudioBytes(const std::vector<unsigned char>& bytes, std::wstring& error) {
    return ValidateAudioBytes(bytes, true, error);
}

bool ValidateGearBytes(const std::vector<unsigned char>& bytes, std::wstring& error) {
    return ValidateGearBytesInternal(bytes, error);
}

TargetKind DetectTarget(const std::wstring& rootValue, std::wstring& detail) {
    const std::wstring root = CanonicalRoot(rootValue);
    if (root.empty() || !DirectoryExists(root)) {
        detail = L"目标游戏目录不存在。";
        return TargetKind::Invalid;
    }
    std::wstring error;
    if (!ValidatePe32Executable(JoinPath(root, L"Ekd5.exe"), error)) {
        detail = L"目标目录缺少有效的 x86 Ekd5.exe。\r\n" + error;
        return TargetKind::Invalid;
    }
    if (FileExists(ManifestPath(root))) {
        Manifest manifest;
        if (!LoadManifest(root, manifest, error)) {
            detail = L"发现安装清单，但清单已损坏。\r\n" + error;
            return TargetKind::Invalid;
        }
        detail = L"已安装，可执行修复/更新或应急卸载。";
        return TargetKind::Installed;
    }
    const std::wstring proxy = JoinPath(root, L"Koeicda.dll");
    if (ValidateOriginalAudio(proxy, error)) {
        detail = L"检测到未安装补丁的原始音频 DLL。";
        return TargetKind::Clean;
    }
    const std::wstring original = JoinPath(root, L"Koeicda_org.dll");
    const std::wstring gear = JoinPath(root, L"GearNtKe.dll");
    std::wstring proxyError, originalError, gearError;
    if (ValidateProxyAudio(proxy, proxyError) && ValidateOriginalAudio(original, originalError) &&
        ValidateGear(gear, gearError)) {
        detail = L"检测到旧版松散文件安装，可执行有限迁移。";
        return TargetKind::Legacy;
    }
    detail = L"无法确认音频 DLL 与齿轮 DLL 的身份，已拒绝覆盖。\r\n代理检查：" + proxyError +
             L"\r\n原音频检查：" + originalError + L"\r\n齿轮检查：" + gearError;
    return TargetKind::Invalid;
}

bool LoadManifest(const std::wstring& rootValue, Manifest& manifest, std::wstring& error) {
    const std::wstring root = CanonicalRoot(rootValue);
    std::map<std::wstring, std::wstring> values;
    if (!ReadManifestMap(ManifestPath(root), values, error)) return false;
    std::wstring schema, mode, patchExisted, privateExisted;
    if (!GetRequired(values, L"Install/SchemaVersion", schema, error) ||
        !GetRequired(values, L"Install/PackageVersion", manifest.packageVersion, error) ||
        !GetRequired(values, L"Install/InstallId", manifest.installId, error) ||
        !GetRequired(values, L"Install/InstallMode", mode, error) ||
        !GetRequired(values, L"Install/Root", manifest.root, error) ||
        !GetRequired(values, L"Install/PatchDirExisted", patchExisted, error) ||
        !GetRequired(values, L"Install/PrivateDirExisted", privateExisted, error) ||
        !GetRequired(values, L"Installed/ProxySha256", manifest.proxySha256, error) ||
        !GetRequired(values, L"Installed/GearSha256", manifest.gearSha256, error) ||
        !GetRequired(values, L"Installed/OriginalCopySha256", manifest.originalCopySha256, error)) return false;
    uint64_t schemaValue = 0, patchValue = 0, privateValue = 0;
    if (!ParseUnsigned(schema, schemaValue) || schemaValue != kManifestSchema ||
        !ParseUnsigned(patchExisted, patchValue) || patchValue > 1 ||
        !ParseUnsigned(privateExisted, privateValue) || privateValue > 1) {
        error = L"安装清单版本或目录状态字段无效。";
        return false;
    }
    manifest.schemaVersion = static_cast<unsigned>(schemaValue);
    manifest.patchDirExisted = patchValue != 0;
    manifest.privateDirExisted = privateValue != 0;
    if (mode == L"Clean") manifest.mode = InstallMode::Clean;
    else if (mode == L"LegacyAdopted") manifest.mode = InstallMode::LegacyAdopted;
    else {
        error = L"安装清单包含未知 InstallMode。";
        return false;
    }
    if (_wcsicmp(CanonicalRoot(manifest.root).c_str(), root.c_str())) {
        error = L"安装清单记录的游戏目录与当前目录不一致。\r\n清单：" + manifest.root + L"\r\n当前：" + root;
        return false;
    }
    const wchar_t* sections[] = {L"File.Koeicda", L"File.KoeicdaOrg", L"File.Gear"};
    const wchar_t* expectedPaths[] = {L"Koeicda.dll", L"Koeicda_org.dll", L"GearNtKe.dll"};
    const wchar_t* expectedBackups[] = {
        L"patch\\xiaoan66\\backup\\Koeicda.dll.orig",
        L"patch\\xiaoan66\\backup\\Koeicda_org.dll.orig",
        L"patch\\xiaoan66\\backup\\GearNtKe.dll.orig"
    };
    if (!IsSafeInstallId(manifest.installId) || !IsHexHash(manifest.proxySha256) ||
        !IsHexHash(manifest.gearSha256) || !IsHexHash(manifest.originalCopySha256)) {
        error = L"安装清单的安装编号或载荷哈希格式无效。";
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        if (!LoadRecord(values, sections[i], manifest.files[i], error)) return false;
        if (_wcsicmp(manifest.files[i].path.c_str(), expectedPaths[i]) ||
            _wcsicmp(manifest.files[i].backup.c_str(), expectedBackups[i]) ||
            !IsSafeRelative(manifest.files[i].path) || !IsSafeRelative(manifest.files[i].backup) ||
            (manifest.files[i].existed && !IsHexHash(manifest.files[i].sha256)) ||
            (!manifest.files[i].existed && (!manifest.files[i].sha256.empty() || manifest.files[i].size))) {
            error = L"安装清单包含无效的文件身份、备份路径或原文件哈希。";
            return false;
        }
    }
    return true;
}

bool ValidateUninstall(const std::wstring& rootValue, Manifest& manifest, std::wstring& error,
                       bool requireInstalledProxy) {
    const std::wstring root = CanonicalRoot(rootValue);
    if (!LoadManifest(root, manifest, error)) return false;
    for (const FileRecord& record : manifest.files) {
        if (!record.existed) continue;
        const std::wstring backup = JoinPath(root, record.backup);
        if (!VerifyFile(backup, record.sha256, record.size, error)) {
            error = L"原始备份校验失败，卸载已停止。\r\n" + error;
            return false;
        }
    }
    if (requireInstalledProxy) {
        const std::wstring current = JoinPath(root, L"Koeicda.dll");
        std::wstring hash;
        if (!ValidateProxyAudio(current, error) || !Sha256File(current, hash, error) ||
            _wcsicmp(hash.c_str(), manifest.proxySha256.c_str())) {
            error = L"当前 Koeicda.dll 不是清单记录的本补丁代理，卸载已停止。\r\n" + error;
            return false;
        }
    }
    return true;
}

bool WriteBytes(const std::wstring& path, const std::vector<unsigned char>& bytes, std::wstring& error) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && !EnsureDirectory(path.substr(0, slash), error)) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = Failure(L"无法写入文件", path, GetLastError());
        return false;
    }
    size_t offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const DWORD request = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        if (!WriteFile(file, bytes.data() + offset, request, &written, nullptr) || written != request) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(path.c_str());
        error = Failure(L"写入文件失败", path, code);
        return false;
    }
    return true;
}

bool ReadResourceBytes(HMODULE module, WORD id, std::vector<unsigned char>& bytes, std::wstring& error) {
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!resource) {
        error = L"找不到内嵌资源，编号 " + std::to_wstring(id) + L"。";
        return false;
    }
    HGLOBAL loaded = LoadResource(module, resource);
    const DWORD size = SizeofResource(module, resource);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || !size) {
        error = L"无法读取内嵌资源，编号 " + std::to_wstring(id) + L"。";
        return false;
    }
    const auto first = static_cast<const unsigned char*>(data);
    bytes.assign(first, first + size);
    return true;
}

bool InstallPatch(const std::wstring& rootValue, const PackagePayload& payload,
                  TargetKind requestedKind, std::wstring& report, std::wstring& error) {
    const std::wstring root = CanonicalRoot(rootValue);
    if (root.empty() || !DirectoryExists(root)) {
        error = L"目标游戏目录不存在。";
        return false;
    }
    if (IsGameRunning(root, error)) return false;
    if (!ValidateProxyAudioBytes(payload.proxy, error)) {
        error = L"安装器内嵌代理无效。\r\n" + error;
        return false;
    }
    if (!ValidateGearBytes(payload.gear, error)) {
        error = L"安装器内嵌齿轮模块无效。\r\n" + error;
        return false;
    }
    if (!ValidateConfig(payload.defaultConfig)) {
        error = L"安装器内嵌配置不是 74 行格式。";
        return false;
    }
    if (payload.defaultFeatures.empty()) {
        error = L"安装器内嵌功能配置为空。";
        return false;
    }
    std::wstring proxyHash, gearHash;
    if (!Sha256Bytes(payload.proxy, proxyHash, error) || !Sha256Bytes(payload.gear, gearHash, error)) return false;

    std::wstring detail;
    const TargetKind detected = DetectTarget(root, detail);
    if (detected == TargetKind::Invalid ||
        (requestedKind != TargetKind::Invalid && requestedKind != detected)) {
        error = L"安装目标状态发生变化，操作已停止。\r\n" + detail;
        return false;
    }
    Manifest manifest;
    const bool repair = detected == TargetKind::Installed;
    if (repair) {
        if (!ValidateUninstall(root, manifest, error, false)) return false;
    } else {
        manifest.packageVersion = kPackageVersion;
        manifest.installId = HexId();
        manifest.mode = detected == TargetKind::Legacy ? InstallMode::LegacyAdopted : InstallMode::Clean;
        manifest.root = root;
        manifest.patchDirExisted = DirectoryExists(JoinPath(root, L"patch"));
        manifest.privateDirExisted = DirectoryExists(JoinPath(root, kPrivateRelative));
        if (manifest.privateDirExisted && !DirectoryEmpty(JoinPath(root, kPrivateRelative))) {
            error = L"patch\\xiaoan66 已存在但没有有效安装清单。为避免覆盖未知备份，安装已停止。";
            return false;
        }
        if (detected == TargetKind::Clean) {
            if (!CaptureFile(JoinPath(root, L"Koeicda.dll"), L"Koeicda.dll", L"patch\\xiaoan66\\backup\\Koeicda.dll.orig", manifest.files[0], error) ||
                !CaptureFile(JoinPath(root, L"Koeicda_org.dll"), L"Koeicda_org.dll", L"patch\\xiaoan66\\backup\\Koeicda_org.dll.orig", manifest.files[1], error) ||
                !CaptureFile(JoinPath(root, L"GearNtKe.dll"), L"GearNtKe.dll", L"patch\\xiaoan66\\backup\\GearNtKe.dll.orig", manifest.files[2], error)) return false;
        } else {
            if (!CaptureFile(JoinPath(root, L"Koeicda_org.dll"), L"Koeicda.dll", L"patch\\xiaoan66\\backup\\Koeicda.dll.orig", manifest.files[0], error)) return false;
            manifest.files[1].path = L"Koeicda_org.dll";
            manifest.files[1].backup = L"patch\\xiaoan66\\backup\\Koeicda_org.dll.orig";
            manifest.files[2].path = L"GearNtKe.dll";
            manifest.files[2].backup = L"patch\\xiaoan66\\backup\\GearNtKe.dll.orig";
        }
    }

    const std::wstring stage = JoinPath(root, L".xiaoan66-stage-" + (repair ? HexId() : manifest.installId));
    if (!EnsureDirectory(stage, error)) return false;
    auto cleanupStage = [&]() { RemoveKnownTree(stage); };
    const std::wstring stagedProxy = JoinPath(stage, L"Koeicda.dll");
    const std::wstring stagedGear = JoinPath(stage, L"GearNtKe.dll");
    const std::wstring stagedOrg = JoinPath(stage, L"Koeicda_org.dll");
    if (!WriteBytes(stagedProxy, payload.proxy, error) || !WriteBytes(stagedGear, payload.gear, error)) {
        cleanupStage();
        return false;
    }

    const std::wstring baselineBackup = JoinPath(root, manifest.files[0].backup);
    std::wstring baselineSource;
    if (repair) baselineSource = baselineBackup;
    else baselineSource = detected == TargetKind::Legacy ? JoinPath(root, L"Koeicda_org.dll") : JoinPath(root, L"Koeicda.dll");
    if (!CopyPreserving(baselineSource, stagedOrg, error)) {
        cleanupStage();
        return false;
    }
    std::wstring originalHash;
    if (!Sha256File(stagedOrg, originalHash, error)) {
        cleanupStage();
        return false;
    }

    std::vector<unsigned char> privateConfig;
    std::wstring stagedConfig;
    std::wstring stagedFeatures;
    if (!repair) {
        if (manifest.mode == InstallMode::LegacyAdopted)
            ImportLegacyConfig(root, payload.defaultConfig, privateConfig);
        else
            privateConfig = payload.defaultConfig;
        stagedConfig = JoinPath(stage, L"pl_06.ini");
        stagedFeatures = JoinPath(stage, L"features.ini");
        if (!WriteBytes(stagedConfig, privateConfig, error) ||
            !WriteBytes(stagedFeatures, payload.defaultFeatures, error)) {
            cleanupStage();
            return false;
        }
        const std::wstring patch = JoinPath(root, L"patch");
        const std::wstring privateDir = JoinPath(root, kPrivateRelative);
        const std::wstring backupDir = JoinPath(root, kBackupRelative);
        if (!EnsureDirectory(backupDir, error)) {
            cleanupStage();
            return false;
        }
        SetFileAttributesW(backupDir.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_DIRECTORY);
        for (const FileRecord& record : manifest.files) if (record.existed) {
            std::wstring source;
            if (manifest.mode == InstallMode::LegacyAdopted && record.path == L"Koeicda.dll")
                source = JoinPath(root, L"Koeicda_org.dll");
            else source = JoinPath(root, record.path);
            if (!CopyPreserving(source, JoinPath(root, record.backup), error) ||
                !VerifyFile(JoinPath(root, record.backup), record.sha256, record.size, error)) {
                if (!manifest.privateDirExisted) RemoveKnownTree(privateDir);
                else RemoveKnownTree(backupDir);
                if (!manifest.patchDirExisted && DirectoryEmpty(patch)) RemoveDirectoryW(patch.c_str());
                cleanupStage();
                return false;
            }
        }
    }

    std::vector<Change> changes = {
        {JoinPath(root, L"Koeicda.dll"), stagedProxy, JoinPath(stage, L"rollback-Koeicda.dll")},
        {JoinPath(root, L"Koeicda_org.dll"), stagedOrg, JoinPath(stage, L"rollback-Koeicda_org.dll")},
        {JoinPath(root, L"GearNtKe.dll"), stagedGear, JoinPath(stage, L"rollback-GearNtKe.dll")}
    };
    if (!repair) {
        changes.push_back({JoinPath(root, kConfigRelative), stagedConfig, JoinPath(stage, L"rollback-pl_06.ini")});
        changes.push_back({JoinPath(root, kFeaturesRelative), stagedFeatures, JoinPath(stage, L"rollback-features.ini")});
    }
    if (!BeginChanges(changes, error)) {
        if (!repair) {
            if (!manifest.privateDirExisted) RemoveKnownTree(JoinPath(root, kPrivateRelative));
            else RemoveKnownTree(JoinPath(root, kBackupRelative));
        }
        cleanupStage();
        return false;
    }
    bool verified = VerifyFile(JoinPath(root, L"Koeicda.dll"), proxyHash, payload.proxy.size(), error) &&
                    VerifyFile(JoinPath(root, L"GearNtKe.dll"), gearHash, payload.gear.size(), error);
    if (verified) {
        WIN32_FILE_ATTRIBUTE_DATA orgData = {};
        if (!GetFileAttributesExW(JoinPath(root, L"Koeicda_org.dll").c_str(), GetFileExInfoStandard, &orgData)) verified = false;
        if (verified) verified = VerifyFile(JoinPath(root, L"Koeicda_org.dll"), originalHash,
                                            manifest.files[0].size, error);
    }
    manifest.packageVersion = kPackageVersion;
    manifest.proxySha256 = proxyHash;
    manifest.gearSha256 = gearHash;
    manifest.originalCopySha256 = originalHash;
    if (verified && !SaveManifestFile(ManifestPath(root), manifest, error)) verified = false;
    if (!verified) {
        RollbackChanges(changes);
        if (!repair) {
            if (!manifest.privateDirExisted) RemoveKnownTree(JoinPath(root, kPrivateRelative));
            else RemoveKnownTree(JoinPath(root, kBackupRelative));
        }
        cleanupStage();
        return false;
    }
    CommitChanges(changes);
    cleanupStage();
    SetFileAttributesW(JoinPath(root, kPrivateRelative).c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_DIRECTORY);
    report = repair ? L"补丁已修复/更新，首次安装保存的原始备份和当前私有配置均已保留。" :
             (manifest.mode == InstallMode::LegacyAdopted ?
              L"旧版松散安装已完成有限迁移。卸载可恢复原音频 DLL 并移除加速模块；共享 patch\\pl_06.ini 保持不变，但无法找回第一次旧安装前已丢失的配置内容。" :
              L"补丁安装完成。卸载时将按清单逐字节恢复本次安装前的文件。共享 patch\\pl_06.ini 未被修改。" );
    return true;
}

bool RestorePatch(const std::wstring& rootValue, std::wstring& report, std::wstring& error,
                  bool requireInstalledProxy) {
    const std::wstring root = CanonicalRoot(rootValue);
    Manifest manifest;
    if (!ValidateUninstall(root, manifest, error, requireInstalledProxy)) return false;
    const std::wstring stage = JoinPath(root, L".xiaoan66-restore-" + HexId());
    if (!EnsureDirectory(stage, error)) return false;
    std::vector<Change> changes;
    for (size_t i = 0; i < 3; ++i) {
        const FileRecord& record = manifest.files[i];
        std::wstring payload;
        if (record.existed) {
            payload = JoinPath(stage, L"restore-" + std::to_wstring(i));
            if (!CopyPreserving(JoinPath(root, record.backup), payload, error)) {
                RemoveKnownTree(stage);
                return false;
            }
        }
        changes.push_back({JoinPath(root, record.path), payload,
                           JoinPath(stage, L"rollback-" + std::to_wstring(i))});
    }
    if (!BeginChanges(changes, error)) {
        RemoveKnownTree(stage);
        return false;
    }
    bool success = true;
    for (size_t i = 0; i < 3 && success; ++i) {
        const FileRecord& record = manifest.files[i];
        const std::wstring target = JoinPath(root, record.path);
        if (record.existed) {
            success = VerifyFile(target, record.sha256, record.size, error) && ApplyMetadata(target, record, error);
        } else if (FileExists(target)) {
            error = L"卸载验证失败：应删除的文件仍然存在：" + target;
            success = false;
        }
    }
    if (!success) {
        RollbackChanges(changes);
        RemoveKnownTree(stage);
        return false;
    }
    CommitChanges(changes);
    RemoveKnownTree(stage);

    const std::wstring privateDir = JoinPath(root, kPrivateRelative);
    SetFileAttributesW(privateDir.c_str(), FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_DIRECTORY);
    RemoveKnownTree(privateDir);
    if (manifest.privateDirExisted) EnsureDirectory(privateDir, error);
    const std::wstring patchDir = JoinPath(root, L"patch");
    if (!manifest.patchDirExisted && DirectoryEmpty(patchDir)) RemoveDirectoryW(patchDir.c_str());

    report = manifest.mode == InstallMode::LegacyAdopted ?
        L"加速补丁已卸载，原音频 DLL 已恢复，代理和齿轮模块已移除。共享 patch\\pl_06.ini 保持迁移时状态；无法保证恢复第一次旧安装前已经丢失的配置内容。" :
        L"加速补丁已完整卸载，安装前存在的文件内容、属性和时间均已恢复。";
    return true;
}

} // namespace XiaoAnInstall
