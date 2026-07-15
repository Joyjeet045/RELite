#include "backend/disk_manager.hpp"

#include <cstring>
#include <ios>
#include <vector>

#include "backend/durability.hpp"

namespace db::backend {

DiskManager::DiskManager(const std::string& path, bool truncate) : path_(path) {
    auto mode = std::ios::in | std::ios::out | std::ios::binary;
    if (truncate) {
        std::fstream create(path_, std::ios::out | std::ios::binary | std::ios::trunc);
    }
    file_.open(path_, mode);
    if (!file_.is_open()) {
        file_.clear();
        std::fstream create(path_, std::ios::out | std::ios::binary);
        create.close();
        file_.open(path_, mode);
    }

    file_.seekg(0, std::ios::end);
    std::streamoff bytes = file_.tellg();
    if (bytes < 0) bytes = 0;
    numPages_ = static_cast<int>(bytes / PAGE_SIZE);
}

DiskManager::~DiskManager() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void DiskManager::readPage(PageId id, char* out) {
    std::lock_guard<std::mutex> lock(mtx_);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(id) * PAGE_SIZE, std::ios::beg);
    file_.read(out, PAGE_SIZE);
    std::streamsize got = file_.gcount();
    if (got < PAGE_SIZE) {
        std::memset(out + got, 0, PAGE_SIZE - got);
    }
    file_.clear();
}

void DiskManager::writePage(PageId id, const char* in) {
    std::lock_guard<std::mutex> lock(mtx_);
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(id) * PAGE_SIZE, std::ios::beg);
    file_.write(in, PAGE_SIZE);
    file_.flush();
    if (id + 1 > numPages_) {
        numPages_ = id + 1;
    }
}

PageId DiskManager::allocatePage() {
    std::lock_guard<std::mutex> lock(mtx_);
    PageId id = numPages_++;
    std::vector<char> zeros(PAGE_SIZE, 0);
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(id) * PAGE_SIZE, std::ios::beg);
    file_.write(zeros.data(), PAGE_SIZE);
    file_.flush();
    return id;
}

int DiskManager::numPages() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return numPages_;
}

void DiskManager::sync() {
    std::lock_guard<std::mutex> lock(mtx_);
    file_.flush();
    syncFileToDisk(path_);
}

}
