#pragma once

#include <condition_variable>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "vm/record_id.hpp"

namespace db::txn {

enum class LockMode { Shared, Exclusive };

// Row-level lock table for two-phase locking. Multiple transactions may hold a
// Shared lock on a row; an Exclusive lock is exclusive. A transaction holding a
// Shared lock may upgrade to Exclusive once it is the sole holder. lock()
// blocks until the request is grantable; tryLock() never blocks.
//
// Deadlock detection is out of scope (a documented simplification); callers are
// expected to acquire in a consistent order or rely on the single-writer REPL.
class LockManager {
public:
    bool lock(int txnId, const vm::RecordID& rid, LockMode mode);
    bool tryLock(int txnId, const vm::RecordID& rid, LockMode mode);
    void unlock(int txnId, const vm::RecordID& rid);
    void unlockAll(int txnId);

private:
    struct Entry {
        std::map<int, LockMode> holders;  // txnId -> granted mode
    };

    bool canGrant(const Entry& entry, int txnId, LockMode mode) const;
    void grant(const vm::RecordID& rid, int txnId, LockMode mode);

    std::mutex mtx_;
    std::condition_variable cv_;
    std::unordered_map<vm::RecordID, Entry> table_;
    std::unordered_map<int, std::vector<vm::RecordID>> heldByTxn_;
};

}  // namespace db::txn
