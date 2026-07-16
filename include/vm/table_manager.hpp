#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "backend/buffer_pool.hpp"
#include "vm/record_id.hpp"

namespace db::vm {

class TableManager {
public:
    explicit TableManager(backend::BufferPool* pool) : pool_(pool) {}

    void registerTable(int tableId);
    bool hasTable(int tableId) const;
    void dropTable(int tableId);
    void truncateTable(int tableId);

    RecordID insertTuple(int tableId, const std::string& bytes);

    bool getTuple(int tableId, const RecordID& rid, std::string& out);

    bool eraseTuple(int tableId, const RecordID& rid);

    RecordID updateTuple(int tableId, const RecordID& rid, const std::string& bytes);

    /* Recovery hooks: place / clear a tuple at an exact RecordID. redo* are
     * idempotent (guarded by the page LSN); undo* always apply. */
    void redoInsert(int tableId, const RecordID& rid, const std::string& bytes,
                    std::uint64_t lsn);
    void redoDelete(int tableId, const RecordID& rid, std::uint64_t lsn);
    void undoInsert(int tableId, const RecordID& rid);
    void undoDelete(int tableId, const RecordID& rid, const std::string& bytes);

    const std::vector<backend::PageId>& pageList(int tableId) const;
    backend::BufferPool* pool() const { return pool_; }

    void restorePages(int tableId, std::vector<backend::PageId> pages) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pages_[tableId] = std::move(pages);
    }

private:
    backend::BufferPool* pool_;
    mutable std::recursive_mutex mutex_;
    std::unordered_map<int, std::vector<backend::PageId>> pages_;
    std::unordered_map<backend::PageId, int> pageFree_;
    static const std::vector<backend::PageId> kEmpty;
};

class TableIterator {
public:
    TableIterator(TableManager* manager, int tableId);

    bool valid() const { return valid_; }
    void next();
    const RecordID& rid() const { return rid_; }
    const std::string& bytes() const { return bytes_; }

private:
    void advanceToLive();

    TableManager* manager_;
    int tableId_;
    std::size_t pageIdx_ = 0;
    int slot_ = -1;
    RecordID rid_;
    std::string bytes_;
    bool valid_ = false;
};

}
