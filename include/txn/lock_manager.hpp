#pragma once

#include <condition_variable>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "vm/record_id.hpp"

namespace db::txn {

enum class LockMode { Shared, Exclusive };

class LockManager {
public:
    bool lock(int txnId, const vm::RecordID& rid, LockMode mode);
    bool tryLock(int txnId, const vm::RecordID& rid, LockMode mode);
    void unlock(int txnId, const vm::RecordID& rid);
    void unlockAll(int txnId);

private:
    struct Entry {
        std::map<int, LockMode> holders;
    };

    bool canGrant(const Entry& entry, int txnId, LockMode mode) const;
    void grant(const vm::RecordID& rid, int txnId, LockMode mode);

    std::mutex mtx_;
    std::condition_variable cv_;
    std::unordered_map<vm::RecordID, Entry> table_;
    std::unordered_map<int, std::vector<vm::RecordID>> heldByTxn_;
};

}
