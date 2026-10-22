#include "SaveTransactions.h"
#include "BattleStatsPersistence.h"
#include "EngineProfile.h"
#include <algorithm>
#include <filesystem>
#include <vector>
#include <cstdio>
#include <cstring>
#include <new>
#include <array>
#include <bcrypt.h>

namespace {
struct SavedFile { std::wstring path, backup; bool existed = false; };
struct Transaction {
    bool active = false, failed = false, closed = false;
    bool extensionRequired = false, extensionObserved = false;
    unsigned writes = 0;
    unsigned long long bytes = 0;
    std::wstring root, temporary, journal, nativeExtension, targetExtension, extensionError;
    HANDLE rootLock = INVALID_HANDLE_VALUE;
    BattleStatsPersistedState stats;
    std::vector<SavedFile> originals;
};
thread_local Transaction gTransaction;
thread_local HANDLE gGroupReadLock = INVALID_HANDLE_VALUE;
volatile LONG gTransactionOwner = 0;
volatile LONG gSequence = 0;
struct OwnerGuard {
    bool held = true;
    ~OwnerGuard() { if (held) InterlockedExchange(&gTransactionOwner, 0); }
    void ReleaseOwnership() noexcept { held = false; }
};
struct HandleGuard {
    HANDLE handle;
    explicit HandleGuard(HANDLE value) noexcept : handle(value) {}
    ~HandleGuard() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    HANDLE Detach() noexcept { const HANDLE value = handle; handle = INVALID_HANDLE_VALUE; return value; }
};
void SetErrorNoThrow(std::wstring& error, const wchar_t* message) noexcept {
    try { error = message; } catch (...) { error.clear(); }
}
void ReleaseFailedTransaction() noexcept {
    if (gTransaction.rootLock != INVALID_HANDLE_VALUE) CloseHandle(gTransaction.rootLock);
    gTransaction = Transaction();
    InterlockedExchange(&gTransactionOwner, 0);
}
bool ReadMemory(uintptr_t address, void* output, size_t bytes) noexcept {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        output, bytes, &read) && read == bytes;
}
bool HashWriterBody(const std::array<BYTE, 0x385>& body, std::array<BYTE, 32>& digest) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<BYTE, 1024> object = {};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), static_cast<ULONG>(object.size()),
        nullptr, 0, 0) >= 0;
    if (ok) ok = BCryptHashData(hash, const_cast<BYTE*>(body.data()), static_cast<ULONG>(body.size()), 0) >= 0;
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}
bool ValidateNativeExtension(uintptr_t nativeWriter, const std::wstring& path, std::wstring& error) {
    HMODULE module = nullptr;
    if (!nativeWriter || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(nativeWriter), &module) ||
        nativeWriter != reinterpret_cast<uintptr_t>(module) + 0xDBF0) {
        error = L"无法验证扩展存档写入器，原保存组不会被替换。"; return false;
    }
    std::array<BYTE, 0x385> body = {};
    if (!ReadMemory(nativeWriter, body.data(), body.size())) {
        error = L"无法读取扩展存档写入器，原保存组不会被替换。"; return false;
    }
    // SHA-256 covers the entire verified SaveVar function, including all
    // loops, strides, writes and closing calls. Normalize exactly its PE
    // HIGHLOW relocations so ASLR does not change the recognized body.
    constexpr unsigned relocations[] = {0xA,0x14,0x1E,0x8B,0xC0,0x15F,0x17C,
        0x1DE,0x21E,0x28B,0x2CB,0x30D,0x34D};
    for (const unsigned offset : relocations) {
        uint32_t value = 0; memcpy(&value, body.data() + offset, sizeof(value));
        value -= static_cast<uint32_t>(reinterpret_cast<uintptr_t>(module));
        memcpy(body.data() + offset, &value, sizeof(value));
    }
    constexpr std::array<BYTE, 32> expectedHash = {0xb4,0xe9,0x4a,0x86,0x5b,0xee,0x77,0xbc,
        0x08,0x47,0x85,0xf9,0x6c,0x71,0x2b,0x7e,0x70,0xdc,0xfd,0xe7,0xef,0x42,0xf1,0xe7,
        0xb9,0x73,0x50,0xaf,0x1b,0xa6,0x98,0xc7};
    std::array<BYTE, 32> digest = {};
    if (!HashWriterBody(body, digest) || digest != expectedHash) {
        error = L"扩展存档写入器版本或指令结构未通过验证，原保存组不会被替换。"; return false;
    }
    constexpr unsigned operandOffsets[] = {0x15F,0x1DE,0x28B,0x30D};
    constexpr uint32_t capacities[] = {50,256,32,1024};
    constexpr uint64_t multipliers[] = {1024*4,1024*4,1024*2,64};
    uint64_t expectedBytes = 0;
    for (size_t index = 0; index < 4; ++index) {
        uint32_t operand = 0, count = 0;
        memcpy(&operand, body.data() + operandOffsets[index], sizeof(operand));
        if (!ReadMemory(reinterpret_cast<uintptr_t>(module) + operand, &count, sizeof(count)) ||
            count > capacities[index]) {
            error = L"扩展存档运行维度无效，无法确认完整性，原保存组不会被替换。"; return false;
        }
        expectedBytes += multipliers[index] * count;
    }
    if (!expectedBytes) { error = L"扩展存档运行维度为空，无法确认完整性。"; return false; }
    HandleGuard file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr));
    LARGE_INTEGER actual = {};
    if (file.handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.handle, &actual) ||
        actual.QuadPart < 0 || static_cast<uint64_t>(actual.QuadPart) != expectedBytes ||
        !FlushFileBuffers(file.handle)) {
        error = L"新扩展存档未完整写入，长度或刷新校验失败，原保存组将恢复。"; return false;
    }
    return true;
}
#ifdef XA66_TEST
int gFault = 0;
#endif
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool Flush(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = FlushFileBuffers(h) != FALSE;
    ok = CloseHandle(h) != FALSE && ok;
    return ok;
}
bool InRoot(const std::wstring& root, const std::wstring& path) {
    auto r = std::filesystem::path(root).lexically_normal().wstring();
    auto p = std::filesystem::path(path).lexically_normal().wstring();
    if (!r.empty() && r.back() != L'\\') r += L'\\';
    return p.size() > r.size() && _wcsnicmp(r.c_str(), p.c_str(), r.size()) == 0;
}
HANDLE LockRoot(const std::wstring& root, std::wstring& error) {
    const std::wstring folder = PatchPrivatePath(root, CompiledEngineProfile()) + L"\\transactions";
    std::error_code code;
    std::filesystem::create_directories(folder, code);
    if (code) {
        error = L"无法建立存档事务目录，原存档未被替换。";
        return INVALID_HANDLE_VALUE;
    }
    // The OS releases this exclusive handle if a game process crashes. A
    // second process must not recover or overwrite another process's active
    // journal while its save is still running.
    HANDLE result = CreateFileW((folder + L"\\root.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (result == INVALID_HANDLE_VALUE)
        error = L"另一游戏实例正在操作此目录的存档，或事务锁无法打开。请稍后重试。";
    return result;
}
bool WriteJournal(const Transaction& t, bool committed) {
    std::wstring text = L"[Transaction]\r\nCommitted=" + std::to_wstring(committed ? 1 : 0) +
        L"\r\nTemporary=" + t.temporary + L"\r\nCount=" + std::to_wstring(t.originals.size()) + L"\r\n";
    for (size_t i = 0; i < t.originals.size(); ++i) {
        const auto& f = t.originals[i];
        text += L"Path" + std::to_wstring(i) + L"=" + f.path + L"\r\nBackup" + std::to_wstring(i) +
            L"=" + f.backup + L"\r\nExisted" + std::to_wstring(i) + L"=" + std::to_wstring(f.existed ? 1 : 0) + L"\r\n";
    }
    const std::wstring temp = t.journal + L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; const wchar_t bom = 0xfeff;
    bool ok = WriteFile(h, &bom, 2, &written, nullptr) && written == 2;
    DWORD size = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    ok = ok && WriteFile(h, text.data(), size, &written, nullptr) && written == size && FlushFileBuffers(h);
    ok = CloseHandle(h) != FALSE && ok;
    return ok && MoveFileExW(temp.c_str(), t.journal.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
bool Restore(const Transaction& t) {
    bool ok = true;
    for (const auto& f : t.originals) {
        if (f.existed) {
            // Keep the backup until every member is restored, including on a
            // second crash during recovery. Never rename away the only backup.
            const std::wstring restore = f.path + L".xa-restore";
            bool member = CopyFileW(f.backup.c_str(), restore.c_str(), FALSE) && Flush(restore) &&
                MoveFileExW(restore.c_str(), f.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            ok = member && ok;
        } else if (!DeleteFileW(f.path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) ok = false;
    }
    return ok;
}
void Cleanup(const Transaction& t) {
    DeleteFileW(t.temporary.c_str());
    for (const auto& f : t.originals) { DeleteFileW(f.backup.c_str()); DeleteFileW((f.path + L".xa-restore").c_str()); }
    DeleteFileW(t.journal.c_str()); DeleteFileW((t.journal + L".tmp").c_str());
}
std::wstring Ini(const std::wstring& p, const std::wstring& key) {
    std::vector<wchar_t> b(32768);
    GetPrivateProfileStringW(L"Transaction", key.c_str(), L"", b.data(), static_cast<DWORD>(b.size()), p.c_str());
    return b.data();
}
bool ReadBoolean(const std::wstring& p, const std::wstring& key, bool& result) {
    const std::wstring value = Ini(p, key);
    if (value != L"0" && value != L"1") return false;
    result = value == L"1";
    return true;
}
bool RecoverUnlocked(const std::wstring& root, std::wstring& error) {
    const std::wstring folder = PatchPrivatePath(root, CompiledEngineProfile()) + L"\\transactions";
    WIN32_FIND_DATAW data = {}; HANDLE search = FindFirstFileW((folder + L"\\*.ini").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    bool ok = true;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Transaction t; t.root = root; t.journal = folder + L"\\" + data.cFileName;
        t.temporary = Ini(t.journal, L"Temporary");
        const std::wstring count = Ini(t.journal, L"Count");
        bool committed = false;
        if (count.size() != 1 || count[0] < L'1' || count[0] > L'3' ||
            !ReadBoolean(t.journal, L"Committed", committed) || !InRoot(root, t.temporary)) {
            ok = false; break;
        }
        const int n = count[0] - L'0';
        for (int i = 0; i < n; ++i) {
            SavedFile f; f.path = Ini(t.journal, L"Path" + std::to_wstring(i));
            f.backup = Ini(t.journal, L"Backup" + std::to_wstring(i));
            if (!ReadBoolean(t.journal, L"Existed" + std::to_wstring(i), f.existed) ||
                !InRoot(root, f.path) || f.backup != f.path + L".xa-old" ||
                !_wcsicmp(t.temporary.c_str(), f.path.c_str()) ||
                std::any_of(t.originals.begin(), t.originals.end(), [&](const SavedFile& other) {
                    return !_wcsicmp(other.path.c_str(), f.path.c_str());
                })) { ok = false; break; }
            t.originals.push_back(f);
        }
        if (!ok) break;
        if (!committed && !Restore(t)) { ok = false; break; }
        Cleanup(t);
    } while (FindNextFileW(search, &data));
    FindClose(search);
    if (!ok) error = L"上次存档事务尚未完整恢复，请保留 transactions 目录和 .xa-old 备份。";
    return ok;
}
}
bool RecoverSaveTransactionsImpl(const std::wstring& root, std::wstring& error) {
    if (InterlockedCompareExchange(&gTransactionOwner, 1, 0)) { error = L"存档事务正在进行。"; return false; }
    OwnerGuard owner;
    HandleGuard rootLock(LockRoot(root, error));
#ifdef XA66_TEST
    if (gFault == 6) throw std::bad_alloc();
#endif
    return rootLock.handle != INVALID_HANDLE_VALUE && RecoverUnlocked(root, error);
}
bool BeginSaveGroupReadImpl(const std::wstring& root, std::wstring& error) {
    if (gGroupReadLock != INVALID_HANDLE_VALUE ||
        InterlockedCompareExchange(&gTransactionOwner, 1, 0)) {
        error = L"另一次存读档事务尚未结束。"; return false;
    }
    OwnerGuard owner;
    HandleGuard rootLock(LockRoot(root, error));
#ifdef XA66_TEST
    if (gFault == 7) throw std::bad_alloc();
#endif
    if (rootLock.handle == INVALID_HANDLE_VALUE || !RecoverUnlocked(root, error)) return false;
    gGroupReadLock = rootLock.Detach();
    owner.ReleaseOwnership();
    return true;
}
void EndSaveGroupRead() noexcept {
    // TLS ownership makes a failed/nested begin or a different thread's
    // cleanup incapable of releasing the active reader's lock.
    if (gGroupReadLock == INVALID_HANDLE_VALUE) return;
    CloseHandle(gGroupReadLock); gGroupReadLock = INVALID_HANDLE_VALUE;
    InterlockedExchange(&gTransactionOwner, 0);
}
bool BeginSaveTransactionImpl(const std::wstring& root, const std::wstring& target,
    const std::wstring& nativeExtension, const std::wstring& targetExtension, char* shortFilename,
    std::wstring& error, bool requireNativeExtension) {
    if (!shortFilename || gTransaction.active || InterlockedCompareExchange(&gTransactionOwner, 1, 0)) {
        error = L"另一次存读档事务尚未结束。"; return false;
    }
    OwnerGuard owner;
    Transaction t; t.root = root; t.nativeExtension = nativeExtension; t.targetExtension = targetExtension;
    t.extensionRequired = requireNativeExtension;
    t.rootLock = LockRoot(root, error);
    HandleGuard rootLock(t.rootLock);
    if (t.rootLock == INVALID_HANDLE_VALUE) return false;
    if (!RecoverUnlocked(root,error)) {
        // This journal belongs to an earlier interrupted transaction. Never
        // let cleanup of a new attempt destroy its recovery information.
        return false;
    }
    bool ok = InRoot(root, target) && InRoot(root, nativeExtension) && InRoot(root, targetExtension);
    std::error_code ec;
    const std::wstring folder = PatchPrivatePath(root, CompiledEngineProfile()) + L"\\transactions";
    if (ok) { std::filesystem::create_directories(folder, ec); ok = !ec; }
    if (ok) { std::filesystem::create_directories(std::filesystem::path(target).parent_path(), ec); ok = !ec; }
    char name[16] = {};
    for (int attempt = 0; ok && attempt < 256; ++attempt) {
        unsigned sequence = static_cast<unsigned>(InterlockedIncrement(&gSequence)) ^ GetCurrentProcessId() ^ GetTickCount();
        sprintf_s(name, "SV\\X%06X.E5S", sequence & 0xffffff);
        t.temporary = root + L"\\" + std::wstring(name, name + strlen(name));
        HANDLE h = CreateFileW(t.temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); break; }
        t.temporary.clear();
    }
    ok = ok && !t.temporary.empty() && CaptureBattleStatsState(t.stats, GetTickCount64());
    for (const auto& p : {target, nativeExtension, targetExtension}) {
        if (!ok) break;
        if (std::any_of(t.originals.begin(), t.originals.end(), [&](const SavedFile& f) {
            return !_wcsicmp(f.path.c_str(), p.c_str());
        })) continue;
        SavedFile f; f.path = p; f.backup = p + L".xa-old"; f.existed = Exists(p);
        if (f.existed && (!CopyFileW(p.c_str(), f.backup.c_str(), FALSE) || !Flush(f.backup))) ok = false;
        t.originals.push_back(f);
    }
    t.journal = folder + L"\\active.ini";
    const bool journalReady = ok && WriteJournal(t, false);
    ok = journalReady;
    if (ok && requireNativeExtension) {
        // The next extension callback must create a fresh file. Keeping an
        // old EXS here would let a skipped or failed callback commit stale
        // extended data with a newly written E5S.
        ok = DeleteFileW(nativeExtension.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    }
#ifdef XA66_TEST
    if (ok && gFault == 4) throw std::bad_alloc();
#endif
    if (!ok) {
        if (!journalReady || Restore(t)) Cleanup(t);
        else error = L"无法启动保存，原扩展存档恢复尚未完成。已保留事务日志和备份。";
        if (error.empty()) error = L"无法建立完整的存档临时文件和恢复备份。";
        return false;
    }
    t.active = true; gTransaction = std::move(t); strcpy_s(shortFilename, 16, name);
    rootLock.Detach(); owner.ReleaseOwnership(); return true;
}
void ObserveSaveWrite(unsigned requested, unsigned written) {
    if (!gTransaction.active) return;
    ++gTransaction.writes; gTransaction.bytes += written;
    if (requested != written) gTransaction.failed = true;
}
void ObserveSaveClose(bool succeeded) {
    if (gTransaction.active) { gTransaction.closed = true; gTransaction.failed = gTransaction.failed || !succeeded; }
}
void ObserveSaveExtension(bool succeeded) {
    if (gTransaction.active) {
        gTransaction.extensionObserved = true;
        gTransaction.failed = gTransaction.failed || !succeeded;
    }
}
void ObserveNativeSaveExtension(uintptr_t nativeWriter) noexcept {
    if (!gTransaction.active) return;
    try {
        const bool ok = ValidateNativeExtension(nativeWriter, gTransaction.nativeExtension,
            gTransaction.extensionError);
        ObserveSaveExtension(ok);
    } catch (...) {
        ObserveSaveExtension(false);
        SetErrorNoThrow(gTransaction.extensionError, L"验证新扩展存档时遇到异常，原保存组将恢复。");
    }
}
bool SaveTransactionActive() { return gTransaction.active; }
bool SaveTransactionHealthy() { return gTransaction.active && !gTransaction.failed && gTransaction.closed; }
bool FinishSaveTransactionImpl(std::wstring& error) {
    if (!gTransaction.active) { error = L"存档事务未启动，原存档未被替换。"; return false; }
    auto& t = gTransaction;
    if (t.failed && !t.extensionError.empty()) error = t.extensionError;
    bool ok = !t.failed && t.closed && t.writes == 4;
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    ok = ok && GetFileAttributesExW(t.temporary.c_str(), GetFileExInfoStandard, &info) &&
        ((static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) == t.bytes;
    ok = ok && WriteBattleStatsEmbedded(t.temporary, t.stats, error);
    if (ok && t.extensionRequired) {
        ok = t.extensionObserved && Exists(t.nativeExtension) && Flush(t.nativeExtension);
        if (ok && t.nativeExtension != t.targetExtension) {
            ok = CopyFileW(t.nativeExtension.c_str(), t.targetExtension.c_str(), FALSE) && Flush(t.targetExtension);
        }
    }
#ifdef XA66_TEST
    if (gFault == 1) ok = false;
#endif
    if (ok) ok = MoveFileExW(t.temporary.c_str(), t.originals.front().path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#ifdef XA66_TEST
    if (gFault == 2) ok = false;
    if (gFault == 3) ExitProcess(91);
    if (gFault == 5) throw std::bad_alloc();
#endif
    if (ok && t.extensionRequired && t.nativeExtension != t.targetExtension) {
        // Private autosaves must restore the numbered EXS touched by the
        // engine, so the player's ordinary pair is not overwritten.
        for (const auto& f : t.originals) if (f.path == t.nativeExtension) {
            if (f.existed) ok = CopyFileW(f.backup.c_str(), f.path.c_str(), FALSE) && Flush(f.path);
            else ok = DeleteFileW(f.path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
        }
    }
    ok = ok && WriteJournal(t, true);
    if (ok || Restore(t)) Cleanup(t);
    else error = L"保存失败，恢复尚未完成。已保留事务日志和原始备份，禁止读取不完整存档。";
    ReleaseFailedTransaction();
    if (!ok && error.empty()) error = L"存档写入或校验失败，已恢复上一份完整存档。";
    return ok;
}
void AbortSaveTransactionImpl() {
    if (!gTransaction.active) return;
    if (Restore(gTransaction)) Cleanup(gTransaction);
    ReleaseFailedTransaction();
}
bool RecoverSaveTransactions(const std::wstring& root, std::wstring& error) noexcept {
    try { return RecoverSaveTransactionsImpl(root, error); }
    catch (...) {
        SetErrorNoThrow(error, L"存档恢复遇到内存或文件处理异常。恢复日志和备份已保留，请稍后重试。");
        return false;
    }
}
bool BeginSaveGroupRead(const std::wstring& root, std::wstring& error) noexcept {
    try { return BeginSaveGroupReadImpl(root, error); }
    catch (...) {
        SetErrorNoThrow(error, L"无法准备读取存档，恢复日志和备份已保留。");
        return false;
    }
}
bool BeginSaveTransaction(const std::wstring& root, const std::wstring& target,
    const std::wstring& nativeExtension, const std::wstring& targetExtension, char* shortFilename,
    std::wstring& error, bool requireNativeExtension) noexcept {
    try {
        return BeginSaveTransactionImpl(root, target, nativeExtension, targetExtension,
            shortFilename, error, requireNativeExtension);
    } catch (...) {
        SetErrorNoThrow(error, L"无法准备保存，原存档未被提交覆盖。若已有事务，其日志和备份已保留。");
        return false;
    }
}
bool FinishSaveTransaction(std::wstring& error) noexcept {
    try { return FinishSaveTransactionImpl(error); }
    catch (...) {
        // Do not allocate or attempt a second potentially throwing recovery.
        // The durable journal describes the previous complete save group;
        // the next read/save recovers it while holding the same root lock.
        if (gTransaction.active) ReleaseFailedTransaction();
        SetErrorNoThrow(error, L"保存遇到内存或文件处理异常，事务日志和原始备份已保留，重新读档前将恢复。");
        return false;
    }
}
void AbortSaveTransaction() noexcept {
    try { AbortSaveTransactionImpl(); }
    catch (...) { if (gTransaction.active) ReleaseFailedTransaction(); }
}
#ifdef XA66_TEST
void SetSaveTransactionFaultForTest(int fault) { gFault = fault; }
bool ValidateNativeExtensionForTest(uintptr_t nativeWriter, const std::wstring& path,
    std::wstring& error) noexcept {
    try { return ValidateNativeExtension(nativeWriter, path, error); }
    catch (...) { SetErrorNoThrow(error, L"验证扩展存档时遇到异常。"); return false; }
}
#endif
