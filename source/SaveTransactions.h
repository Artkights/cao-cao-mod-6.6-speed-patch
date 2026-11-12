#pragma once
#include "BattleStats.h"
#include <string>

// Only the verified native E5S writer calls these functions. Other game I/O
// is never redirected. All paths are absolute and belong to this game root.
// Journals retain legacy Committed compatibility: Prepared requires restore;
// Committed/Restored are cleanup-only. Backups survive until either outcome
// is durably recorded. E5S-only transactions never inspect either EXS path;
// callers should pass empty extension paths when the verified graph has none.
enum class SaveRecoveryStatus : LONG { Ready, RecoveryPending, CleanupPending };
// Nonblocking status only; never acquires a root lock or retries recovery.
// CleanupPending still permits the just-committed save to report success.
SaveRecoveryStatus ReadSaveRecoveryStatus() noexcept;
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
// Existing faults 1..7 retain their original meanings. Additional deterministic
// points: 8 snapshot allocation, 9 backup allocation, 10 restored-marker failure,
// 11 cleanup failure, 12 enumeration allocation, 13 enumeration I/O failure,
// 14 commit-marker failure. Child-process crash points: 21 after restore,
// 22 after Restored marker, 24 before journal deletion, 25 after Committed;
// 200+i after restoring member i; 230+i after deleting backup i.
void SetSaveTransactionFaultForTest(int fault);
bool ValidateNativeExtensionForTest(uintptr_t nativeWriter, const std::wstring& path,
    std::wstring& error) noexcept;
#endif
