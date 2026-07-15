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

class TransactionManager {
public:
    TransactionManager(WriteAheadLog* wal, LockManager* lockManager);

    int begin();
    bool isActive(int txnId) const;

    void registerUndo(int txnId, std::function<void()> undo);

    bool lockShared(int txnId, const vm::RecordID& rid);
    bool lockExclusive(int txnId, const vm::RecordID& rid);

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

}
