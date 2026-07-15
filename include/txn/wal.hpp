#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "vm/record_id.hpp"

namespace db::txn {

using lsn_t = std::uint64_t;

enum class LogType : std::uint8_t {
    Begin,
    Insert,
    Delete,
    Update,
    Commit,
    Abort,
};

struct LogRecord {
    lsn_t lsn = 0;
    int txnId = 0;
    LogType type = LogType::Begin;
    int tableId = -1;
    vm::RecordID rid;
    std::string beforeImage;
    std::string afterImage;
};

class WriteAheadLog {
public:
    explicit WriteAheadLog(const std::string& path, bool truncate = false);
    ~WriteAheadLog();

    lsn_t append(LogRecord record);

    std::vector<LogRecord> readAll();

    void flush();

    void reset();

    std::size_t pendingRecords() const;

    static std::vector<int> committedTxns(const std::vector<LogRecord>& records);

private:
    std::mutex mtx_;
    std::string path_;
    std::fstream file_;
    lsn_t nextLsn_ = 1;
    std::size_t appendsSinceReset_ = 0;
};

}
