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

// One append-only log entry. before/after images carry the serialized row bytes
// needed to undo/redo a change.
struct LogRecord {
    lsn_t lsn = 0;
    int txnId = 0;
    LogType type = LogType::Begin;
    int tableId = -1;
    vm::RecordID rid;
    std::string beforeImage;
    std::string afterImage;
};

// Durable, append-only write-ahead log. Every append is flushed so committed
// work survives a crash. readAll() replays the file for recovery.
class WriteAheadLog {
public:
    explicit WriteAheadLog(const std::string& path, bool truncate = false);
    ~WriteAheadLog();

    // Appends a record, assigning and returning its LSN. Thread-safe.
    lsn_t append(LogRecord record);

    // Reads every record from the start of the log (for recovery).
    std::vector<LogRecord> readAll();

    void flush();

    // Truncates the log to empty and restarts LSN numbering. Called after a
    // recovery pass has durably applied the log's effects (a checkpoint).
    void reset();

    // Number of records appended since the log was last reset (checkpoint hint).
    std::size_t pendingRecords() const;

    // Recovery analysis: the set of transaction ids that reached Commit.
    static std::vector<int> committedTxns(const std::vector<LogRecord>& records);

private:
    std::mutex mtx_;
    std::string path_;
    std::fstream file_;
    lsn_t nextLsn_ = 1;
    std::size_t appendsSinceReset_ = 0;
};

}  // namespace db::txn
