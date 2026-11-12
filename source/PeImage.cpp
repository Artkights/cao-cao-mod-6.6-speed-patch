#include "PeImage.h"
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <limits>

#pragma comment(lib, "bcrypt.lib")

namespace {
bool Range(uint64_t offset, uint64_t length, uint64_t size) noexcept {
    return offset <= size && length <= size - offset;
}
bool Readable(DWORD protection) noexcept {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD value = protection & 0xFF;
    return value == PAGE_READONLY || value == PAGE_READWRITE || value == PAGE_WRITECOPY ||
        value == PAGE_EXECUTE_READ || value == PAGE_EXECUTE_READWRITE || value == PAGE_EXECUTE_WRITECOPY;
}
bool Executable(DWORD protection) noexcept {
    const DWORD value = protection & 0xFF;
    return Readable(protection) && (value == PAGE_EXECUTE_READ ||
        value == PAGE_EXECUTE_READWRITE || value == PAGE_EXECUTE_WRITECOPY);
}
bool CopyChecked(void* destination, const void* source, size_t length) noexcept {
    __try { memcpy(destination, source, length); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadHeaderPrefix(const PeFileView& file, std::vector<BYTE>& headers, std::wstring& error) {
    IMAGE_DOS_HEADER dos = {};
    if (!file.Read(0, &dos, sizeof(dos), error)) return false;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) || dos.e_lfanew > 0x100000) {
        error = L"主程序 DOS/PE 头偏移无效。"; return false;
    }
    IMAGE_NT_HEADERS32 nt = {};
    if (!file.Read(static_cast<uint64_t>(dos.e_lfanew), &nt, sizeof(nt), error)) return false;
    const size_t count = nt.OptionalHeader.SizeOfHeaders;
    if (count < sizeof(dos) || count > 0x100000 || count > file.Size()) {
        error = L"主程序 PE 头长度无效。"; return false;
    }
    headers.resize(count);
    return file.Read(0, headers.data(), headers.size(), error);
}
}

uint32_t PeLogicalSectionSize(const IMAGE_SECTION_HEADER& section) noexcept {
    return section.Misc.VirtualSize ? section.Misc.VirtualSize : section.SizeOfRawData;
}

bool ParsePeImageMetadata(const BYTE* headers, size_t headerBytes, uint64_t fileSize,
    bool validateRawRanges, PeImageMetadata& metadata, std::wstring& error) {
    metadata = PeImageMetadata(); error.clear();
    IMAGE_DOS_HEADER dos = {};
    if (!headers || !Range(0, sizeof(dos), headerBytes)) { error = L"主程序 DOS 头截断。"; return false; }
    memcpy(&dos, headers, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) ||
        !Range(static_cast<uint64_t>(dos.e_lfanew), sizeof(metadata.nt), headerBytes)) {
        error = L"主程序 PE 头截断或偏移无效。"; return false;
    }
    memcpy(&metadata.nt, headers + dos.e_lfanew, sizeof(metadata.nt));
    const auto& nt = metadata.nt;
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        !(nt.FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
        (nt.FileHeader.Characteristics & IMAGE_FILE_DLL) || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32) ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96 ||
        nt.OptionalHeader.SizeOfImage < 4096 || nt.OptionalHeader.SizeOfImage > kMaximumPeAnalysisImage ||
        nt.OptionalHeader.SizeOfHeaders > nt.OptionalHeader.SizeOfImage ||
        nt.OptionalHeader.SizeOfHeaders > headerBytes) {
        error = L"主程序不是结构完整且可分析的 x86 PE32。"; return false;
    }
    const uint64_t table = static_cast<uint64_t>(dos.e_lfanew) + 24 + nt.FileHeader.SizeOfOptionalHeader;
    const size_t tableBytes = nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (!Range(table, tableBytes, nt.OptionalHeader.SizeOfHeaders)) {
        error = L"主程序节表超出 PE 头范围。"; return false;
    }
    metadata.sections.resize(nt.FileHeader.NumberOfSections);
    memcpy(metadata.sections.data(), headers + static_cast<size_t>(table), tableBytes);
    metadata.fileSize = fileSize;
    for (size_t i = 0; i < metadata.sections.size(); ++i) {
        const auto& section = metadata.sections[i];
        const uint64_t length = PeLogicalSectionSize(section);
        if (!Range(section.VirtualAddress, length, nt.OptionalHeader.SizeOfImage) ||
            (length && section.VirtualAddress < nt.OptionalHeader.SizeOfHeaders)) {
            error = L"主程序节的逻辑虚拟范围越界或覆盖 PE 头。"; return false;
        }
        if (validateRawRanges && !Range(section.PointerToRawData, section.SizeOfRawData, fileSize)) {
            error = L"主程序节的磁盘原始范围超出文件末尾。"; return false;
        }
        if (!length) continue;
        for (size_t j = 0; j < i; ++j) {
            const auto& other = metadata.sections[j];
            const uint64_t otherLength = PeLogicalSectionSize(other);
            if (otherLength && section.VirtualAddress < static_cast<uint64_t>(other.VirtualAddress) + otherLength &&
                other.VirtualAddress < static_cast<uint64_t>(section.VirtualAddress) + length) {
                error = L"主程序节的逻辑虚拟范围发生重叠。"; return false;
            }
        }
    }
    return true;
}

