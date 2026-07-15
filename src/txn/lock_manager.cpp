#include "txn/lock_manager.hpp"

#include <algorithm>
#include <chrono>

namespace db::txn {

bool LockManager::canGrant(const Entry& entry, int txnId, LockMode mode) const {
    for (const auto& [holder, held] : entry.holders) {
        if (holder == txnId) {
            continue;
        }
        if (mode == LockMode::Exclusive || held == LockMode::Exclusive) {
            return false;
        }
    }
    return true;
}

void LockManager::grant(const vm::RecordID& rid, int txnId, LockMode mode) {
    Entry& entry = table_[rid];
    auto it = entry.holders.find(txnId);
    if (it == entry.holders.end()) {
        entry.holders.emplace(txnId, mode);
        heldByTxn_[txnId].push_back(rid);
    } else if (mode == LockMode::Exclusive) {
        it->second = LockMode::Exclusive;
    }
}

bool LockManager::tryLock(int txnId, const vm::RecordID& rid, LockMode mode) {
    std::lock_guard<std::mutex> lock(mtx_);
    Entry& entry = table_[rid];
    if (!canGrant(entry, txnId, mode)) {
        return false;
    }
    grant(rid, txnId, mode);
    return true;
}

bool LockManager::lock(int txnId, const vm::RecordID& rid, LockMode mode) {
    std::unique_lock<std::mutex> lock(mtx_);
    bool granted = cv_.wait_for(lock, std::chrono::seconds(5), [&] {
        return canGrant(table_[rid], txnId, mode);
    });
    if (!granted) {
        return false;
    }
    grant(rid, txnId, mode);
    return true;
}

void LockManager::unlock(int txnId, const vm::RecordID& rid) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = table_.find(rid);
    if (it != table_.end()) {
        it->second.holders.erase(txnId);
        if (it->second.holders.empty()) {
            table_.erase(it);
        }
    }
    auto& held = heldByTxn_[txnId];
    held.erase(std::remove(held.begin(), held.end(), rid), held.end());
    cv_.notify_all();
}

void LockManager::unlockAll(int txnId) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = heldByTxn_.find(txnId);
    if (it != heldByTxn_.end()) {
        for (const vm::RecordID& rid : it->second) {
            auto entryIt = table_.find(rid);
            if (entryIt != table_.end()) {
                entryIt->second.holders.erase(txnId);
                if (entryIt->second.holders.empty()) {
                    table_.erase(entryIt);
                }
            }
        }
        heldByTxn_.erase(it);
    }
    cv_.notify_all();
}

}
