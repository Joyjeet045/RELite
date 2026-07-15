#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "txn/lock_manager.hpp"
#include "txn/transaction_manager.hpp"
#include "txn/wal.hpp"

using namespace db;
using db::txn::LockMode;

namespace {

void testLockManager() {
    txn::LockManager lm;
    vm::RecordID rid{0, 0};

    assert(lm.tryLock(1, rid, LockMode::Shared));
    assert(lm.tryLock(2, rid, LockMode::Shared));
    assert(!lm.tryLock(3, rid, LockMode::Exclusive));

    lm.unlock(1, rid);
    lm.unlock(2, rid);
    assert(lm.tryLock(3, rid, LockMode::Exclusive));
    assert(!lm.tryLock(4, rid, LockMode::Shared));
    lm.unlockAll(3);

    assert(lm.tryLock(5, rid, LockMode::Shared));
    assert(lm.tryLock(5, rid, LockMode::Exclusive));
    lm.unlockAll(5);
    assert(lm.tryLock(6, rid, LockMode::Exclusive));
    lm.unlockAll(6);
}

void testWalRoundTrip() {
    std::string path = "prqlite_test_wal.log";
    {
        txn::WriteAheadLog wal(path, /*truncate=*/true);
        txn::LogRecord begin;
        begin.txnId = 1;
        begin.type = txn::LogType::Begin;
        wal.append(begin);

        txn::LogRecord ins;
        ins.txnId = 1;
        ins.type = txn::LogType::Insert;
        ins.tableId = 0;
        ins.rid = vm::RecordID{2, 3};
        ins.afterImage = "row-bytes";
        wal.append(ins);

        txn::LogRecord commit;
        commit.txnId = 1;
        commit.type = txn::LogType::Commit;
        wal.append(commit);

        txn::LogRecord begin2;
        begin2.txnId = 2;
        begin2.type = txn::LogType::Begin;
        wal.append(begin2);
    }

    txn::WriteAheadLog wal(path);
    auto records = wal.readAll();
    assert(records.size() == 4);
    assert(records[1].afterImage == "row-bytes");
    assert(records[1].rid == (vm::RecordID{2, 3}));

    auto committed = txn::WriteAheadLog::committedTxns(records);
    assert(committed.size() == 1 && committed[0] == 1);

    std::remove(path.c_str());
}

void testTransactionUndo() {
    txn::TransactionManager tm(nullptr, nullptr);

    int t = tm.begin();
    assert(tm.isActive(t));
    std::vector<int> trace;
    tm.registerUndo(t, [&] { trace.push_back(1); });
    tm.registerUndo(t, [&] { trace.push_back(2); });
    tm.registerUndo(t, [&] { trace.push_back(3); });
    tm.rollback(t);
    assert(!tm.isActive(t));
    assert((trace == std::vector<int>{3, 2, 1}));

    int t2 = tm.begin();
    bool ran = false;
    tm.registerUndo(t2, [&] { ran = true; });
    tm.commit(t2);
    assert(!ran);
    assert(!tm.isActive(t2));
}

void testTransactionWithWal() {
    std::string path = "prqlite_test_txnwal.log";
    txn::WriteAheadLog wal(path, /*truncate=*/true);
    txn::LockManager lm;
    txn::TransactionManager tm(&wal, &lm);

    int t = tm.begin();
    tm.logInsert(t, 0, vm::RecordID{0, 0}, "after");
    tm.commit(t);

    auto records = wal.readAll();
    auto committed = txn::WriteAheadLog::committedTxns(records);
    assert(committed.size() == 1 && committed[0] == t);

    std::remove(path.c_str());
}

}

int main() {
    testLockManager();
    testWalRoundTrip();
    testTransactionUndo();
    testTransactionWithWal();
    std::cout << "All txn tests passed.\n";
    return 0;
}
