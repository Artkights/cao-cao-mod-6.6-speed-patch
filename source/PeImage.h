#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// Disk storage and a loaded PE image have different bounds. Packed containers
// can have very large raw sections whose logical virtual extent is small.
constexpr size_t kMaximumPeAnalysisImage = 64u * 1024u * 1024u;
uint32_t PeLogicalSectionSize(const IMAGE_SECTION_HEADER& section) noexcept;

struct PeImageMetadata {
    IMAGE_NT_HEADERS32 nt = {};
    std::vector<IMAGE_SECTION_HEADER> sections;
    uint64_t fileSize = 0;
};

bool ParsePeImageMetadata(const BYTE* headers, size_t headerBytes, uint64_t fileSize,
    bool validateRawRanges, PeImageMetadata& metadata, std::wstring& error);

class PeFileView {
public:
    PeFileView() = default;
    ~PeFileView();
    PeFileView(const PeFileView&) = delete;
    PeFileView& operator=(const PeFileView&) = delete;
    bool Open(const std::wstring& path, std::wstring& error);
    void Close() noexcept;
    uint64_t Size() const noexcept { return size_; }
    bool Read(uint64_t offset, void* destination, size_t bytes, std::wstring& error) const;
    bool HashSha256(std::wstring& hash, std::wstring& error) const;
private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

bool Sha256FileStream(const std::wstring& path, std::wstring& hash, std::wstring& error);

class PeAnalysisView {
public:
    bool Map(const PeFileView& file, std::wstring& error);
    const BYTE* Data() const noexcept { return bytes_.data(); }
    size_t Size() const noexcept { return bytes_.size(); }
    uintptr_t PreferredBase() const noexcept { return metadata_.nt.OptionalHeader.ImageBase; }
    const PeImageMetadata& Metadata() const noexcept { return metadata_; }
private:
    PeImageMetadata metadata_;
    std::vector<BYTE> bytes_;
};

// Each read is split at VirtualQuery region boundaries. Guard, reserved and
// inaccessible pages are never read, including when they lie inside SizeOfImage.
bool ValidateRuntimeMemory(const void* address, size_t bytes, bool executable,
    std::wstring& error);
bool ReadRuntimeMemory(const void* address, void* destination, size_t bytes,
    std::wstring& error);

class RuntimeImageView {
public:
    bool Capture(const BYTE* base, size_t sizeHint, std::wstring& error);
    const BYTE* Data() const noexcept { return bytes_.data(); }
    size_t Size() const noexcept { return bytes_.size(); }
    uintptr_t PreferredBase() const noexcept { return metadata_.nt.OptionalHeader.ImageBase; }
    uintptr_t RuntimeBase() const noexcept { return runtimeBase_; }
    const PeImageMetadata& Metadata() const noexcept { return metadata_; }
    bool IsReadable(size_t rva, size_t length) const noexcept;
    bool IsExecutable(size_t rva, size_t length) const noexcept;
private:
    struct Region { size_t begin; size_t end; bool executable; };
    bool Contains(size_t rva, size_t length, bool executable) const noexcept;
    PeImageMetadata metadata_;
    std::vector<BYTE> bytes_;
    std::vector<Region> readable_;
    uintptr_t runtimeBase_ = 0;
};
