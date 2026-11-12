#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <array>

// Each capability is resolved independently. A protector's current import
// directory and a preserved native table may expose different subsets.
enum class RuntimeNativeImportKind : unsigned {
    CreateFileA, CreateFileW, TranslateMessage, DispatchMessageA,
    DispatchMessageW, GetTickCount, SetTimer, Count
};
constexpr size_t kRuntimeNativeImportCount =
    static_cast<size_t>(RuntimeNativeImportKind::Count);
constexpr unsigned RuntimeNativeImportBit(RuntimeNativeImportKind kind) {
    return 1u << static_cast<unsigned>(kind);
}
struct RuntimeNativeImportTargets {
    std::array<void*, kRuntimeNativeImportCount> originals = {};
};
struct RuntimeNativeImportSite {
    size_t slotOffset = 0;
    DWORD descriptorRva = 0;
    void* original = nullptr;
    void* replacement = nullptr;
    DWORD protection = 0;
    unsigned callReferences = 0;
    bool present = false;
    bool ambiguous = false;
    bool committed = false;
    bool protectionPending = false;
    bool flushPending = false;
};
struct RuntimeNativeImportPlan {
    std::array<RuntimeNativeImportSite, kRuntimeNativeImportCount> sites = {};
    unsigned capabilities = 0;
    unsigned ambiguousCapabilities = 0;
    // Gear can configure these two imports independently when protectors split
    // KERNEL32 and USER32 across different native descriptor tables.
    DWORD clockTickSlotRva = 0;
    DWORD clockTimerSlotRva = 0;
    uint64_t fingerprint = 0;
};

bool ResolveRuntimeNativeImports(const BYTE* image, size_t imageSize,
    uintptr_t runtimeBase, const RuntimeNativeImportTargets& targets,
    RuntimeNativeImportPlan& plan, std::wstring& error);
bool CommitRuntimeNativeImports(BYTE* image, size_t imageSize,
    RuntimeNativeImportPlan& plan, std::wstring& error);
bool RestoreRuntimeNativeImports(BYTE* image, size_t imageSize,
    RuntimeNativeImportPlan& plan, std::wstring& error);
bool RuntimeNativeImportsStillOwned(const BYTE* image, size_t imageSize,
    const RuntimeNativeImportPlan& plan);

// The protected 2.2.0 image has no useful disk raw bytes for its unpacked
// code.  This small bridge is therefore deliberately limited to the audited
// in-memory image and three named import slots.  It does not search arbitrary
// writable memory or accept a caller supplied offset.
struct ProtectedNativeImportPlan {
    size_t slotOffsets[3] = {};
    void* originals[3] = {};
    void* replacements[3] = {};
    DWORD protections[3] = {};
    bool committed[3] = {};
    bool protectionPending[3] = {};
    bool flushPending[3] = {};
    // Descriptor containing the preserved import table.  Gear's native clock
    // bridge must use the same table rather than assuming the audited 2.2.0
    // RVA when a version-selected image exposes a different table.
    DWORD descriptorRva = 0;
};

bool ResolveProtectedNativeImports(const BYTE* image, size_t imageSize, uintptr_t runtimeBase,
    void* createFile, void* translateMessage, void* dispatchMessage,
    ProtectedNativeImportPlan& plan, std::wstring& error);

// Version-selected installations retain the program's current file wrapper
// and resolve named imports without requiring one exact protected build.
bool ResolveVersionNativeImports(const BYTE* image, size_t imageSize,
    ProtectedNativeImportPlan& plan, std::wstring& error);

bool CommitProtectedNativeImports(BYTE* image, size_t imageSize,
    ProtectedNativeImportPlan& requested, std::wstring& error);

bool RestoreProtectedNativeImports(BYTE* image, size_t imageSize,
    ProtectedNativeImportPlan& plan, std::wstring& error);

// A loader gate uses this only after a successful bounded read of the exact
// protected PE shape.  It performs no allocation, file I/O or UI work.
bool InstallProtectedNativeImportGate(BYTE* image, size_t imageSize,
    uintptr_t runtimeBase, void* createFile, void* translateMessage,
    void* dispatchMessage, void* replacementCreateFile,
    void* replacementTranslateMessage, void* replacementDispatchMessage,
    ProtectedNativeImportPlan& plan, std::wstring& error);

#ifdef XA66_TEST
enum class RuntimeNativeImportFault { None, Writable, RestoreProtection, Flush };
void SetRuntimeNativeImportFaultForTest(RuntimeNativeImportFault fault, unsigned failures);
#endif
