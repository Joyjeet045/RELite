#include "txn/transaction_manager.hpp"

namespace db::txn {

TransactionManager::TransactionManager(WriteAheadLog* wal, LockManager* lockManager)
    : wal_(wal), locks_(lockManager) {}

int TransactionManager::begin() {
    std::lock_guard<std::mutex> lock(mtx_);
    int id = nextTxnId_++;
    active_[id];
    if (wal_ != nullptr) {
        LogRecord r;
        r.txnId = id;
        r.type = LogType::Begin;
        wal_->append(r);
    }
    return id;
}

bool TransactionManager::isActive(int txnId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return active_.count(txnId) != 0;
}

void TransactionManager::registerUndo(int txnId, std::function<void()> undo) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = active_.find(txnId);
    if (it != active_.end()) {
        it->second.undo.push_back(std::move(undo));
    }
}

bool TransactionManager::lockShared(int txnId, const vm::RecordID& rid) {
    if (locks_ == nullptr) return true;
    return locks_->lock(txnId, rid, LockMode::Shared);
}

bool TransactionManager::lockExclusive(int txnId, const vm::RecordID& rid) {
    if (locks_ == nullptr) return true;
    return locks_->lock(txnId, rid, LockMode::Exclusive);
}

void TransactionManager::logInsert(int txnId, int tableId, const vm::RecordID& rid,
                                   const std::string& afterImage) {
    if (wal_ == nullptr) return;
    LogRecord r;
    r.txnId = txnId;
    r.type = LogType::Insert;
    r.tableId = tableId;
    r.rid = rid;
    r.afterImage = afterImage;
    wal_->append(std::move(r));
}

void TransactionManager::logDelete(int txnId, int tableId, const vm::RecordID& rid,
                                   const std::string& beforeImage) {
    if (wal_ == nullptr) return;
    LogRecord r;
    r.txnId = txnId;
    r.type = LogType::Delete;
    r.tableId = tableId;
    r.rid = rid;
    r.beforeImage = beforeImage;
    wal_->append(std::move(r));
}

void TransactionManager::commit(int txnId) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (active_.count(txnId) == 0) return;
    if (wal_ != nullptr) {
        LogRecord r;
        r.txnId = txnId;
        r.type = LogType::Commit;
        wal_->append(r);
        wal_->flush();
    }
    if (locks_ != nullptr) {
        locks_->unlockAll(txnId);
    }
    active_.erase(txnId);
}

void TransactionManager::rollback(int txnId) {
    std::unique_lock<std::mutex> lock(mtx_);
    auto it = active_.find(txnId);
    if (it == active_.end()) return;

    std::vector<std::function<void()>> undo = std::move(it->second.undo);
    lock.unlock();
    for (auto rit = undo.rbegin(); rit != undo.rend(); ++rit) {
        (*rit)();
    }
    lock.lock();

    if (wal_ != nullptr) {
        LogRecord r;
        r.txnId = txnId;
        r.type = LogType::Abort;
        wal_->append(r);
        wal_->flush();
    }
    if (locks_ != nullptr) {
        locks_->unlockAll(txnId);
    }
    active_.erase(txnId);
}

}
