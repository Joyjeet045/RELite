#pragma once

#include <fstream>
#include <mutex>
#include <string>

#include "backend/page.hpp"

namespace db::backend {

// OS-independent reader/writer of fixed-size pages to a single .db file.
// Pages are addressed by id at byte offset id * PAGE_SIZE. Thread-safe.
class DiskManager {
public:
    explicit DiskManager(const std::string& path, bool truncate = false);
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // Reads a full page into `out` (must hold PAGE_SIZE bytes). Reads past the
    // end of file are zero-filled.
    void readPage(PageId id, char* out);

    // Writes a full page (`in` must hold PAGE_SIZE bytes) and flushes.
    void writePage(PageId id, const char* in);

    // Grows the file by one zeroed page and returns its id.
    PageId allocatePage();

    // Flushes the stream and forces the OS cache to stable storage (fsync).
    // Call after a batch of writePage()s to make them durable.
    void sync();

    int numPages() const;

private:
    mutable std::mutex mtx_;
    std::string path_;
    std::fstream file_;
    int numPages_ = 0;
};

}  // namespace db::backend