PeFileView::~PeFileView() { Close(); }
void PeFileView::Close() noexcept {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE; size_ = 0;
}
bool PeFileView::Open(const std::wstring& path, std::wstring& error) {
    Close(); error.clear();
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER length = {};
    if (file_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(file_, &length) || length.QuadPart < 0) {
        Close(); error = L"无法以只读方式打开主程序或取得 64 位文件长度。"; return false;
    }
    size_ = static_cast<uint64_t>(length.QuadPart);
    return true;
}
bool PeFileView::Read(uint64_t offset, void* destination, size_t bytes, std::wstring& error) const {
    error.clear();
    if (file_ == INVALID_HANDLE_VALUE || (!destination && bytes) || !Range(offset, bytes, size_) ||
        offset > static_cast<uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        error = L"主程序读取范围越过文件边界。"; return false;
    }
    LARGE_INTEGER position = {}; position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file_, position, nullptr, FILE_BEGIN)) { error = L"无法定位主程序读取位置。"; return false; }
    BYTE* target = static_cast<BYTE*>(destination);
    while (bytes) {
        const DWORD part = static_cast<DWORD>((std::min)(bytes, size_t(1024 * 1024)));
        DWORD copied = 0;
        if (!ReadFile(file_, target, part, &copied, nullptr) || copied != part) {
            error = L"主程序读取不完整或文件在读取期间发生变化。"; return false;
        }
        target += part; bytes -= part;
    }
    return true;
}
bool PeFileView::HashSha256(std::wstring& result, std::wstring& error) const {
    result.clear(); error.clear();
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, copied = 0; BYTE digest[32] = {};
    bool ok = file_ != INVALID_HANDLE_VALUE &&
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<BYTE*>(&objectBytes),
        sizeof(objectBytes), &copied, 0) >= 0;
    std::vector<BYTE> object(objectBytes), chunk(1024 * 1024);
    if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) >= 0;
    for (uint64_t offset = 0; ok && offset < size_;) {
        const size_t part = static_cast<size_t>((std::min)(uint64_t(chunk.size()), size_ - offset));
        ok = Read(offset, chunk.data(), part, error) && BCryptHashData(hash, chunk.data(), static_cast<ULONG>(part), 0) >= 0;
        offset += part;
    }
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) { if (error.empty()) error = L"无法计算主程序的流式 SHA-256。"; return false; }
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    for (BYTE value : digest) { result.push_back(digits[value >> 4]); result.push_back(digits[value & 15]); }
    return true;
}
bool Sha256FileStream(const std::wstring& path, std::wstring& hash, std::wstring& error) {
    PeFileView file;
    return file.Open(path, error) && file.HashSha256(hash, error);
}

bool PeAnalysisView::Map(const PeFileView& file, std::wstring& error) {
    metadata_ = PeImageMetadata(); bytes_.clear();
    std::vector<BYTE> headers;
    if (!ReadHeaderPrefix(file, headers, error) ||
        !ParsePeImageMetadata(headers.data(), headers.size(), file.Size(), true, metadata_, error)) return false;
    bytes_.assign(metadata_.nt.OptionalHeader.SizeOfImage, 0);
    memcpy(bytes_.data(), headers.data(), headers.size());
    for (const auto& section : metadata_.sections) {
        const size_t count = (std::min<uint32_t>)(PeLogicalSectionSize(section),
            static_cast<uint32_t>(section.SizeOfRawData));
        if (count && !file.Read(section.PointerToRawData, bytes_.data() + section.VirtualAddress, count, error)) {
            bytes_.clear(); return false;
        }
    }
    return true;
}

