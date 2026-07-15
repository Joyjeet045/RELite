#pragma once

#include <memory>
#include <string>

#include "frontend/catalog.hpp"

namespace db {

namespace vm {
class StorageEngine;
}
namespace txn {
class WriteAheadLog;
class LockManager;
class TransactionManager;
}

class DB {
public:
    DB();
    ~DB();

    std::string connect(const std::string& query);

    void run();

private:
    std::unique_ptr<vm::StorageEngine> storage_;
    std::unique_ptr<txn::WriteAheadLog> wal_;
    std::unique_ptr<txn::LockManager> locks_;
    std::unique_ptr<txn::TransactionManager> txnMgr_;
    semantic::Catalog catalog_;
    int currentTxn_ = 0;

    void saveCatalog();
    void loadCatalog();

    void rebuildIndexes();

    void recover();
};

}
