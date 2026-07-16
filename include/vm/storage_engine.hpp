#pragma once

#include <cstddef>
#include <string>

#include "backend/buffer_pool.hpp"
#include "backend/disk_manager.hpp"
#include "index/index_manager.hpp"
#include "vm/table_manager.hpp"
#include "vm/version_store.hpp"

namespace db::vm {

class StorageEngine {
public:
    explicit StorageEngine(const std::string& path, bool truncate = true,
                           std::size_t frames = 128)
        : disk_(path, truncate), pool_(&disk_, frames), tables_(&pool_) {
        indexes_.setBufferPool(&pool_);
    }

    TableManager& tables() { return tables_; }
    index::IndexManager& indexes() { return indexes_; }
    VersionStore& versions() { return versions_; }
    backend::BufferPool& bufferPool() { return pool_; }
    backend::DiskManager& disk() { return disk_; }

    void flush() {
        pool_.flushAll();
        disk_.sync();
    }

private:
    backend::DiskManager disk_;
    backend::BufferPool pool_;
    TableManager tables_;
    index::IndexManager indexes_;
    VersionStore versions_;
};

}
