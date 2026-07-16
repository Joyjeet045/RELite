#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "frontend/ast.hpp"

namespace db::semantic {

struct ColumnSchema {
    std::string name;
    parser::DataType type = parser::DataType::Int;
    int varcharLength = 0;

    bool notNull = false;
    bool primaryKey = false;
    bool unique = false;
    bool autoIncrement = false;
    bool hasDefault = false;
    parser::CachedValue defaultValue;

    std::shared_ptr<parser::Expression> checkExpr;
    std::string checkSource;
};

struct ForeignKey {
    enum class Action { Restrict, Cascade, SetNull };
    int columnIndex = -1;
    std::string refTable;
    std::string refColumn;
    Action onDelete = Action::Restrict;
    Action onUpdate = Action::Restrict;
};

struct TableSchema {
    int tableId = -1;
    std::string name;
    std::vector<ColumnSchema> columns;
    std::vector<ForeignKey> foreignKeys;
    bool isView = false;
    std::shared_ptr<parser::SelectStatement> viewQuery;
    std::string viewSource;

    int columnIndex(const std::string& column) const;
};

class Catalog {
public:
    Catalog() = default;

    static Catalog& instance();

    bool createTable(const std::string& name,
                     const std::vector<ColumnSchema>& columns,
                     int& outTableId);
    bool createView(const std::string& name,
                    const std::vector<ColumnSchema>& columns,
                    std::shared_ptr<parser::SelectStatement> query,
                    int& outTableId, const std::string& source = "");

    bool hasTable(const std::string& name) const;
    const TableSchema* getTable(const std::string& name) const;
    const TableSchema* getTableById(int tableId) const;
    std::vector<const TableSchema*> allTables() const;

    bool dropTable(const std::string& name);

    bool addColumn(const std::string& table, const ColumnSchema& column);
    bool dropColumn(const std::string& table, const std::string& column);
    bool addForeignKey(const std::string& table, int columnIndex,
                       const std::string& refTable, const std::string& refColumn,
                       ForeignKey::Action onDelete = ForeignKey::Action::Restrict,
                       ForeignKey::Action onUpdate = ForeignKey::Action::Restrict);

    bool createIndex(const std::string& indexName, const std::string& table,
                     const std::vector<std::string>& columns);
    bool hasIndex(const std::string& indexName) const;
    bool dropIndex(const std::string& indexName);

    void reset();

    struct IndexRef {
        std::string name;
        std::string table;
        std::vector<std::string> columns;
    };
    std::vector<IndexRef> allIndexes() const;

    int nextTableId() const { return nextTableId_; }
    void setNextTableId(int value) { nextTableId_ = value; }

    void restoreTable(const TableSchema& schema);

    bool setColumnCheckExpr(const std::string& table, int columnIndex,
                            std::shared_ptr<parser::Expression> expr);

private:
    int nextTableId_ = 0;
    std::unordered_map<std::string, TableSchema> tables_;
    std::unordered_map<int, std::string> tableNamesById_;
    std::unordered_map<std::string, std::pair<std::string, std::vector<std::string>>> indexes_;
};

}
