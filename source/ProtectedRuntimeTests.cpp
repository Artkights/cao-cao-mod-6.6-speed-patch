#include "ProtectedRuntime.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
void Check(bool value, const char* label) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}
template<class Image> void PutDword(Image& image, size_t offset, DWORD value) {
    memcpy(image.data() + offset, &value, sizeof(value));
}
template<class Image> void PutName(Image& image, size_t offset, const char* name) {
    memcpy(image.data() + offset + sizeof(WORD), name, strlen(name) + 1);
}
template<class Image> void PutRuntimeDescriptor(Image& image, size_t offset,
    DWORD name, DWORD firstThunk) {
    IMAGE_IMPORT_DESCRIPTOR descriptor = {};
    descriptor.Name = name;
    descriptor.FirstThunk = firstThunk;
    memcpy(image.data() + offset, &descriptor, sizeof(descriptor));
}
template<class Image> void PutRuntimeCall(Image& image, size_t call, size_t slot, uintptr_t base) {
    image[call] = 0xFF;
    image[call + 1] = 0x15;
    PutDword(image, call + 2, static_cast<DWORD>(base + slot));
}
class RuntimeImage {
    BYTE* bytes_ = nullptr;
    size_t size_ = 0;
public:
    RuntimeImage() = default;
    RuntimeImage(const RuntimeImage&) = delete;
    RuntimeImage& operator=(const RuntimeImage&) = delete;
    ~RuntimeImage() { if (bytes_) VirtualFree(bytes_, 0, MEM_RELEASE); }
    bool Allocate(size_t size) {
        if (bytes_) VirtualFree(bytes_, 0, MEM_RELEASE);
        bytes_ = static_cast<BYTE*>(VirtualAlloc(nullptr, size,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        size_ = bytes_ ? size : 0;
        return bytes_ != nullptr;
    }
    BYTE* data() { return bytes_; }
    size_t size() const { return size_; }
    BYTE& operator[](size_t offset) { return bytes_[offset]; }
    bool Protect(size_t offset, size_t length, DWORD protection) {
        DWORD previous = 0;
        return bytes_ && offset <= size_ && length <= size_ - offset &&
            VirtualProtect(bytes_ + offset, length, protection, &previous) != FALSE;
    }
};

bool MakeRuntimeFixture(RuntimeImage& image,
    RuntimeNativeImportTargets& targets) {
    if (!image.Allocate(0x6000)) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(image.data());
    if (base > UINT32_MAX || image.size() > UINT32_MAX - base) return false;
    IMAGE_DOS_HEADER dos = {};
    dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 0x80;
    memcpy(image.data(), &dos, sizeof(dos));
    IMAGE_NT_HEADERS32 nt = {};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt.FileHeader.NumberOfSections = 3;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt.OptionalHeader.ImageBase = 0x400000;
    nt.OptionalHeader.SizeOfImage = static_cast<DWORD>(image.size());
    nt.OptionalHeader.SizeOfHeaders = 0x400;
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = 0x2000;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = 3 * sizeof(IMAGE_IMPORT_DESCRIPTOR);
    memcpy(image.data() + 0x80, &nt, sizeof(nt));
    IMAGE_SECTION_HEADER sections[3] = {};
    sections[0].VirtualAddress = 0x1000; sections[0].Misc.VirtualSize = 0x1000;
    sections[0].SizeOfRawData = 0x1000; sections[0].Characteristics = IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ;
    sections[1].VirtualAddress = 0x2000; sections[1].Misc.VirtualSize = 0x2000;
    sections[1].SizeOfRawData = 0x2000; sections[1].Characteristics = IMAGE_SCN_MEM_READ;
    sections[2].VirtualAddress = 0x4000; sections[2].Misc.VirtualSize = 0x1000;
    sections[2].SizeOfRawData = 0x1000; sections[2].Characteristics = IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE;
    memcpy(image.data() + 0x80 + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        sizeof(IMAGE_OPTIONAL_HEADER32), sections, sizeof(sections));
    PutRuntimeDescriptor(image, 0x2000, 0x2200, 0x3000);
    PutRuntimeDescriptor(image, 0x2014, 0x2240, 0x3040);
    memcpy(image.data() + 0x2200, "KERNEL32.dll", sizeof("KERNEL32.dll"));
    memcpy(image.data() + 0x2240, "USER32.dll", sizeof("USER32.dll"));
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::CreateFileA)] = reinterpret_cast<void*>(&CreateFileA);
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::CreateFileW)] = reinterpret_cast<void*>(&CreateFileW);
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::TranslateMessage)] = reinterpret_cast<void*>(&TranslateMessage);
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::DispatchMessageA)] = reinterpret_cast<void*>(&DispatchMessageA);
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::DispatchMessageW)] = reinterpret_cast<void*>(&DispatchMessageW);
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::GetTickCount)] = reinterpret_cast<void*>(&GetTickCount);
    targets.originals[static_cast<size_t>(RuntimeNativeImportKind::SetTimer)] = reinterpret_cast<void*>(&SetTimer);
    // Match natural import libraries: file/tick entries in KERNEL32 and
    // message/timer entries in USER32; the same plan still resolves each
    // capability independently across descriptor tables.
    const size_t slots[] = {0x3000,0x3004,0x3040,0x3044,0x3048,0x3008,0x304C};
    for (size_t index = 0; index < _countof(slots); ++index)
        PutDword(image, slots[index], static_cast<DWORD>(reinterpret_cast<uintptr_t>(targets.originals[index])));
    const size_t calls[] = {0x1100,0x1110,0x1120,0x1130,0x1140,0x1150,0x1160};
    for (size_t index = 0; index < _countof(calls); ++index)
        PutRuntimeCall(image, calls[index], slots[index], base);
    return image.Protect(0x1000, 0x1000, PAGE_EXECUTE_READWRITE);
}
}

