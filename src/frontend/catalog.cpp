#include "frontend/catalog.hpp"

#include <utility>

namespace db::semantic {

int TableSchema::columnIndex(const std::string& column) const {
    for (int i = 0; i < static_cast<int>(columns.size()); ++i) {
        if (columns[i].name == column) {
            return i;
        }
    }
    return -1;
}

Catalog& Catalog::instance() {
    static Catalog inst;
    return inst;
}

bool Catalog::createTable(const std::string& name,
                          const std::vector<ColumnSchema>& columns,
                          int& outTableId) {
    if (tables_.count(name) != 0) {
        return false;
    }
    TableSchema schema;
    schema.tableId = nextTableId_++;
    schema.name = name;
    schema.columns = columns;
    outTableId = schema.tableId;
    tableNamesById_[schema.tableId] = name;
    tables_.emplace(name, std::move(schema));
    return true;
}

bool Catalog::hasTable(const std::string& name) const {
    return tables_.count(name) != 0;
}

const TableSchema* Catalog::getTable(const std::string& name) const {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : &it->second;
}

const TableSchema* Catalog::getTableById(int tableId) const {
    auto it = tableNamesById_.find(tableId);
    if (it == tableNamesById_.end()) {
        return nullptr;
    }
    return getTable(it->second);
}

std::vector<const TableSchema*> Catalog::allTables() const {
    std::vector<const TableSchema*> out;
    out.reserve(tables_.size());
    for (const auto& [name, schema] : tables_) {
        out.push_back(&schema);
    }
    return out;
}

bool Catalog::dropTable(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) {
        return false;
    }
    tableNamesById_.erase(it->second.tableId);
    tables_.erase(it);
    for (auto i = indexes_.begin(); i != indexes_.end();) {
        if (i->second.first == name) {
            i = indexes_.erase(i);
        } else {
            ++i;
        }
    }
    return true;
}

bool Catalog::createIndex(const std::string& indexName, const std::string& table,
                          const std::string& column) {
    if (indexes_.count(indexName) != 0) {
        return false;
    }
    const TableSchema* schema = getTable(table);
    if (schema == nullptr || schema->columnIndex(column) < 0) {
        return false;
    }
    indexes_.emplace(indexName, std::make_pair(table, column));
    return true;
}

bool Catalog::hasIndex(const std::string& indexName) const {
    return indexes_.count(indexName) != 0;
}

bool Catalog::dropIndex(const std::string& indexName) {
    return indexes_.erase(indexName) != 0;
}

bool Catalog::addColumn(const std::string& table, const ColumnSchema& column) {
    auto it = tables_.find(table);
    if (it == tables_.end()) {
        return false;
    }
    it->second.columns.push_back(column);
    return true;
}

bool Catalog::dropColumn(const std::string& table, const std::string& column) {
    auto it = tables_.find(table);
    if (it == tables_.end()) {
        return false;
    }
    TableSchema& schema = it->second;
    int idx = schema.columnIndex(column);
    if (idx < 0) {
        return false;
    }
    schema.columns.erase(schema.columns.begin() + idx);
    std::vector<ForeignKey> kept;
    for (ForeignKey fk : schema.foreignKeys) {
        if (fk.columnIndex == idx) continue;
        if (fk.columnIndex > idx) --fk.columnIndex;
        kept.push_back(fk);
    }
    schema.foreignKeys = std::move(kept);
    return true;
}

bool Catalog::addForeignKey(const std::string& table, int columnIndex,
                            const std::string& refTable, const std::string& refColumn) {
    auto it = tables_.find(table);
    if (it == tables_.end()) {
        return false;
    }
    it->second.foreignKeys.push_back(ForeignKey{columnIndex, refTable, refColumn});
    return true;
}

void Catalog::reset() {
    tables_.clear();
    tableNamesById_.clear();
    indexes_.clear();
    nextTableId_ = 0;
}

std::vector<Catalog::IndexRef> Catalog::allIndexes() const {
    std::vector<IndexRef> out;
    out.reserve(indexes_.size());
    for (const auto& [name, loc] : indexes_) {
        out.push_back(IndexRef{name, loc.first, loc.second});
    }
    return out;
}

void Catalog::restoreTable(const TableSchema& schema) {
    tableNamesById_[schema.tableId] = schema.name;
    tables_[schema.name] = schema;
    if (schema.tableId >= nextTableId_) {
        nextTableId_ = schema.tableId + 1;
    }
}

bool Catalog::setColumnCheckExpr(const std::string& table, int columnIndex,
                                 std::shared_ptr<parser::Expression> expr) {
    auto it = tables_.find(table);
    if (it == tables_.end()) {
        return false;
    }
    if (columnIndex < 0 || columnIndex >= static_cast<int>(it->second.columns.size())) {
        return false;
    }
    it->second.columns[columnIndex].checkExpr = std::move(expr);
    return true;
}

}
