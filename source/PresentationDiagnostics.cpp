#include "PresentationDiagnostics.h"
#include "InstallCommon.h"
#include <cstdio>
#include <cwchar>

namespace {
struct LastErrorGuard {
    DWORD value = GetLastError();
    ~LastErrorGuard() { SetLastError(value); }
};
struct Operation {
    uintptr_t token, owner, state;
    size_t stateBytes;
    unsigned domain, kind;
    int camp;
    bool canceled;
    LONGLONG started;
    unsigned nativeWaits, canceledWaits, nativeComposes, canceledComposes, submits, deferred;
    DWORD before;
};
thread_local Operation operations[64] = {};
thread_local Operation detached[128] = {};
thread_local size_t depth = 0;
volatile LONG serial = 0;
INIT_ONCE initialized = INIT_ONCE_STATIC_INIT;
SRWLOCK fileLock = SRWLOCK_INIT;
wchar_t logPath[MAX_PATH] = {};
using Counter = BOOL (WINAPI*)(LARGE_INTEGER*);
Counter counter = nullptr;
LONGLONG frequency = 0;
BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    counter = reinterpret_cast<Counter>(GetProcAddress(kernel,"QueryPerformanceCounter"));
    const auto getFrequency = reinterpret_cast<Counter>(GetProcAddress(kernel,"QueryPerformanceFrequency"));
    LARGE_INTEGER value = {};
    if (getFrequency && getFrequency(&value)) frequency = value.QuadPart;
    if (!GetModuleFileNameW(nullptr,logPath,_countof(logPath))) return TRUE;
    wchar_t* slash = wcsrchr(logPath,L'\\');
    if (!slash) { logPath[0] = 0; return TRUE; }
    *slash = 0;
    if (wcscat_s(logPath,L"\\patch")) return TRUE;
    CreateDirectoryW(logPath,nullptr);
    if (wcscat_s(logPath,CompiledEngineProfile() == EngineProfileId::Ccz64 ? L"\\xiaoan64" :
        CompiledEngineProfile() == EngineProfileId::Ccz65 ? L"\\xiaoan65" : L"\\xiaoan66")) return TRUE;
    CreateDirectoryW(logPath,nullptr);
    if (wcscat_s(logPath,L"\\presentation-operations.log")) logPath[0] = 0;
    return TRUE;
}
LONGLONG Now() noexcept {
    LARGE_INTEGER value = {};
    return counter && counter(&value) ? value.QuadPart : 0;
}
DWORD Digest(uintptr_t address, size_t bytes) noexcept {
    if (!address || !bytes || bytes > 8192) return 0;
    DWORD result = 2166136261u;
    __try {
        for (size_t i = 0; i < bytes; ++i) result = (result ^ reinterpret_cast<const BYTE*>(address)[i]) * 16777619u;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return result;
}
void Emit(const Operation& operation,const char* result) noexcept {
    if (!logPath[0]) return;
    const LONGLONG finished = Now();
    const LONGLONG micros = frequency > 0 && finished >= operation.started ?
        (finished - operation.started) * 1000000 / frequency : 0;
    char line[768] = {};
    const int length = sprintf_s(line,
        "pid=%lu thread=%lu op=%u build=%ls profile=%u domain=%u owner=0x%08X kind=%u camp=%d selected=%u "
        "wait.native=%u wait.canceled=%u compose.native=%u compose.canceled=%u submit=%u deferred=%u "
        "real.us=%lld state.before=%08lX state.after=%08lX result=%s\r\n",
        GetCurrentProcessId(),GetCurrentThreadId(),static_cast<unsigned>(operation.token),
        XiaoAnInstall::kPackageVersion,static_cast<unsigned>(CompiledEngineProfile()),operation.domain,
        static_cast<unsigned>(operation.owner),operation.kind,operation.camp,operation.canceled ? 1u : 0u,
        operation.nativeWaits,operation.canceledWaits,operation.nativeComposes,operation.canceledComposes,
        operation.submits,operation.deferred,micros,operation.before,Digest(operation.state,operation.stateBytes),result);
    if (length <= 0) return;
    AcquireSRWLockExclusive(&fileLock);
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    bool writable = true;
    if (GetFileAttributesExW(logPath,GetFileExInfoStandard,&attributes) &&
        (attributes.nFileSizeHigh || attributes.nFileSizeLow > 2 * 1024 * 1024)) {
        wchar_t previous[MAX_PATH] = {};
        writable = !wcscpy_s(previous,logPath) && !wcscat_s(previous,L".1") &&
            MoveFileExW(logPath,previous,MOVEFILE_REPLACE_EXISTING);
    }
    if (!writable) { ReleaseSRWLockExclusive(&fileLock); return; }
    HANDLE file = CreateFileW(logPath,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file,line,static_cast<DWORD>(length),&written,nullptr);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&fileLock);
}
}
uintptr_t BeginPresentationDiagnostic(unsigned domain,uintptr_t owner,unsigned kind,int camp,
    bool canceled,uintptr_t state,size_t stateBytes) noexcept {
    const LastErrorGuard preserve;
    if (depth == _countof(operations)) return 0;
    InitOnceExecuteOnce(&initialized,&Initialize,nullptr,nullptr);
    uintptr_t token = static_cast<DWORD>(InterlockedIncrement(&serial));
    if (!token) token = static_cast<DWORD>(InterlockedIncrement(&serial));
    Operation operation = {};
    operation.token = token; operation.domain = domain; operation.owner = owner;
    operation.kind = kind; operation.camp = camp; operation.canceled = canceled;
    operation.state = state; operation.stateBytes = stateBytes;
    operation.before = Digest(state,stateBytes); operation.started = Now();
    operations[depth++] = operation;
    return token;
}
void EndPresentationDiagnostic(uintptr_t token,const char* result) noexcept {
    const LastErrorGuard preserve;
    if (!token || !depth) return;
    size_t owner = depth;
    while (owner && operations[owner - 1].token != token) --owner;
    if (!owner) return;
    // An SEH/longjmp can bypass an independently owned native child. Closing
    // its proven live ancestor also retires every intervening diagnostic.
    while (depth > owner) Emit(operations[--depth],"unwind");
    const Operation operation = operations[--depth];
    Emit(operation,result ? result : "complete");
}
void RecordPresentationWait(bool canceled) noexcept {
    for (size_t i = 0; i < depth; ++i) {
        if (canceled) ++operations[i].canceledWaits; else ++operations[i].nativeWaits;
    }
}
void RecordPresentationCompose(bool canceled) noexcept {
    for (size_t i = 0; i < depth; ++i) {
        if (canceled) ++operations[i].canceledComposes; else ++operations[i].nativeComposes;
    }
}
void RecordPresentationSubmit(bool deferred) noexcept {
    for (size_t i = 0; i < depth; ++i) {
        ++operations[i].submits; if (deferred) ++operations[i].deferred;
    }
}
void ResetPresentationDiagnostics(const char* reason) noexcept {
    const LastErrorGuard preserve;
    while (depth) EndPresentationDiagnostic(operations[depth - 1].token,reason);
    for (auto& operation : detached) {
        if (!operation.token) continue;
        Emit(operation,reason ? reason : "reset");operation=Operation();
    }
}
uintptr_t BeginDetachedPresentationDiagnostic(unsigned domain,uintptr_t owner,unsigned kind,int camp,
    bool canceled,uintptr_t state,size_t stateBytes) noexcept {
    const LastErrorGuard preserve;
    for (auto& operation : detached) {
        if (operation.token) continue;
        InitOnceExecuteOnce(&initialized,&Initialize,nullptr,nullptr);
        uintptr_t token=static_cast<DWORD>(InterlockedIncrement(&serial));
        if (!token) token=static_cast<DWORD>(InterlockedIncrement(&serial));
        operation=Operation();operation.token=token;operation.owner=owner;operation.domain=domain;
        operation.kind=kind;operation.camp=camp;operation.canceled=canceled;
        operation.state=state;operation.stateBytes=stateBytes;operation.before=Digest(state,stateBytes);
        operation.started=Now();return token;
    }
    return 0;
}
void RecordDetachedPresentationWait(uintptr_t token,bool canceled) noexcept {
    if (!token) return;
    for (auto& operation : detached) if (operation.token==token) {
        if (canceled) ++operation.canceledWaits;else ++operation.nativeWaits;
        return;
    }
}
void RecordDetachedPresentationCompose(uintptr_t token,bool canceled) noexcept {
    if (!token) return;
    for (auto& operation : detached) if (operation.token==token) {
        if (canceled) ++operation.canceledComposes;else ++operation.nativeComposes;
        return;
    }
}
void EndDetachedPresentationDiagnostic(uintptr_t token,const char* result) noexcept {
    const LastErrorGuard preserve;
    if (!token) return;
    for (auto& operation : detached) if (operation.token==token) {
        Emit(operation,result ? result : "complete");operation=Operation();return;
    }
}
