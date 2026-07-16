#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

#include "backend/buffer_pool.hpp"
#include "vm/record_id.hpp"
#include "vm/value.hpp"

namespace db::index {

/*
 * A page-resident B+ tree. Each node is one buffer-pool page, so the tree is
 * disk-backed and evictable rather than held entirely in RAM. Entries are the
 * composite (indexed value, RecordID); storing one entry per row keeps leaf
 * records small, so duplicate values never overflow a page. Leaves are linked
 * for forward range scans; deletion is lazy (no merge).
 */
class DiskBPlusTree {
public:
    using Key = vm::Value;

    explicit DiskBPlusTree(backend::BufferPool* pool = nullptr);

    void setBufferPool(backend::BufferPool* pool) { pool_ = pool; }

    void insert(const Key& key, const vm::RecordID& rid);
    std::vector<vm::RecordID> lookup(const Key& key) const;
    bool erase(const Key& key, const vm::RecordID& rid);

    std::vector<vm::RecordID> range(const Key& lo, const Key& hi) const;
    std::vector<vm::RecordID> rangeScan(const Key* lo, const Key* hi) const;

    std::size_t distinctKeys() const { return distinctKeys_; }

private:
    struct NodeView {
        bool leaf = true;
        int nextLeaf = -1;
        std::vector<vm::Value> keys;
        std::vector<vm::RecordID> rids;
        std::vector<int> children;
    };

    NodeView readNode(int pageId) const;
    void writeNode(int pageId, const NodeView& view);
    int allocNode(const NodeView& view);

    int findLeaf(const vm::Value& key, const vm::RecordID& rid,
                 std::vector<int>& path) const;
    int firstLeaf() const;

    backend::BufferPool* pool_;
    int rootId_ = -1;
    std::size_t distinctKeys_ = 0;
    mutable std::mutex mutex_;
};

}