int main() {
    std::vector<BYTE> image(0x246000, 0);
    const uintptr_t base = reinterpret_cast<uintptr_t>(image.data());
    IMAGE_IMPORT_DESCRIPTOR create = {};
    create.Name = 0x90000; create.OriginalFirstThunk = 0x91000; create.FirstThunk = 0x86118;
    memcpy(image.data() + 0x89240, &create, sizeof(create));
    IMAGE_IMPORT_DESCRIPTOR user = {};
    user.Name = 0x90020; user.OriginalFirstThunk = 0x91020; user.FirstThunk = 0x86310;
    memcpy(image.data() + 0x89254, &user, sizeof(user));
    memcpy(image.data() + 0x90000, "KERNEL32.dll", sizeof("KERNEL32.dll"));
    memcpy(image.data() + 0x90020, "USER32.dll", sizeof("USER32.dll"));
    PutName(image, 0x92000, "CreateFileA");
    PutName(image, 0x92020, "TranslateMessage");
    PutName(image, 0x92040, "DispatchMessageA");
    PutDword(image, 0x91000, 0x92000); PutDword(image, 0x91004, 0);
    PutDword(image, 0x91020, 0x92020); PutDword(image, 0x91024, 0x92040); PutDword(image, 0x91028, 0);
    const size_t callOffsets[] = {0x18FA5,0x18FF1,0x19284,0x6D6F9,0x7EAE3,0x7EB5E,
        0x6D703,0x7EAED,0x7EB68};
    for (size_t call : callOffsets) { image[call] = 0xFF; image[call + 1] = 0x15; }
    PutDword(image, 0x18FA5 + 2, 0x86118 + static_cast<DWORD>(base));
    PutDword(image, 0x18FF1 + 2, 0x86118 + static_cast<DWORD>(base));
    PutDword(image, 0x19284 + 2, 0x86118 + static_cast<DWORD>(base));
    PutDword(image, 0x6D6F9 + 2, 0x86310 + static_cast<DWORD>(base));
    PutDword(image, 0x7EAE3 + 2, 0x86310 + static_cast<DWORD>(base));
    PutDword(image, 0x7EB5E + 2, 0x86310 + static_cast<DWORD>(base));
    PutDword(image, 0x6D703 + 2, 0x86314 + static_cast<DWORD>(base));
    PutDword(image, 0x7EAED + 2, 0x86314 + static_cast<DWORD>(base));
    PutDword(image, 0x7EB68 + 2, 0x86314 + static_cast<DWORD>(base));
    PutDword(image, 0x1625E3, 0x16873C + static_cast<DWORD>(base));
    PutDword(image, 0x16873C, static_cast<DWORD>(reinterpret_cast<uintptr_t>(&CreateFileA)));
    PutDword(image, 0x86118, 0x1625A3 + static_cast<DWORD>(base));
    PutDword(image, 0x86310, static_cast<DWORD>(reinterpret_cast<uintptr_t>(&TranslateMessage)));
    PutDword(image, 0x86314, static_cast<DWORD>(reinterpret_cast<uintptr_t>(&DispatchMessageA)));
    PutDword(image, 0x1625A3, 0x90909090);
    ProtectedNativeImportPlan plan;
    std::wstring error;
    const bool resolved = ResolveProtectedNativeImports(image.data(), image.size(), base,
        reinterpret_cast<void*>(&CreateFileA), reinterpret_cast<void*>(&TranslateMessage),
        reinterpret_cast<void*>(&DispatchMessageA), plan, error);
    if (!resolved) std::printf("resolve error: %ls\n", error.c_str());
    Check(resolved,
        "synthetic Protected220 import graph resolves uniquely");
    plan.replacements[0] = reinterpret_cast<void*>(&CloseHandle);
    plan.replacements[1] = reinterpret_cast<void*>(&DefWindowProcA);
    plan.replacements[2] = reinterpret_cast<void*>(&DefWindowProcA);
    Check(CommitProtectedNativeImports(image.data(), image.size(), plan, error),
        "three protected import slots commit atomically");
    const bool restored = RestoreProtectedNativeImports(image.data(), image.size(), plan, error);
    if (!restored) std::printf("restore error: %ls\n", error.c_str());
    Check(restored,
        "three protected import slots restore atomically");
    image[0x18FA5] = 0x90;
    Check(!ResolveProtectedNativeImports(image.data(), image.size(), base,
        reinterpret_cast<void*>(&CreateFileA), reinterpret_cast<void*>(&TranslateMessage),
        reinterpret_cast<void*>(&DispatchMessageA), plan, error),
        "changed FF15 call relation is rejected");
    const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    const auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data() + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 5;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(image.size());
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = 0x89240;
    Check(ResolveVersionNativeImports(image.data(), image.size(), plan, error) &&
        plan.originals[0] == reinterpret_cast<void*>(base + 0x1625A3),
        "version import resolution preserves the VFS wrapper despite changed code signatures");
    plan.replacements[0] = reinterpret_cast<void*>(&CloseHandle);
    plan.replacements[1] = reinterpret_cast<void*>(&DefWindowProcA);
    plan.replacements[2] = reinterpret_cast<void*>(&DefWindowProcA);
    Check(CommitProtectedNativeImports(image.data(), image.size(), plan, error) &&
        RestoreProtectedNativeImports(image.data(), image.size(), plan, error),
        "version import commit and restore retain actual original targets");

    // The same named import appearing twice in one table is ambiguous.  The
    // resolver must reject that table instead of taking its first slot.
    PutName(image, 0x92100, "CreateFileA");
    PutDword(image, 0x91004, 0x92100);
    PutDword(image, 0x91008, 0);
    PutDword(image, 0x8611C, static_cast<DWORD>(reinterpret_cast<uintptr_t>(&CreateFileA)));
    Check(!ResolveVersionNativeImports(image.data(), image.size(), plan, error),
        "duplicate named import in one candidate table is rejected");

    RuntimeImage runtimeImage;
    RuntimeNativeImportTargets runtimeTargets;
    Check(MakeRuntimeFixture(runtimeImage, runtimeTargets),
        "synthetic 6.6 runtime PE and independent imports are prepared");
    RuntimeNativeImportPlan runtimePlan;
    const uintptr_t runtimeBase = reinterpret_cast<uintptr_t>(runtimeImage.data());
    const bool runtimeResolved = ResolveRuntimeNativeImports(runtimeImage.data(), runtimeImage.size(),
        runtimeBase, runtimeTargets, runtimePlan, error);
    if (!runtimeResolved || runtimePlan.capabilities != 0x7F ||
        runtimePlan.clockTickSlotRva != 0x3008 || runtimePlan.clockTimerSlotRva != 0x304C)
        std::printf("runtime import error: %ls; caps=0x%X tick=0x%X timer=0x%X\n", error.c_str(),
            runtimePlan.capabilities, runtimePlan.clockTickSlotRva, runtimePlan.clockTimerSlotRva);
    Check(runtimeResolved && runtimePlan.capabilities == 0x7F &&
        runtimePlan.clockTickSlotRva == 0x3008 && runtimePlan.clockTimerSlotRva == 0x304C,
        "OFT-less native IAT resolves seven individually cross-referenced imports and split clock slots");
    RuntimeNativeImportPlan invalidAddressPlan;
    Check(!ResolveRuntimeNativeImports(runtimeImage.data(), runtimeImage.size(), runtimeBase + 0x1000,
        runtimeTargets, invalidAddressPlan, error),
        "a copied snapshot cannot masquerade as a different live module address");
    Check(runtimeImage.Protect(0x1000, 0x1000, PAGE_READWRITE),
        "runtime call page can become temporarily non-executable while unpacking");
    RuntimeNativeImportPlan nonExecutablePlan;
    Check(!ResolveRuntimeNativeImports(runtimeImage.data(), runtimeImage.size(), runtimeBase,
        runtimeTargets, nonExecutablePlan, error) && !nonExecutablePlan.capabilities,
        "PE execute flags alone do not authorize calls from a non-executable runtime page");
    Check(runtimeImage.Protect(0x1000, 0x1000, PAGE_EXECUTE_READWRITE),
        "runtime call page returns to executable protection");
    RuntimeImage wrapperImage;
    RuntimeNativeImportTargets wrapperTargets;
    Check(MakeRuntimeFixture(wrapperImage, wrapperTargets), "VFS wrapper fixture is prepared");
    const uintptr_t wrapperBase = reinterpret_cast<uintptr_t>(wrapperImage.data());
    const size_t wrapperSlots[] = {0x3000,0x3004,0x3040,0x3044,0x3048,0x3008,0x304C};
    const size_t wrapperCalls[] = {0x1100,0x1110,0x1120,0x1130,0x1140,0x1150,0x1160};
    for (size_t index = 0; index < _countof(wrapperSlots); ++index)
        PutRuntimeCall(wrapperImage, wrapperCalls[index], wrapperSlots[index], wrapperBase);
    IMAGE_IMPORT_DESCRIPTOR wrapperKernel = {};
    wrapperKernel.Name = 0x2200; wrapperKernel.OriginalFirstThunk = 0x3200; wrapperKernel.FirstThunk = 0x3000;
    memcpy(wrapperImage.data() + 0x2000, &wrapperKernel, sizeof(wrapperKernel));
    IMAGE_IMPORT_DESCRIPTOR wrapperUser = {};
    wrapperUser.Name = 0x2240; wrapperUser.OriginalFirstThunk = 0x3240; wrapperUser.FirstThunk = 0x3040;
    memcpy(wrapperImage.data() + 0x2014, &wrapperUser, sizeof(wrapperUser));
    const char* importNames[] = {"CreateFileA","CreateFileW","GetTickCount","TranslateMessage",
        "DispatchMessageA","DispatchMessageW","SetTimer"};
    const size_t nameRvas[] = {0x3400,0x3420,0x3440,0x3460,0x3480,0x34A0,0x34C0};
    const size_t thunkRvas[] = {0x3200,0x3204,0x3208,0x3240,0x3244,0x3248,0x324C};
    for (size_t index = 0; index < _countof(importNames); ++index) {
        PutDword(wrapperImage, thunkRvas[index], static_cast<DWORD>(nameRvas[index]));
        PutName(wrapperImage, nameRvas[index], importNames[index]);
    }
    PutDword(wrapperImage, 0x3000, static_cast<DWORD>(wrapperBase + 0x1700));
    wrapperImage[0x1700] = 0xC3;
    PutRuntimeDescriptor(wrapperImage, 0x2500, 0x2280, 0x3080);
    const auto wrapperNt = reinterpret_cast<IMAGE_NT_HEADERS32*>(wrapperImage.data() + 0x80);
    wrapperNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = 0x2500;
    memcpy(wrapperImage.data() + 0x2280, "KERNEL32.dll", sizeof("KERNEL32.dll"));
    PutDword(wrapperImage, 0x3080, static_cast<DWORD>(reinterpret_cast<uintptr_t>(&CreateFileA)));
    PutRuntimeCall(wrapperImage, 0x1170, 0x3080, wrapperBase);
    RuntimeNativeImportPlan wrapperPlan;
    Check(ResolveRuntimeNativeImports(wrapperImage.data(), wrapperImage.size(), wrapperBase,
        wrapperTargets, wrapperPlan, error) &&
        wrapperPlan.sites[static_cast<size_t>(RuntimeNativeImportKind::CreateFileA)].slotOffset == 0x3000 &&
        wrapperPlan.sites[static_cast<size_t>(RuntimeNativeImportKind::CreateFileA)].original ==
            reinterpret_cast<void*>(wrapperBase + 0x1700) &&
        wrapperPlan.sites[static_cast<size_t>(RuntimeNativeImportKind::TranslateMessage)].present,
        "a preserved native table is discovered behind a shell directory and retains its VFS wrapper");
    const auto wrapperSections = reinterpret_cast<IMAGE_SECTION_HEADER*>(wrapperImage.data() +
        0x80 + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(IMAGE_OPTIONAL_HEADER32));
    wrapperSections[2].Characteristics |= IMAGE_SCN_MEM_EXECUTE;
    PutDword(wrapperImage, 0x3000, static_cast<DWORD>(wrapperBase + 0x4100));
    wrapperImage[0x4100] = 0xC3;
    Check(ResolveRuntimeNativeImports(wrapperImage.data(), wrapperImage.size(), wrapperBase,
        wrapperTargets, wrapperPlan, error) &&
        !wrapperPlan.sites[static_cast<size_t>(RuntimeNativeImportKind::CreateFileA)].present &&
        wrapperPlan.sites[static_cast<size_t>(RuntimeNativeImportKind::TranslateMessage)].present,
        "an unfinished VFS page blocks only its file capability instead of falling back to the shell OS slot");
    Check(wrapperImage.Protect(0x4000, 0x1000, PAGE_EXECUTE_READWRITE) &&
        ResolveRuntimeNativeImports(wrapperImage.data(), wrapperImage.size(), wrapperBase,
            wrapperTargets, wrapperPlan, error) &&
        wrapperPlan.sites[static_cast<size_t>(RuntimeNativeImportKind::CreateFileA)].original ==
            reinterpret_cast<void*>(wrapperBase + 0x4100),
        "the native VFS target becomes ready only after its real page is executable");
    for (size_t index = 0; index < kRuntimeNativeImportCount; ++index)
        runtimePlan.sites[index].replacement = reinterpret_cast<void*>(&CloseHandle);
    Check(CommitRuntimeNativeImports(runtimeImage.data(), runtimeImage.size(), runtimePlan, error) &&
        RuntimeNativeImportsStillOwned(runtimeImage.data(), runtimeImage.size(), runtimePlan),
        "independent native imports commit only after all slots pass ownership checks");
    PutDword(runtimeImage, 0x3000, static_cast<DWORD>(reinterpret_cast<uintptr_t>(&DefWindowProcA)));
    Check(!RuntimeNativeImportsStillOwned(runtimeImage.data(), runtimeImage.size(), runtimePlan) &&
        !RestoreRuntimeNativeImports(runtimeImage.data(), runtimeImage.size(), runtimePlan, error) &&
        runtimePlan.sites[static_cast<size_t>(RuntimeNativeImportKind::CreateFileA)].committed &&
        *reinterpret_cast<void**>(runtimeImage.data() + 0x3000) == reinterpret_cast<void*>(&DefWindowProcA),
        "a later writer is retained and its conflicted restore journal stays pending");
    PutDword(runtimeImage, 0x3000, static_cast<DWORD>(reinterpret_cast<uintptr_t>(&CloseHandle)));
    Check(RestoreRuntimeNativeImports(runtimeImage.data(), runtimeImage.size(), runtimePlan, error),
        "remaining imported entries restore when the conflict is resolved and recovery is retried");

    // The ordinary shell table can retain a file import while the original
    // native message and clock table is separately discoverable in memory.
    runtimeImage[0x1120] = 0x90;
    Check(ResolveRuntimeNativeImports(runtimeImage.data(), runtimeImage.size(), runtimeBase,
        runtimeTargets, runtimePlan, error) &&
        !(runtimePlan.capabilities & RuntimeNativeImportBit(RuntimeNativeImportKind::TranslateMessage)) &&
        (runtimePlan.capabilities & RuntimeNativeImportBit(RuntimeNativeImportKind::CreateFileA)),
        "missing message call disables only that capability and preserves file forwarding");

    // A duplicate slot with the same entry point and a real call is ambiguous,
    // even though other import capabilities remain independently usable.
    PutRuntimeDescriptor(runtimeImage, 0x2028, 0x2200, 0x3060);
    PutDword(runtimeImage, 0x3060, static_cast<DWORD>(reinterpret_cast<uintptr_t>(&CreateFileA)));
    PutRuntimeCall(runtimeImage, 0x1170, 0x3060, runtimeBase);
    Check(ResolveRuntimeNativeImports(runtimeImage.data(), runtimeImage.size(), runtimeBase,
        runtimeTargets, runtimePlan, error) &&
        (runtimePlan.ambiguousCapabilities & RuntimeNativeImportBit(RuntimeNativeImportKind::CreateFileA)) &&
        !(runtimePlan.capabilities & RuntimeNativeImportBit(RuntimeNativeImportKind::CreateFileA)) &&
        (runtimePlan.capabilities & RuntimeNativeImportBit(RuntimeNativeImportKind::CreateFileW)),
        "duplicate live import calls report ambiguity without blocking other functions");
#ifdef XA66_TEST
    for (const auto fault : {RuntimeNativeImportFault::Writable,
        RuntimeNativeImportFault::RestoreProtection, RuntimeNativeImportFault::Flush}) {
        RuntimeImage faultImage;
        RuntimeNativeImportTargets faultTargets;
        RuntimeNativeImportPlan faultPlan;
        const bool prepared = MakeRuntimeFixture(faultImage, faultTargets) &&
            ResolveRuntimeNativeImports(faultImage.data(), faultImage.size(),
                reinterpret_cast<uintptr_t>(faultImage.data()), faultTargets, faultPlan, error) &&
            faultImage.Protect(0x3000, 0x1000, PAGE_READONLY);
        Check(prepared, "a dedicated readonly IAT page is prepared for metadata fault recovery");
        if (!prepared) continue;
        auto& faultSite = faultPlan.sites[static_cast<size_t>(RuntimeNativeImportKind::CreateFileA)];
        faultSite.replacement = reinterpret_cast<void*>(&CloseHandle);
        SetRuntimeNativeImportFaultForTest(fault, 32);
        Check(!CommitRuntimeNativeImports(faultImage.data(), faultImage.size(), faultPlan, error),
            "writable, page-restore and cache-flush faults each reject the import transaction");
        const bool pending = faultSite.protectionPending || faultSite.flushPending;
        Check(!faultSite.committed &&
            *reinterpret_cast<void**>(faultImage.data() + faultSite.slotOffset) == faultSite.original &&
            (fault == RuntimeNativeImportFault::Writable ? !pending : pending),
            "original bytes are restored while unresolved page or cache metadata remains journaled");
        MEMORY_BASIC_INFORMATION beforeRecovery = {};
        VirtualQuery(faultImage.data() + faultSite.slotOffset, &beforeRecovery, sizeof(beforeRecovery));
        Check(fault != RuntimeNativeImportFault::RestoreProtection ||
            (faultSite.protectionPending && beforeRecovery.Protect == PAGE_READWRITE),
            "a failed permission restoration preserves the original readonly protection in its journal");
        SetRuntimeNativeImportFaultForTest(RuntimeNativeImportFault::None, 0);
        MEMORY_BASIC_INFORMATION afterRecovery = {};
        const bool recovered = RestoreRuntimeNativeImports(faultImage.data(), faultImage.size(), faultPlan, error);
        VirtualQuery(faultImage.data() + faultSite.slotOffset, &afterRecovery, sizeof(afterRecovery));
        Check(recovered && !faultSite.committed && !faultSite.protectionPending &&
            !faultSite.flushPending && afterRecovery.Protect == PAGE_READONLY,
            "recovery can retry metadata alone after original bytes have already returned");
    }
#endif
    std::printf("ProtectedRuntimeTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
