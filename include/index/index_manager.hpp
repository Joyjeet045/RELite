#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "index/bloom_filter.hpp"
#include "index/bplus_tree.hpp"
#include "vm/record_id.hpp"
#include "vm/value.hpp"

namespace db::index {

// One named index: a B+ tree for ordered lookups/ranges plus a Bloom filter
// that lets equality probes bail out early when a key is definitely absent.
struct Index {
    std::string name;
    int tableId;
    int columnIndex;
    BPlusTree tree;
    BloomFilter bloom;

    Index(std::string n, int t, int c)
        : name(std::move(n)), tableId(t), columnIndex(c), tree(), bloom(4096) {}

    void add(const vm::Value& key, const vm::RecordID& rid);
    bool remove(const vm::Value& key, const vm::RecordID& rid);
    std::vector<vm::RecordID> lookup(const vm::Value& key) const;
};

// Registry of all indexes, keyed by index name.
class IndexManager {
public:
    // Creates an index; returns nullptr if the name already exists.
    Index* create(const std::string& name, int tableId, int columnIndex);
    bool exists(const std::string& name) const;
    bool drop(const std::string& name);

    // All indexes defined on a table (for maintenance on insert/delete).
    std::vector<Index*> forTable(int tableId) const;

    // First index defined on a specific (table, column), or nullptr.
    Index* find(int tableId, int columnIndex) const;

    void dropTable(int tableId);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Index>> byName_;
};

}  // namespace db::index
