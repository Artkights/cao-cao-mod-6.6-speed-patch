#pragma once
#include "BattleStats.h"
#include <string>

// Only the verified native E5S writer calls these functions. Other game I/O
// is never redirected. All paths are absolute and belong to this game root.
bool RecoverSaveTransactions(const std::wstring& root, std::wstring& error) noexcept;
// Outer native load holds the same process/root lock until all E5S, EXS and
// statistics reads finish. Call End only when Begin returned true.
bool BeginSaveGroupRead(const std::wstring& root, std::wstring& error) noexcept;
void EndSaveGroupRead() noexcept;
bool BeginSaveTransaction(const std::wstring& root, const std::wstring& target,
    const std::wstring& nativeExtension, const std::wstring& targetExtension,
    char* shortFilename, std::wstring& error, bool requireNativeExtension = false) noexcept;
void ObserveSaveWrite(unsigned requested, unsigned written);
void ObserveSaveClose(bool succeeded);
// The verified extension callback must complete for this transaction. A
// pre-existing EXS is removed only after its recovery backup is durable.
void ObserveSaveExtension(bool succeeded);
// Validates the known 6.6 SaveVar implementation and this fresh EXS using
// its actual runtime dimensions; unknown writers cannot commit silently.
void ObserveNativeSaveExtension(uintptr_t nativeWriter) noexcept;
bool FinishSaveTransaction(std::wstring& error) noexcept;
void AbortSaveTransaction() noexcept;
bool SaveTransactionActive();
bool SaveTransactionHealthy();

#ifdef XA66_TEST
void SetSaveTransactionFaultForTest(int fault);
bool ValidateNativeExtensionForTest(uintptr_t nativeWriter, const std::wstring& path,
    std::wstring& error) noexcept;
#endif
