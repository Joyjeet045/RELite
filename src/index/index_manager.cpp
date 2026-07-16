#include "index/index_manager.hpp"

namespace db::index {

namespace {

std::string keyBytes(const vm::Value& v) {
    switch (v.type) {
        case vm::ValueType::Int:
            return std::string(reinterpret_cast<const char*>(&v.intValue),
                               sizeof(v.intValue));
        case vm::ValueType::Bool:
            return std::string(1, v.boolValue ? '\1' : '\0');
        case vm::ValueType::Text:
            return v.textValue;
        case vm::ValueType::Null:
            return std::string();
    }
    return std::string();
}

}

vm::Value Index::keyFromValues(const std::vector<vm::Value>& picked) const {
    if (picked.size() == 1) return picked.front();
    std::string enc;
    for (const vm::Value& v : picked) {
        enc += std::to_string(static_cast<int>(v.type)) + ':' + v.toString() + '\x1f';
    }
    return vm::Value::makeText(enc);
}

vm::Value Index::keyOf(const std::vector<vm::Value>& row) const {
    std::vector<vm::Value> picked;
    picked.reserve(columns.size());
    for (int c : columns) {
        picked.push_back(c >= 0 && c < static_cast<int>(row.size()) ? row[c]
                                                                    : vm::Value::null());
    }
    return keyFromValues(picked);
}

void Index::add(const vm::Value& key, const vm::RecordID& rid) {
    tree.insert(key, rid);
    bloom.add(keyBytes(key));
}

bool Index::remove(const vm::Value& key, const vm::RecordID& rid) {
    return tree.erase(key, rid);
}

std::vector<vm::RecordID> Index::lookup(const vm::Value& key) const {
    if (!bloom.mightContain(keyBytes(key))) {
        return {};
    }
    return tree.lookup(key);
}

Index* IndexManager::create(const std::string& name, int tableId,
                            std::vector<int> columns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (byName_.count(name) != 0) {
        return nullptr;
    }
    auto idx = std::make_unique<Index>(name, tableId, std::move(columns));
    Index* raw = idx.get();
    byName_.emplace(name, std::move(idx));
    return raw;
}

bool IndexManager::exists(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return byName_.count(name) != 0;
}

bool IndexManager::drop(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return byName_.erase(name) != 0;
}

std::vector<Index*> IndexManager::forTable(int tableId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Index*> result;
    for (const auto& [name, idx] : byName_) {
        if (idx->tableId == tableId) {
            result.push_back(idx.get());
        }
    }
    return result;
}

Index* IndexManager::find(int tableId, int columnIndex) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, idx] : byName_) {
        if (idx->tableId == tableId && idx->columns.size() == 1 &&
            idx->columnIndex == columnIndex) {
            return idx.get();
        }
    }
    return nullptr;
}

void IndexManager::dropTable(int tableId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = byName_.begin(); it != byName_.end();) {
        if (it->second->tableId == tableId) {
            it = byName_.erase(it);
        } else {
            ++it;
        }
    }
}

}
