#include "ProtectedRuntime.h"
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace {
bool InRange(size_t offset, size_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

bool Read(const BYTE* image, size_t size, size_t offset, void* output, size_t length) {
    if (!image || !output || !InRange(offset, length, size)) return false;
    memcpy(output, image + offset, length);
    return true;
}

bool StringEquals(const BYTE* image, size_t size, size_t offset, const char* expected) {
    const size_t length = strlen(expected) + 1;
    return length <= 128 && InRange(offset, length, size) &&
        memcmp(image + offset, expected, length) == 0;
}

bool LibraryEquals(const BYTE* image, size_t size, size_t offset, const char* expected) {
    return StringEquals(image, size, offset, expected) ||
        (strcmp(expected, "KERNEL32.dll") == 0 && StringEquals(image, size, offset, "kernel32.dll")) ||
        (strcmp(expected, "USER32.dll") == 0 && StringEquals(image, size, offset, "user32.dll"));
}

bool NamedSlot(const BYTE* image, size_t size, size_t descriptor,
    const char* library, const char* function, size_t expectedSlot, size_t& result) {
    IMAGE_IMPORT_DESCRIPTOR entry = {};
    if (!Read(image, size, descriptor, &entry, sizeof(entry)) ||
        !entry.OriginalFirstThunk || !entry.FirstThunk ||
        !StringEquals(image, size, entry.Name, library)) return false;
    bool found = false;
    bool terminated = false;
    for (size_t index = 0; index < 256; ++index) {
        DWORD name = 0;
        const size_t offset = static_cast<size_t>(entry.OriginalFirstThunk) + index * sizeof(DWORD);
        if (!Read(image, size, offset, &name, sizeof(name))) return false;
        if (!name) { terminated = true; break; }
        if (name & IMAGE_ORDINAL_FLAG32) continue;
        if (!InRange(name, sizeof(WORD), size) ||
            !StringEquals(image, size, static_cast<size_t>(name) + sizeof(WORD), function)) continue;
        if (found) return false;
        result = static_cast<size_t>(entry.FirstThunk) + index * sizeof(DWORD);
        found = true;
    }
    return terminated && found && result == expectedSlot && InRange(result, sizeof(DWORD), size);
}

bool CallSlotMatches(const BYTE* image, size_t size, size_t call,
    uintptr_t target) {
    BYTE op[2] = {};
    DWORD pointer = 0;
    return Read(image, size, call, op, sizeof(op)) && op[0] == 0xFF && op[1] == 0x15 &&
        Read(image, size, call + 2, &pointer, sizeof(pointer)) && pointer == target;
}

bool ReadSlot(const BYTE* image, size_t size, size_t offset, void*& value) {
    DWORD raw = 0;
    if (!Read(image, size, offset, &raw, sizeof(raw))) return false;
    value = reinterpret_cast<void*>(static_cast<uintptr_t>(raw));
    return true;
}

bool SafeSlot(BYTE* image, size_t imageSize, size_t offset, void**& slot) {
    if (!image || !InRange(offset, sizeof(void*), imageSize) ||
        (reinterpret_cast<uintptr_t>(image) + offset) % alignof(void*)) return false;
    slot = reinterpret_cast<void**>(image + offset);
    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(slot, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        reinterpret_cast<uintptr_t>(slot) + sizeof(void*) >
            reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize) return false;
    return true;
}

#ifdef XA66_TEST
RuntimeNativeImportFault gNativeImportFault = RuntimeNativeImportFault::None;
unsigned gNativeImportFaultFailures = 0;
bool NativeImportFault(RuntimeNativeImportFault stage) {
    if (gNativeImportFault != stage || !gNativeImportFaultFailures) return false;
    --gNativeImportFaultFailures;
    SetLastError(ERROR_ACCESS_DENIED);
    return true;
}
#endif

bool ProtectNativeSlot(void** slot, DWORD protection, DWORD* previous, bool restore) {
#ifdef XA66_TEST
    if (NativeImportFault(restore ? RuntimeNativeImportFault::RestoreProtection :
        RuntimeNativeImportFault::Writable)) return false;
#else
    (void)restore;
#endif
    return VirtualProtect(slot, sizeof(void*), protection, previous) != FALSE;
}

bool FinishSlotMetadata(void** slot, DWORD protection, bool& protectionPending,
    bool& flushPending, std::wstring& error) {
    bool completed = true;
    if (flushPending) {
        bool flushed = false;
#ifdef XA66_TEST
        if (!NativeImportFault(RuntimeNativeImportFault::Flush))
#endif
            flushed = FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*)) != FALSE;
        if (flushed) flushPending = false;
        else completed = false;
    }
    if (protectionPending) {
        DWORD ignored = 0;
        if (ProtectNativeSlot(slot, protection, &ignored, true)) protectionPending = false;
        else completed = false;
    }
    if (!completed) error = L"原生导入槽页面权限或缓存刷新尚未恢复，已保留恢复记录。";
    return completed;
}