bool ValidateRuntimeMemory(const void* address, size_t bytes, bool executable, std::wstring& error) {
    error.clear();
    uintptr_t at = reinterpret_cast<uintptr_t>(address);
    if ((!at && bytes) || bytes > UINTPTR_MAX - at) { error = L"运行时读取地址发生溢出。"; return false; }
    const uintptr_t end = at + bytes;
    while (at < end) {
        MEMORY_BASIC_INFORMATION region = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) || region.State != MEM_COMMIT ||
            !Readable(region.Protect) || (executable && !Executable(region.Protect))) {
            error = L"运行时范围包含未提交、不可读、guard 或权限不符的页面。"; return false;
        }
        const uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - start || start + region.RegionSize <= at) {
            error = L"运行时页面范围发生溢出。"; return false;
        }
        at = (std::min<uintptr_t>)(end, start + static_cast<uintptr_t>(region.RegionSize));
    }
    return true;
}
bool ReadRuntimeMemory(const void* address, void* destination, size_t bytes, std::wstring& error) {
    if (!destination && bytes) { error = L"运行时读取目标缓冲区为空。"; return false; }
    if (!ValidateRuntimeMemory(address, bytes, false, error)) return false;
    if (!CopyChecked(destination, address, bytes)) { error = L"运行时页面在读取期间发生变化。"; return false; }
    return true;
}
bool RuntimeImageView::Capture(const BYTE* base, size_t sizeHint, std::wstring& error) {
    metadata_ = PeImageMetadata(); bytes_.clear(); readable_.clear(); runtimeBase_ = 0;
    MEMORY_BASIC_INFORMATION initial = {};
    if (!base || !VirtualQuery(base, &initial, sizeof(initial)) || initial.AllocationBase != base) {
        error = L"运行时主程序基址不对应独立的映像分配。"; return false;
    }
    IMAGE_DOS_HEADER dos = {}; IMAGE_NT_HEADERS32 nt = {};
    if ((sizeHint && sizeHint < sizeof(dos)) || !ReadRuntimeMemory(base, &dos, sizeof(dos), error) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) || dos.e_lfanew > 0x100000) {
        if (error.empty()) error = L"运行时 DOS 头无效。"; return false;
    }
    const size_t ntOffset = static_cast<size_t>(dos.e_lfanew);
    if ((sizeHint && !Range(ntOffset, sizeof(nt), sizeHint)) ||
        !ReadRuntimeMemory(base + ntOffset, &nt, sizeof(nt), error) ||
        nt.OptionalHeader.SizeOfHeaders > 0x100000 || !nt.OptionalHeader.SizeOfHeaders ||
        (sizeHint && nt.OptionalHeader.SizeOfHeaders > sizeHint)) {
        if (error.empty()) error = L"运行时 PE 头范围无效。"; return false;
    }
    std::vector<BYTE> headers(nt.OptionalHeader.SizeOfHeaders);
    if (!ReadRuntimeMemory(base, headers.data(), headers.size(), error) ||
        !ParsePeImageMetadata(headers.data(), headers.size(), 0, false, metadata_, error) ||
        (sizeHint && metadata_.nt.OptionalHeader.SizeOfImage > sizeHint)) return false;
    const size_t imageSize = metadata_.nt.OptionalHeader.SizeOfImage;
    const uintptr_t origin = reinterpret_cast<uintptr_t>(base);
    if (imageSize > UINTPTR_MAX - origin) { error = L"运行时映像范围发生溢出。"; return false; }
    bytes_.assign(imageSize, 0); runtimeBase_ = origin;
    for (size_t offset = 0; offset < imageSize;) {
        MEMORY_BASIC_INFORMATION region = {};
        const uintptr_t at = origin + offset;
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region))) {
            error = L"无法查询运行时映像页面。"; bytes_.clear(); return false;
        }
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - regionBase || regionBase + region.RegionSize <= at) {
            error = L"运行时映像页面范围无效。"; bytes_.clear(); return false;
        }
        const size_t count = (std::min)(imageSize - offset, static_cast<size_t>(regionBase + region.RegionSize - at));
        // A malformed or stale SizeOfImage must not capture an unrelated
        // allocation that happens to follow this module in address space.
        if (region.AllocationBase == base && region.State == MEM_COMMIT && Readable(region.Protect)) {
            if (!ReadRuntimeMemory(reinterpret_cast<const void*>(at), bytes_.data() + offset, count, error)) {
                bytes_.clear(); return false;
            }
            readable_.push_back({offset, offset + count, Executable(region.Protect)});
        }
        offset += count;
    }
    return true;
}
bool RuntimeImageView::Contains(size_t rva, size_t length, bool executable) const noexcept {
    if (!Range(rva, length, bytes_.size())) return false;
    const size_t end = rva + length;
    for (const auto& region : readable_) {
        if (region.end <= rva) continue;
        if (region.begin > rva || (executable && !region.executable)) return false;
        if (region.end >= end) return true;
        rva = region.end;
    }
    return length == 0;
}
bool RuntimeImageView::IsReadable(size_t rva, size_t length) const noexcept { return Contains(rva, length, false); }
bool RuntimeImageView::IsExecutable(size_t rva, size_t length) const noexcept { return Contains(rva, length, true); }
