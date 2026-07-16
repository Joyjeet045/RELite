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

struct Index {
    std::string name;
    int tableId;
    int columnIndex;
    std::vector<int> columns;
    BPlusTree tree;
    BloomFilter bloom;

    Index(std::string n, int t, std::vector<int> cols)
        : name(std::move(n)), tableId(t),
          columnIndex(cols.empty() ? -1 : cols.front()),
          columns(std::move(cols)), tree(), bloom(4096) {}

    bool isComposite() const { return columns.size() > 1; }
    bool coversRow(std::size_t width) const {
        for (int c : columns) {
            if (c < 0 || c >= static_cast<int>(width)) return false;
        }
        return true;
    }
    vm::Value keyOf(const std::vector<vm::Value>& row) const;
    vm::Value keyFromValues(const std::vector<vm::Value>& picked) const;

    void add(const vm::Value& key, const vm::RecordID& rid);
    bool remove(const vm::Value& key, const vm::RecordID& rid);
    std::vector<vm::RecordID> lookup(const vm::Value& key) const;
};

class IndexManager {
public:
    Index* create(const std::string& name, int tableId, std::vector<int> columns);
    bool exists(const std::string& name) const;
    bool drop(const std::string& name);

    std::vector<Index*> forTable(int tableId) const;

    Index* find(int tableId, int columnIndex) const;

    void dropTable(int tableId);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Index>> byName_;
};

}