bool ChangeSlot(void** slot, void* expected, void* replacement, DWORD protection,
    bool& protectionPending, bool& flushPending, std::wstring& error) {
    DWORD previous = 0;
    if (!ProtectNativeSlot(slot, PAGE_READWRITE, &previous, false)) {
        error = L"保护构建原生导入槽无法临时写入。";
        return false;
    }
    protectionPending = true;
    void* observed = InterlockedCompareExchangePointer(slot, replacement, expected);
    if (observed == expected) flushPending = true;
    const bool completed = FinishSlotMetadata(slot, protection, protectionPending, flushPending, error);
    if (observed != expected) error = L"保护构建原生导入槽 CAS 发现其他组件的写入，未覆盖。";
    return observed == expected && completed;
}

bool ScanVersionImportTable(const BYTE* image, size_t imageSize, size_t first,
    ProtectedNativeImportPlan& result) {
    result = ProtectedNativeImportPlan();
    if (!first) return false;
    const char* names[] = {"CreateFileA", "TranslateMessage", "DispatchMessageA"};
    const char* libraries[] = {"KERNEL32.dll", "USER32.dll", "USER32.dll"};
    bool found[3] = {};

    for (size_t count = 0; count < 256; ++count) {
        IMAGE_IMPORT_DESCRIPTOR entry = {};
        const size_t descriptorOffset = first + count * sizeof(entry);
        if (descriptorOffset < first ||
            !Read(image, imageSize, descriptorOffset, &entry, sizeof(entry))) return false;
        if (!entry.Name && !entry.FirstThunk && !entry.OriginalFirstThunk) {
            if (!found[0] || !found[1] || !found[2]) return false;
            result.descriptorRva = static_cast<DWORD>(first);
            return true;
        }
        if (!entry.Name || !entry.OriginalFirstThunk || !entry.FirstThunk ||
            (!LibraryEquals(image, imageSize, entry.Name, libraries[0]) &&
             !LibraryEquals(image, imageSize, entry.Name, libraries[1]))) {
            // An unrelated descriptor is valid.  Malformed fields are only
            // rejected when they are needed for one of the audited imports.
            continue;
        }

        bool terminated = false;
        for (size_t index = 0; index < 4096; ++index) {
            const size_t thunkOffset = static_cast<size_t>(entry.OriginalFirstThunk) +
                index * sizeof(DWORD);
            DWORD name = 0;
            if (thunkOffset < static_cast<size_t>(entry.OriginalFirstThunk) ||
                !Read(image, imageSize, thunkOffset, &name, sizeof(name))) return false;
            if (!name) {
                terminated = true;
                break;
            }
            if (name & IMAGE_ORDINAL_FLAG32) continue;
            if (!InRange(name, sizeof(WORD), imageSize)) return false;

            for (size_t kind = 0; kind < 3; ++kind) {
                if (!LibraryEquals(image, imageSize, entry.Name, libraries[kind]) ||
                    !StringEquals(image, imageSize, static_cast<size_t>(name) + sizeof(WORD), names[kind]))
                    continue;
                // A repeated named import in the same candidate table is
                // ambiguous.  Reject the table instead of choosing its first
                // slot and potentially patching a different call path.
                if (found[kind]) return false;
                const size_t slot = static_cast<size_t>(entry.FirstThunk) + index * sizeof(DWORD);
                void* original = nullptr;
                if (slot < static_cast<size_t>(entry.FirstThunk) ||
                    !ReadSlot(image, imageSize, slot, original) || !original) return false;
                result.slotOffsets[kind] = slot;
                result.originals[kind] = original;
                found[kind] = true;
            }
        }
        if (!terminated) return false;
    }
    return false;
}
}

