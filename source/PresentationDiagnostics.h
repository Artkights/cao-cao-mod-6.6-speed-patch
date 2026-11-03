#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>

// Frame hot paths update only thread-local counters. One bounded record is
// emitted when an owned presentation ends. Logging cannot change its result.
uintptr_t BeginPresentationDiagnostic(unsigned domain, uintptr_t owner,
    unsigned kind, int camp, bool canceled, uintptr_t state = 0, size_t stateBytes = 0) noexcept;
void EndPresentationDiagnostic(uintptr_t token, const char* result = "complete") noexcept;
void RecordPresentationWait(bool canceled) noexcept;
void RecordPresentationCompose(bool canceled) noexcept;
void RecordPresentationSubmit(bool deferred) noexcept;
// Native asynchronous R objects outlive their script handler's stack frame.
// Detached ownership never changes the synchronous LIFO operation stack.
uintptr_t BeginDetachedPresentationDiagnostic(unsigned domain, uintptr_t owner,
    unsigned kind, int camp, bool canceled, uintptr_t state, size_t stateBytes) noexcept;
void RecordDetachedPresentationWait(uintptr_t token, bool canceled) noexcept;
void RecordDetachedPresentationCompose(uintptr_t token, bool canceled) noexcept;
void EndDetachedPresentationDiagnostic(uintptr_t token, const char* result) noexcept;
void ResetPresentationDiagnostics(const char* reason) noexcept;
