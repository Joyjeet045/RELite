#pragma once

#include <fstream>
#include <mutex>
#include <string>

#include "backend/page.hpp"

namespace db::backend {

class DiskManager {
public:
    explicit DiskManager(const std::string& path, bool truncate = false);
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    void readPage(PageId id, char* out);

    void writePage(PageId id, const char* in);

    PageId allocatePage();

    void sync();

    int numPages() const;

private:
    mutable std::mutex mtx_;
    std::string path_;
    std::fstream file_;
    int numPages_ = 0;
};

}