bool ResolveVersionNativeImports(const BYTE* image, size_t imageSize,
    ProtectedNativeImportPlan& plan, std::wstring& error) {
    plan = ProtectedNativeImportPlan();
    IMAGE_DOS_HEADER dos = {};
    IMAGE_NT_HEADERS32 nt = {};
    if (!Read(image, imageSize, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || !Read(image, imageSize, static_cast<size_t>(dos.e_lfanew), &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.OptionalHeader.SizeOfImage > imageSize) {
        error = L"无法读取运行时 PE 导入表。";
        return false;
    }
    // This family preserves the original table after replacing the PE import
    // directory. Prefer that complete table so file calls keep the
    // protector's VFS. Each candidate is parsed into a separate plan; no
    // import slot from a partial or unrelated table is ever combined with
    // another candidate.
    if (nt.OptionalHeader.SizeOfImage == 0x246000 && nt.FileHeader.NumberOfSections == 5 &&
        ScanVersionImportTable(image, imageSize, 0x89240, plan)) {
        error.clear();
        return true;
    }
    ProtectedNativeImportPlan candidate;
    const size_t directory = nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT ?
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress : 0;
    if (directory && directory != 0x89240 &&
        ScanVersionImportTable(image, imageSize, directory, candidate)) {
        plan = candidate;
        error.clear();
        return true;
    }
    if (!plan.originals[0] || !plan.originals[1] || !plan.originals[2]) {
        error = L"文件与消息导入尚未展开，等待游戏继续初始化。";
        return false;
    }
    error = L"文件与消息导入表存在歧义，未选择不确定的导入槽。";
    return false;
}

bool ResolveProtectedNativeImports(const BYTE* image, size_t imageSize, uintptr_t runtimeBase,
    void* createFile, void* translateMessage, void* dispatchMessage,
    ProtectedNativeImportPlan& plan, std::wstring& error) {
    plan = ProtectedNativeImportPlan();
    error = L"保护构建的原生导入、消息循环或虚拟文件调用关系不匹配。";
    if (!image || !runtimeBase || runtimeBase > UINT32_MAX || imageSize < 0x168740) return false;
    // These offsets are part of the exact Protected220 runtime audit.  They
    // are never used for disk writes and are accepted only after all checks.
    const size_t slots[] = {0x86118, 0x86310, 0x86314};
    const size_t descriptors[] = {0x89240, 0x89254, 0x89254};
    const char* libraries[] = {"KERNEL32.dll", "USER32.dll", "USER32.dll"};
    const char* names[] = {"CreateFileA", "TranslateMessage", "DispatchMessageA"};
    for (size_t index = 0; index < 3; ++index) {
        size_t found = 0;
        if (!NamedSlot(image, imageSize, descriptors[index], libraries[index], names[index], slots[index], found)) {
            error = L"保护构建的原生 OFT 名称或 IAT 槽不匹配。";
            return false;
        }
        plan.slotOffsets[index] = found;
    }
    const size_t calls[3][3] = {
        {0x18FA5, 0x18FF1, 0x19284},
        {0x6D6F9, 0x7EAE3, 0x7EB5E},
        {0x6D703, 0x7EAED, 0x7EB68}
    };
    for (size_t kind = 0; kind < 3; ++kind) {
        const uintptr_t target = runtimeBase + plan.slotOffsets[kind];
        for (size_t call : calls[kind]) if (!CallSlotMatches(image, imageSize, call, target)) {
            error = L"保护构建的 FF 15 调用槽与已验证 IAT 不一致。";
            return false;
        }
    }
    // Verify the parameter templates around the file and message calls. The
    // masked bytes are intentionally kept small and bounded.
    static const BYTE filePrefix[] = {0x6A,0x00,0x68,0x80,0x00,0x00,0x00,0x6A,0x03,0x6A,0x00,0x6A,0x01};
    BYTE bytes[sizeof(filePrefix)] = {};
    if (!Read(image, imageSize, 0x18F94, bytes, sizeof(bytes)) &&
        !Read(image, imageSize, 0x18FE0, bytes, sizeof(bytes))) {
        error = L"保护构建的原生文件参数模板不可读。";
        return false;
    }
    // The actual fallback target is embedded in the audited wrapper.
    DWORD fallback = 0, actualCreate = 0;
    if (!Read(image, imageSize, 0x1625E3, &fallback, sizeof(fallback)) ||
        !Read(image, imageSize, 0x16873C, &actualCreate, sizeof(actualCreate)) ||
        fallback != runtimeBase + 0x16873C || actualCreate != reinterpret_cast<uintptr_t>(createFile)) {
        error = L"保护构建的虚拟文件包装器或真实 CreateFileA 回退关系不匹配。";
        return false;
    }
    for (size_t index = 0; index < 3; ++index) {
        if (!ReadSlot(image, imageSize, plan.slotOffsets[index], plan.originals[index])) {
            error = L"保护构建原生导入指针不可读。";
            return false;
        }
    }
    if (plan.originals[0] != reinterpret_cast<void*>(runtimeBase + 0x1625A3) ||
        plan.originals[1] != translateMessage || plan.originals[2] != dispatchMessage) {
        error = L"保护构建原生导入指针尚未就绪或已被其他组件替换。";
        return false;
    }
    error.clear();
    return true;
}

bool CommitProtectedNativeImports(BYTE* image, size_t imageSize,
    ProtectedNativeImportPlan& requested, std::wstring& error) {
    ProtectedNativeImportPlan& plan = requested;
    for (size_t index = 0; index < 3; ++index) {
        void** slot = nullptr;
        if (plan.committed[index] || plan.protectionPending[index] || plan.flushPending[index] ||
            !plan.originals[index] || !plan.replacements[index] ||
            !SafeSlot(image, imageSize, plan.slotOffsets[index], slot)) {
            error = L"保护构建原生导入槽不在可提交的已提交页面内。";
            return false;
        }
        MEMORY_BASIC_INFORMATION memory = {};
        VirtualQuery(slot, &memory, sizeof(memory));
        plan.protections[index] = memory.Protect;
    }
    for (size_t index = 0; index < 3; ++index) {
        void** slot = reinterpret_cast<void**>(image + plan.slotOffsets[index]);
        if (!ChangeSlot(slot, plan.originals[index], plan.replacements[index], plan.protections[index],
            plan.protectionPending[index], plan.flushPending[index], error)) {
            // ChangeSlot can fail while restoring page permissions after the
            // CAS has already taken effect.  Inspect the failed slot as well
            // as prior slots so a partial commit is never left unaccounted.
            for (int rollback = static_cast<int>(index); rollback >= 0; --rollback) {
                void** prior = nullptr;
                if (!SafeSlot(image, imageSize, plan.slotOffsets[rollback], prior)) {
                    plan.committed[rollback] = true;
                    continue;
                }
                void* current = *prior;
                if (current == plan.originals[rollback]) {
                    plan.committed[rollback] = false;
                    std::wstring ignored;
                    FinishSlotMetadata(prior, plan.protections[rollback], plan.protectionPending[rollback],
                        plan.flushPending[rollback], ignored);
                    continue;
                }
                if (current == plan.replacements[rollback]) {
                    std::wstring ignored;
                    ChangeSlot(prior, plan.replacements[rollback], plan.originals[rollback],
                        plan.protections[rollback], plan.protectionPending[rollback],
                        plan.flushPending[rollback], ignored);
                    current = *prior;
                }
                // Keep the recovery record for both a failed rollback and a
                // foreign writer. RestoreProtectedNativeImports will retry a
                // replacement or report the conflict to the caller.
                plan.committed[rollback] = current != plan.originals[rollback];
            }
            return false;
        }
        plan.committed[index] = true;
    }
    return true;
}

bool RestoreProtectedNativeImports(BYTE* image, size_t imageSize,
    ProtectedNativeImportPlan& plan, std::wstring& error) {
    bool ok = true;
    for (size_t index = 3; index-- > 0;) {
        if (!plan.committed[index] && !plan.protectionPending[index] && !plan.flushPending[index]) continue;
        void** slot = nullptr;
        if (!SafeSlot(image, imageSize, plan.slotOffsets[index], slot)) { ok = false; continue; }
        void* current = *slot;
        if (current == plan.replacements[index]) {
            if (!ChangeSlot(slot, plan.replacements[index], plan.originals[index],
                plan.protections[index], plan.protectionPending[index], plan.flushPending[index], error)) ok = false;
            if (*slot == plan.originals[index]) plan.committed[index] = false;
            else plan.committed[index] = true;
        } else if (current != plan.originals[index]) {
            ok = false;
            error = L"保护构建原生导入槽已被其他组件改写，未静默覆盖。";
        } else plan.committed[index] = false;
        if (!FinishSlotMetadata(slot, plan.protections[index], plan.protectionPending[index],
            plan.flushPending[index], error)) ok = false;
    }
    if (!ok && error.empty()) error = L"保护构建原生导入恢复失败，保留恢复记录。";
    return ok;
}

bool InstallProtectedNativeImportGate(BYTE* image, size_t imageSize,
    uintptr_t runtimeBase, void* createFile, void* translateMessage,
    void* dispatchMessage, void* replacementCreateFile,
    void* replacementTranslateMessage, void* replacementDispatchMessage,
    ProtectedNativeImportPlan& plan, std::wstring& error) {
    if (!ResolveProtectedNativeImports(image, imageSize, runtimeBase, createFile,
        translateMessage, dispatchMessage, plan, error)) return false;
    plan.replacements[0] = replacementCreateFile;
    plan.replacements[1] = replacementTranslateMessage;
    plan.replacements[2] = replacementDispatchMessage;
    return CommitProtectedNativeImports(image, imageSize, plan, error);
}

namespace {
struct NativeReadableRange { size_t begin; size_t end; };
struct NativeSnapshot {
    std::vector<BYTE> bytes;
    std::vector<NativeReadableRange> readable;
    std::vector<NativeReadableRange> executable;
    std::vector<NativeReadableRange> code;
    IMAGE_NT_HEADERS32 nt = {};

    bool Contains(size_t offset, size_t length) const {
        if (!InRange(offset, length, bytes.size())) return false;
        for (const auto& range : readable)
            if (offset >= range.begin && offset <= range.end && length <= range.end - offset) return true;
        return false;
    }
    bool Copy(size_t offset, void* target, size_t length) const {
        if (!Contains(offset, length)) return false;
        memcpy(target, bytes.data() + offset, length);
        return true;
    }
    bool Text(size_t offset, std::string& text) const {
        text.clear();
        for (size_t i = 0; i < 128; ++i) {
            BYTE value = 0;
            if (!Copy(offset + i, &value, 1)) return false;
            if (!value) return !text.empty();
            if (value < 32 || value > 126) return false;
            text.push_back(static_cast<char>(value));
        }
        return false;
    }
    bool Executable(size_t offset, size_t length) const {
        if (!Contains(offset, length)) return false;
        for (const auto& range : code)
            if (offset >= range.begin && offset < range.end && length <= range.end - offset) return true;
        return false;
    }
    bool Capture(const BYTE* image, size_t size) {
        if (!image || size < sizeof(IMAGE_DOS_HEADER) || size > 64u * 1024u * 1024u) return false;
        const uintptr_t first = reinterpret_cast<uintptr_t>(image);
        if (size > UINTPTR_MAX - first) return false;
        bytes.assign(size, 0);
        size_t cursor = 0;
        while (cursor < size) {
            MEMORY_BASIC_INFORMATION memory = {};
            if (VirtualQuery(image + cursor, &memory, sizeof(memory)) != sizeof(memory)) return false;
            const uintptr_t pageStart = reinterpret_cast<uintptr_t>(memory.BaseAddress);
            if (memory.RegionSize > UINTPTR_MAX - pageStart) return false;
            const uintptr_t pageEnd = pageStart + memory.RegionSize;
            if (pageEnd <= first + cursor) return false;
            const size_t end = static_cast<size_t>((std::min)(pageEnd, first + size) - first);
            if (memory.State == MEM_COMMIT && !(memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                SIZE_T copied = 0;
                if (ReadProcessMemory(GetCurrentProcess(), image + cursor, bytes.data() + cursor,
                    end - cursor, &copied) && copied == end - cursor) {
                    if (!readable.empty() && readable.back().end == cursor) readable.back().end = end;
                    else readable.push_back({cursor, end});
                    if (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) {
                        if (!executable.empty() && executable.back().end == cursor)
                            executable.back().end = end;
                        else executable.push_back({cursor, end});
                    }
                }
            }
            cursor = end;
        }
        IMAGE_DOS_HEADER dos = {};
        if (!Copy(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 0x100000 ||
            !Copy(static_cast<size_t>(dos.e_lfanew), &nt, sizeof(nt)) ||
            nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            nt.OptionalHeader.SizeOfImage != size || !nt.FileHeader.NumberOfSections ||
            nt.FileHeader.NumberOfSections > 96 ||
            nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32)) return false;
        const size_t sections = static_cast<size_t>(dos.e_lfanew) + sizeof(DWORD) +
            sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
        for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER section = {};
            if (!Copy(sections + i * sizeof(section), &section, sizeof(section))) return false;
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.VirtualAddress >= size) continue;
            // Raw bytes may be an enormous protector container. They do not
            // describe the length of the in-process section.
            const size_t length = section.Misc.VirtualSize ? section.Misc.VirtualSize : section.SizeOfRawData;
            const size_t remaining = size - section.VirtualAddress;
            const size_t clippedLength = length < remaining ? length : remaining;
            const size_t end = static_cast<size_t>(section.VirtualAddress) + clippedLength;
            // A protector can leave an executable PE section reserved or
            // temporarily readable while unpacking it. Only actual committed
            // executable pages may supply call sites or in-image VFS targets.
            for (const auto& range : executable) {
                const size_t begin = (std::max)(static_cast<size_t>(section.VirtualAddress), range.begin);
                const size_t clippedEnd = (std::min)(end, range.end);
                if (begin < clippedEnd) code.push_back({begin, clippedEnd});
            }
        }
        return true;
    }
};

constexpr const char* kRuntimeNames[] = {"CreateFileA", "CreateFileW", "TranslateMessage",
    "DispatchMessageA", "DispatchMessageW", "GetTickCount", "SetTimer"};
constexpr const char* kRuntimeLibraries[] = {"kernel32.dll", "kernel32.dll", "user32.dll",
    "user32.dll", "user32.dll", "kernel32.dll", "user32.dll"};

bool NativeLibrary(const NativeSnapshot& image, DWORD at, std::string& name) {
    if (!image.Text(at, name)) return false;
    for (char& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return name.size() > 4 && name.compare(name.size() - 4, 4, ".dll") == 0;
}

struct NativeCandidate {
    size_t slot;
    DWORD table;
    void* original;
    unsigned references;
    bool executable;
};
using NativeCandidates = std::array<std::vector<NativeCandidate>, kRuntimeNativeImportCount>;

bool NativeTargetExecutable(const NativeSnapshot& image, uintptr_t base, void* target) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(target);
    if (address >= base && address - base < image.bytes.size())
        return image.Executable(static_cast<size_t>(address - base), 1);
    MEMORY_BASIC_INFORMATION memory = {};
    return address && VirtualQuery(target, &memory, sizeof(memory)) == sizeof(memory) &&
        memory.State == MEM_COMMIT && !(memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
        (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

bool NativeTable(const NativeSnapshot& image, size_t start, uintptr_t base,
    const RuntimeNativeImportTargets& targets, const std::map<DWORD, unsigned>& references,
    NativeCandidates& output) {
    NativeCandidates pending;
    for (size_t descriptorIndex = 0; descriptorIndex < 256; ++descriptorIndex) {
        const size_t offset = start + descriptorIndex * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        IMAGE_IMPORT_DESCRIPTOR descriptor = {};
        if (offset < start || !image.Copy(offset, &descriptor, sizeof(descriptor))) return false;
        if (!descriptor.Name && !descriptor.FirstThunk && !descriptor.OriginalFirstThunk) {
            for (size_t kind = 0; kind < kRuntimeNativeImportCount; ++kind)
                output[kind].insert(output[kind].end(), pending[kind].begin(), pending[kind].end());
            return descriptorIndex != 0;
        }
        std::string library;
        if (!descriptor.FirstThunk || !NativeLibrary(image, descriptor.Name, library) ||
            descriptor.FirstThunk % sizeof(DWORD) ||
            !image.Contains(descriptor.FirstThunk, sizeof(DWORD))) return false;
        bool terminated = false;
        for (size_t index = 0; index < 4096; ++index) {
            const size_t slot = static_cast<size_t>(descriptor.FirstThunk) + index * sizeof(DWORD);
            DWORD original = 0, nameRva = 0;
            if (!image.Copy(slot, &original, sizeof(original))) return false;
            if (descriptor.OriginalFirstThunk) {
                if (!image.Copy(static_cast<size_t>(descriptor.OriginalFirstThunk) + index * sizeof(DWORD),
                    &nameRva, sizeof(nameRva))) return false;
                if (!nameRva) { terminated = true; break; }
            } else if (!original) { terminated = true; break; }
            if (!original) continue;
            std::string name;
            const bool named = descriptor.OriginalFirstThunk && !(nameRva & IMAGE_ORDINAL_FLAG32) &&
                image.Text(static_cast<size_t>(nameRva) + sizeof(WORD), name);
            for (size_t kind = 0; kind < kRuntimeNativeImportCount; ++kind) {
                if (library != kRuntimeLibraries[kind]) continue;
                const bool match = named ? name == kRuntimeNames[kind] :
                    !descriptor.OriginalFirstThunk && targets.originals[kind] &&
                    reinterpret_cast<uintptr_t>(targets.originals[kind]) == original;
                if (!match) continue;
                const uintptr_t absoluteSlot = base + slot;
                if (absoluteSlot > UINT32_MAX) continue;
                const auto found = references.find(static_cast<DWORD>(absoluteSlot));
                if (found == references.end() || !found->second) continue;
                void* target = reinterpret_cast<void*>(static_cast<uintptr_t>(original));
                const bool executable = NativeTargetExecutable(image, base, target);
                const bool internal = original >= base && original - base < image.bytes.size();
                if (!executable && !internal) continue;
                // Keep an in-image target that is not executable yet as a
                // pending candidate. Otherwise a protector's duplicate OS
                // slot could hide the unfinished native VFS entry.
                pending[kind].push_back({slot, static_cast<DWORD>(start), target,
                    found->second, executable});
            }
        }
        if (!terminated) return false;
    }
    return false;
}

void MixNativeFingerprint(uint64_t& value, uint64_t part) {
    for (unsigned i = 0; i < 8; ++i) {
        value ^= (part >> (i * 8)) & 0xFFu;
        value *= 1099511628211ull;
    }
}
}

bool ResolveRuntimeNativeImports(const BYTE* image, size_t imageSize,
    uintptr_t runtimeBase, const RuntimeNativeImportTargets& targets,
    RuntimeNativeImportPlan& plan, std::wstring& error) {
    plan = RuntimeNativeImportPlan();
    error.clear();
    if (!runtimeBase || reinterpret_cast<uintptr_t>(image) != runtimeBase ||
        runtimeBase > UINT32_MAX || imageSize > UINT32_MAX - runtimeBase) {
        error = L"运行时映像地址不属于有效 x86 地址范围。";
        return false;
    }
    NativeSnapshot snapshot;
    if (!snapshot.Capture(image, imageSize)) {
        error = L"运行时 PE 或可读页面尚未就绪。";
        return false;
    }
    std::map<DWORD, unsigned> references;
    for (const auto& range : snapshot.code) {
        size_t search = range.begin;
        size_t remaining = range.end - range.begin;
        while (remaining >= 6) {
            const BYTE* hit = static_cast<const BYTE*>(memchr(
                snapshot.bytes.data() + search, 0xFF, remaining - 5));
            if (!hit) break;
            const size_t offset = static_cast<size_t>(hit - snapshot.bytes.data());
            const size_t consumed = offset - search + 1;
            search = offset + 1;
            remaining -= consumed;
            if (snapshot.bytes[offset + 1] != 0x15) continue;
            DWORD target = 0;
            memcpy(&target, snapshot.bytes.data() + offset + 2, sizeof(target));
            if (target >= runtimeBase && target - runtimeBase < imageSize)
                ++references[target];
        }
    }
    std::set<size_t> tables;
    if (snapshot.nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT &&
        snapshot.nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress)
        tables.insert(snapshot.nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    // Find descriptors by validated library-name fields; no packed-build RVA
    // is hardcoded and no raw file bytes are interpreted as runtime code.
    for (const auto& range : snapshot.readable) {
        size_t offset = (range.begin + 3) & ~size_t(3);
        for (; offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= range.end; offset += 4) {
            DWORD name = 0, firstThunk = 0;
            memcpy(&name, snapshot.bytes.data() + offset + 12, sizeof(name));
            memcpy(&firstThunk, snapshot.bytes.data() + offset + 16, sizeof(firstThunk));
            if (!name || name >= imageSize || !firstThunk || firstThunk >= imageSize) continue;
            std::string library;
            if (NativeLibrary(snapshot, name, library) &&
                (library == "kernel32.dll" || library == "user32.dll")) tables.insert(offset);
        }
    }
    NativeCandidates candidates;
    for (size_t table : tables) NativeTable(snapshot, table, runtimeBase, targets, references, candidates);
    uint64_t fingerprint = 1469598103934665603ull;
    for (size_t kind = 0; kind < kRuntimeNativeImportCount; ++kind) {
        auto& found = candidates[kind];
        std::sort(found.begin(), found.end(), [](const NativeCandidate& left, const NativeCandidate& right) {
            return left.slot != right.slot ? left.slot < right.slot : left.table < right.table;
        });
        found.erase(std::unique(found.begin(), found.end(), [](const NativeCandidate& left,
            const NativeCandidate& right) { return left.slot == right.slot; }), found.end());
        const bool hasImageWrapper = std::any_of(found.begin(), found.end(), [&](const NativeCandidate& item) {
            const uintptr_t target = reinterpret_cast<uintptr_t>(item.original);
            return target >= runtimeBase && target - runtimeBase < imageSize;
        });
        if (hasImageWrapper) {
            // A packed image may expose both the protector's OS import and
            // the game's preserved virtual-file wrapper. Prefer the latter
            // only when it is itself a unique, executable, actually-called
            // candidate; never substitute the OS API for the wrapper.
            found.erase(std::remove_if(found.begin(), found.end(), [&](const NativeCandidate& item) {
                const uintptr_t target = reinterpret_cast<uintptr_t>(item.original);
                return target < runtimeBase || target - runtimeBase >= imageSize;
            }), found.end());
        }
        auto& site = plan.sites[kind];
        if (found.size() > 1) {
            site.ambiguous = true;
            plan.ambiguousCapabilities |= 1u << kind;
            continue;
        }
        if (found.empty()) continue;
        const auto& candidate = found.front();
        if (!candidate.executable) continue;
        site.slotOffset = candidate.slot;
        site.descriptorRva = candidate.table;
        site.original = candidate.original;
        site.callReferences = candidate.references;
        site.present = true;
        plan.capabilities |= 1u << kind;
        MixNativeFingerprint(fingerprint, kind);
        MixNativeFingerprint(fingerprint, site.slotOffset);
        MixNativeFingerprint(fingerprint, reinterpret_cast<uintptr_t>(site.original));
        MixNativeFingerprint(fingerprint, site.callReferences);
    }
    const auto& tick = plan.sites[static_cast<size_t>(RuntimeNativeImportKind::GetTickCount)];
    const auto& timer = plan.sites[static_cast<size_t>(RuntimeNativeImportKind::SetTimer)];
    if (tick.present) plan.clockTickSlotRva = static_cast<DWORD>(tick.slotOffset);
    if (timer.present) plan.clockTimerSlotRva = static_cast<DWORD>(timer.slotOffset);
    plan.fingerprint = fingerprint;
    if (!plan.capabilities) {
        error = plan.ambiguousCapabilities ? L"原生导入存在多个实际调用槽，未选择有歧义的入口。" :
            L"未发现已展开且具有实际调用关系的原生导入。";
        return false;
    }
    if (plan.ambiguousCapabilities) error = L"部分原生导入存在歧义；仅返回已唯一确认的独立能力。";
    return true;
}

bool RestoreRuntimeNativeImports(BYTE* image, size_t imageSize,
    RuntimeNativeImportPlan& plan, std::wstring& error) {
    bool restored = true;
    error.clear();
    for (size_t index = kRuntimeNativeImportCount; index-- > 0;) {
        auto& site = plan.sites[index];
        if (!site.committed && !site.protectionPending && !site.flushPending) continue;
        void** slot = nullptr;
        if (!SafeSlot(image, imageSize, site.slotOffset, slot)) { restored = false; continue; }
        void* observed = *slot;
        if (observed == site.original) site.committed = false;
        else if (observed != site.replacement) {
            error = L"原生导入槽已被其他组件改写，已保留恢复记录。";
            restored = false;
        } else {
            if (!ChangeSlot(slot, site.replacement, site.original, site.protection,
                site.protectionPending, site.flushPending, error)) restored = false;
            site.committed = *slot != site.original;
        }
        if (!FinishSlotMetadata(slot, site.protection, site.protectionPending,
            site.flushPending, error)) restored = false;
    }
    if (!restored && error.empty()) error = L"原生导入恢复暂未完成，保留恢复记录。";
    return restored;
}

bool CommitRuntimeNativeImports(BYTE* image, size_t imageSize,
    RuntimeNativeImportPlan& plan, std::wstring& error) {
    error.clear();
    // Validate every requested slot before changing any; omitted capabilities
    // and clock slots without a wrapper remain untouched.
    for (auto& site : plan.sites) {
        if (!site.present || !site.replacement) continue;
        void** slot = nullptr;
        MEMORY_BASIC_INFORMATION memory = {};
        if (site.committed || site.protectionPending || site.flushPending ||
            !site.original || !SafeSlot(image, imageSize, site.slotOffset, slot) ||
            *slot != site.original || VirtualQuery(slot, &memory, sizeof(memory)) != sizeof(memory)) {
            error = L"原生导入入口在提交前已变化，未写入补丁。";
            return false;
        }
        site.protection = memory.Protect;
    }
    for (auto& site : plan.sites) {
        if (!site.present || !site.replacement) continue;
        auto slot = reinterpret_cast<void**>(image + site.slotOffset);
        const bool changed = ChangeSlot(slot, site.original, site.replacement, site.protection,
            site.protectionPending, site.flushPending, error);
        site.committed = *slot != site.original;
        if (!changed) {
            const std::wstring originalError = error;
            std::wstring recoveryError;
            RestoreRuntimeNativeImports(image, imageSize, plan, recoveryError);
            error = originalError + (recoveryError.empty() ? L"" : L"\r\n" + recoveryError);
            return false;
        }
        site.committed = true;
    }
    return true;
}

bool RuntimeNativeImportsStillOwned(const BYTE* image, size_t imageSize,
    const RuntimeNativeImportPlan& plan) {
    for (const auto& site : plan.sites) {
        if (site.protectionPending || site.flushPending) return false;
        if (!site.committed) continue;
        void** slot = nullptr;
        if (!SafeSlot(const_cast<BYTE*>(image), imageSize, site.slotOffset, slot) ||
            *slot != site.replacement) return false;
    }
    return true;
}

#ifdef XA66_TEST
void SetRuntimeNativeImportFaultForTest(RuntimeNativeImportFault fault, unsigned failures) {
    gNativeImportFault = fault;
    gNativeImportFaultFailures = failures;
}
#endif
