#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "txn/lock_manager.hpp"
#include "txn/wal.hpp"
#include "vm/record_id.hpp"

namespace db::txn {

// Coordinates transactions: assigns ids, records per-transaction undo actions,
// writes WAL records for durability, and releases locks on completion.
// commit() makes changes permanent; rollback() runs the undo actions in reverse
// to restore the pre-transaction state.
//
// wal and lockManager are optional (may be null) so the component can be unit
// tested in isolation.
class TransactionManager {
public:
    TransactionManager(WriteAheadLog* wal, LockManager* lockManager);

    int begin();
    bool isActive(int txnId) const;

    // Registers an action that undoes one change (run in reverse on rollback).
    void registerUndo(int txnId, std::function<void()> undo);

    // Two-phase locking helpers. Acquire a row lock on behalf of a transaction
    // (blocking until grantable). No-ops that succeed when no lock manager is
    // configured. Locks are held until commit()/rollback() releases them.
    bool lockShared(int txnId, const vm::RecordID& rid);
    bool lockExclusive(int txnId, const vm::RecordID& rid);

    // WAL logging helpers for durability.
    void logInsert(int txnId, int tableId, const vm::RecordID& rid,
                   const std::string& afterImage);
    void logDelete(int txnId, int tableId, const vm::RecordID& rid,
                   const std::string& beforeImage);

    void commit(int txnId);
    void rollback(int txnId);

private:
    struct TxnState {
        std::vector<std::function<void()>> undo;
    };

    mutable std::mutex mtx_;
    std::unordered_map<int, TxnState> active_;
    int nextTxnId_ = 1;
    WriteAheadLog* wal_;
    LockManager* locks_;
};

}  // namespace db::txn
