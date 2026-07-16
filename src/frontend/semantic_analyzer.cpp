#include "frontend/semantic_analyzer.hpp"

#include <string>
#include <unordered_set>
#include <vector>

namespace db::semantic {

using parser::DataType;
using parser::LiteralExpr;

namespace {

bool isStringType(DataType type) {
    return type == DataType::Text || type == DataType::Varchar;
}

bool isNumericType(DataType type) {
    return type == DataType::Int || type == DataType::Float;
}

bool comparable(DataType a, DataType b) {
    if (isStringType(a) && isStringType(b)) {
        return true;
    }
    if (isNumericType(a) && isNumericType(b)) {
        return true;
    }
    return a == b;
}

bool literalMatchesColumn(LiteralExpr::Kind k, DataType col) {
    switch (k) {
        case LiteralExpr::Kind::Integer: return col == DataType::Int || col == DataType::Float;
        case LiteralExpr::Kind::Float: return col == DataType::Float;
        case LiteralExpr::Kind::Boolean: return col == DataType::Bool;
        case LiteralExpr::Kind::String: return isStringType(col);
        case LiteralExpr::Kind::Null: return true;
    }
    return false;
}

bool defaultMatchesColumn(const parser::CachedValue& v, DataType col) {
    switch (v.kind) {
        case parser::CachedValue::Kind::Int: return col == DataType::Int || col == DataType::Float;
        case parser::CachedValue::Kind::Float: return col == DataType::Float;
        case parser::CachedValue::Kind::Bool: return col == DataType::Bool;
        case parser::CachedValue::Kind::Text: return isStringType(col);
        case parser::CachedValue::Kind::Null: return true;
    }
    return false;
}

std::string typeName(DataType type) {
    return std::string(parser::dataTypeName(type));
}

}

SemanticError::SemanticError(const std::string& message)
    : std::runtime_error(message) {}

SemanticAnalyzer::SemanticAnalyzer(Catalog& catalog) : catalog_(catalog) {}

void SemanticAnalyzer::analyze(parser::ASTNode& node) { node.accept(*this); }

void SemanticAnalyzer::bindExpression(parser::Expression& expr,
                                      const std::string& tableName) {
    const TableSchema* table = catalog_.getTable(tableName);
    if (table == nullptr) {
        throw SemanticError("unknown table '" + tableName + "' for CHECK");
    }
    const TableSchema* saved = currentTable_;
    currentTable_ = table;
    checkPredicate(expr);
    currentTable_ = saved;
}

void SemanticAnalyzer::visit(parser::LiteralExpr& node) {
    switch (node.kind) {
        case LiteralExpr::Kind::Integer: node.resolvedType = DataType::Int; break;
        case LiteralExpr::Kind::Float: node.resolvedType = DataType::Float; break;
        case LiteralExpr::Kind::String: node.resolvedType = DataType::Text; break;
        case LiteralExpr::Kind::Boolean: node.resolvedType = DataType::Bool; break;
        case LiteralExpr::Kind::Null: node.resolvedType = std::nullopt; break;
    }
}

void SemanticAnalyzer::visit(parser::ColumnRef& node) {
    if (node.computed) {
        node.computed->accept(*this);
        node.resolvedType = node.computed->resolvedType;
        node.columnIndex = -1;
        return;
    }
    if (joinMode_) {
        if (!node.table.empty()) {
            if (node.table == leftTable_->name ||
                (!leftAlias_.empty() && node.table == leftAlias_)) {
                int idx = leftTable_->columnIndex(node.column);
                if (idx < 0) throw SemanticError("unknown column '" + node.column +
                                                 "' in table '" + leftTable_->name + "'");
                node.columnIndex = idx;
                node.resolvedType = leftTable_->columns[idx].type;
                return;
            }
            if (node.table == rightTable_->name ||
                (!rightAlias_.empty() && node.table == rightAlias_)) {
                int idx = rightTable_->columnIndex(node.column);
                if (idx < 0) throw SemanticError("unknown column '" + node.column +
                                                 "' in table '" + rightTable_->name + "'");
                node.columnIndex = leftColumnCount_ + idx;
                node.resolvedType = rightTable_->columns[idx].type;
                return;
            }
            throw SemanticError("unknown table qualifier '" + node.table + "'");
        }
        int li = leftTable_->columnIndex(node.column);
        int ri = rightTable_->columnIndex(node.column);
        if (li >= 0 && ri >= 0) {
            throw SemanticError("ambiguous column '" + node.column + "'");
        }
        if (li >= 0) {
            node.columnIndex = li;
            node.resolvedType = leftTable_->columns[li].type;
            return;
        }
        if (ri >= 0) {
            node.columnIndex = leftColumnCount_ + ri;
            node.resolvedType = rightTable_->columns[ri].type;
            return;
        }
        throw SemanticError("unknown column '" + node.column + "'");
    }

    if (currentTable_ == nullptr) {
        throw SemanticError("column reference '" + node.column +
                            "' has no table in scope");
    }
    if (!node.table.empty() && node.table != currentTable_->name &&
        node.table != currentAlias_) {
        throw SemanticError("unknown table qualifier '" + node.table +
                            "' (expected '" + currentTable_->name + "')");
    }
    int idx = currentTable_->columnIndex(node.column);
    if (idx < 0) {
        throw SemanticError("unknown column '" + node.column + "' in table '" +
                            currentTable_->name + "'");
    }
    node.columnIndex = idx;
    node.resolvedType = currentTable_->columns[idx].type;
}

void SemanticAnalyzer::visit(parser::BinaryExpr& node) {
    node.left->accept(*this);
    node.right->accept(*this);
    if (node.left->resolvedType && node.right->resolvedType) {
        DataType lt = *node.left->resolvedType;
        DataType rt = *node.right->resolvedType;
        if (!comparable(lt, rt)) {
            throw SemanticError("type mismatch in comparison: " + typeName(lt) +
                                " vs " + typeName(rt));
        }
    }
    node.resolvedType = DataType::Bool;
}

void SemanticAnalyzer::visit(parser::ArithmeticExpr& node) {
    node.left->accept(*this);
    node.right->accept(*this);
    if (node.left->resolvedType && !isNumericType(*node.left->resolvedType)) {
        throw SemanticError("arithmetic operator requires numeric operands");
    }
    if (node.right->resolvedType && !isNumericType(*node.right->resolvedType)) {
        throw SemanticError("arithmetic operator requires numeric operands");
    }
    bool isFloat = (node.left->resolvedType == DataType::Float) ||
                   (node.right->resolvedType == DataType::Float);
    node.resolvedType = isFloat ? DataType::Float : DataType::Int;
}

void SemanticAnalyzer::visit(parser::LogicalExpr& node) {
    node.left->accept(*this);
    node.right->accept(*this);
    if (node.left->resolvedType != DataType::Bool ||
        node.right->resolvedType != DataType::Bool) {
        throw SemanticError("operands of AND/OR must be boolean expressions");
    }
    node.resolvedType = DataType::Bool;
}

void SemanticAnalyzer::visit(parser::UnaryExpr& node) {
    node.operand->accept(*this);
    if (node.operand->resolvedType != DataType::Bool) {
        throw SemanticError("operand of NOT must be a boolean expression");
    }
    node.resolvedType = DataType::Bool;
}

void SemanticAnalyzer::visit(parser::IsNullExpr& node) {
    node.operand->accept(*this);
    node.resolvedType = DataType::Bool;
}

void SemanticAnalyzer::visit(parser::InExpr& node) {
    node.value->accept(*this);
    if (node.subquery) {
        node.subquery->accept(*this);
        node.resolvedType = DataType::Bool;
        return;
    }
    for (auto& item : node.items) {
        item->accept(*this);
        if (node.value->resolvedType && item->resolvedType &&
            !comparable(*node.value->resolvedType, *item->resolvedType)) {
            throw SemanticError("type mismatch in IN list");
        }
    }
    node.resolvedType = DataType::Bool;
}

void SemanticAnalyzer::visit(parser::BetweenExpr& node) {
    node.value->accept(*this);
    node.lo->accept(*this);
    node.hi->accept(*this);
    node.resolvedType = DataType::Bool;
}

void SemanticAnalyzer::visit(parser::LikeExpr& node) {
    node.value->accept(*this);
    node.pattern->accept(*this);
    if (node.value->resolvedType && !isStringType(*node.value->resolvedType)) {
        throw SemanticError("LIKE requires a text operand");
    }
    node.resolvedType = DataType::Bool;
}

void SemanticAnalyzer::visit(parser::FunctionExpr& node) {
    const std::string& fn = node.name;
    if (fn != "COUNT" && fn != "SUM" && fn != "AVG" && fn != "MIN" && fn != "MAX") {
        throw SemanticError("unknown aggregate function '" + fn + "'");
    }
    if (node.star) {
        if (fn != "COUNT") {
            throw SemanticError(fn + "(*) is not allowed");
        }
        node.resolvedType = DataType::Int;
        return;
    }
    if (!node.argument) {
        throw SemanticError(fn + " requires an argument");
    }
    node.argument->accept(*this);
    DataType at = node.argument->resolvedType.value_or(DataType::Int);
    if ((fn == "SUM" || fn == "AVG") && !isNumericType(at)) {
        throw SemanticError(fn + " requires a numeric column");
    }
    if (fn == "AVG") {
        node.resolvedType = DataType::Float;
    } else if (fn == "SUM" || fn == "MIN" || fn == "MAX") {
        node.resolvedType = at;
    } else {
        node.resolvedType = DataType::Int;
    }
}

void SemanticAnalyzer::visit(parser::CallExpr& node) {
    for (auto& a : node.args) a->accept(*this);
    const std::string& fn = node.name;
    if (node.isCast) {
        if (node.args.size() != 1) throw SemanticError("CAST takes one expression");
        node.resolvedType = node.castType;
        return;
    }
    auto arg0 = node.args.empty() ? std::nullopt : node.args[0]->resolvedType;
    if (fn == "UPPER" || fn == "LOWER" || fn == "TRIM") {
        if (node.args.size() != 1) throw SemanticError(fn + " takes 1 argument");
        node.resolvedType = DataType::Text;
    } else if (fn == "LENGTH") {
        if (node.args.size() != 1) throw SemanticError("LENGTH takes 1 argument");
        node.resolvedType = DataType::Int;
    } else if (fn == "SUBSTR") {
        if (node.args.size() < 2 || node.args.size() > 3)
            throw SemanticError("SUBSTR takes 2 or 3 arguments");
        node.resolvedType = DataType::Text;
    } else if (fn == "ABS") {
        if (node.args.size() != 1) throw SemanticError("ABS takes 1 argument");
        node.resolvedType = (arg0 == DataType::Float) ? DataType::Float : DataType::Int;
    } else if (fn == "ROUND") {
        if (node.args.empty() || node.args.size() > 2)
            throw SemanticError("ROUND takes 1 or 2 arguments");
        node.resolvedType = DataType::Float;
    } else if (fn == "CEIL" || fn == "FLOOR") {
        if (node.args.size() != 1) throw SemanticError(fn + " takes 1 argument");
        node.resolvedType = DataType::Int;
    } else if (fn == "MOD") {
        if (node.args.size() != 2) throw SemanticError("MOD takes 2 arguments");
        node.resolvedType = DataType::Int;
    } else if (fn == "COALESCE") {
        if (node.args.empty()) throw SemanticError("COALESCE requires arguments");
        std::optional<DataType> t;
        for (auto& a : node.args)
            if (a->resolvedType) { t = a->resolvedType; break; }
        node.resolvedType = t;
    } else if (fn == "NULLIF") {
        if (node.args.size() != 2) throw SemanticError("NULLIF takes 2 arguments");
        node.resolvedType = arg0;
    } else if (fn == "COUNT" || fn == "SUM" || fn == "AVG" || fn == "MIN" ||
               fn == "MAX") {
        throw SemanticError("aggregate '" + fn + "' is only allowed in a FETCH list");
    } else {
        throw SemanticError("unknown function '" + fn + "'");
    }
}

void SemanticAnalyzer::visit(parser::CaseExpr& node) {
    if (node.branches.empty()) throw SemanticError("CASE requires a WHEN branch");
    std::optional<DataType> resultType;
    for (auto& br : node.branches) {
        br.when->accept(*this);
        if (br.when->resolvedType && br.when->resolvedType != DataType::Bool) {
            throw SemanticError("CASE WHEN condition must be a boolean expression");
        }
        br.then->accept(*this);
        if (!resultType && br.then->resolvedType) resultType = br.then->resolvedType;
    }
    if (node.elseExpr) {
        node.elseExpr->accept(*this);
        if (!resultType && node.elseExpr->resolvedType) {
            resultType = node.elseExpr->resolvedType;
        }
    }
    node.resolvedType = resultType;
}

void SemanticAnalyzer::visit(parser::SubqueryExpr& node) {
    const TableSchema* savedTable = currentTable_;
    const TableSchema* savedLeft = leftTable_;
    const TableSchema* savedRight = rightTable_;
    int savedLeftCount = leftColumnCount_;
    bool savedJoin = joinMode_;
    currentTable_ = nullptr;
    leftTable_ = nullptr;
    rightTable_ = nullptr;
    joinMode_ = false;

    node.query->accept(*this);

    currentTable_ = savedTable;
    leftTable_ = savedLeft;
    rightTable_ = savedRight;
    leftColumnCount_ = savedLeftCount;
    joinMode_ = savedJoin;

    node.resolvedType =
        (node.kind == parser::SubqueryExpr::Kind::Exists) ? std::optional<DataType>(DataType::Bool)
                                                          : std::nullopt;
}

void SemanticAnalyzer::visit(parser::CreateStatement& node) {
    if (catalog_.hasTable(node.table)) {
        throw SemanticError("table '" + node.table + "' already exists");
    }

    std::unordered_set<std::string> seen;
    std::vector<ColumnSchema> columns;
    columns.reserve(node.columns.size());
    bool sawPrimaryKey = false;
    for (const auto& def : node.columns) {
        if (!seen.insert(def.name).second) {
            throw SemanticError("duplicate column '" + def.name +
                                "' in table '" + node.table + "'");
        }
        if (def.type == DataType::Varchar && def.varcharLength <= 0) {
            throw SemanticError("VARCHAR length must be positive for column '" +
                                def.name + "'");
        }
        if (def.primaryKey) {
            if (sawPrimaryKey) {
                throw SemanticError("table '" + node.table +
                                    "' has more than one PRIMARY KEY");
            }
            sawPrimaryKey = true;
        }
        if (def.hasDefault && !defaultMatchesColumn(def.defaultValue, def.type)) {
            throw SemanticError("DEFAULT value type mismatch for column '" +
                                def.name + "'");
        }
        if ((def.notNull || def.primaryKey) && def.hasDefault &&
            def.defaultValue.kind == parser::CachedValue::Kind::Null) {
            throw SemanticError("column '" + def.name +
                                "' is NOT NULL but has a NULL default");
        }

        ColumnSchema cs;
        cs.name = def.name;
        cs.type = def.type;
        cs.varcharLength = def.varcharLength;
        cs.notNull = def.notNull || def.primaryKey;
        cs.primaryKey = def.primaryKey;
        cs.unique = def.unique || def.primaryKey;
        cs.hasDefault = def.hasDefault;
        cs.defaultValue = def.defaultValue;
        cs.checkExpr = def.checkExpr;
        if (def.checkExpr) {
            cs.checkSource = parser::expressionToString(*def.checkExpr);
        }
        columns.push_back(std::move(cs));
    }

    int id = -1;
    catalog_.createTable(node.table, columns, id);
    node.tableId = id;

    for (std::size_t i = 0; i < node.columns.size(); ++i) {
        const auto& def = node.columns[i];
        if (def.refTable.empty()) continue;
        const TableSchema* parent = catalog_.getTable(def.refTable);
        if (parent == nullptr) {
            throw SemanticError("foreign key references unknown table '" +
                                def.refTable + "'");
        }
        int pidx = parent->columnIndex(def.refColumn);
        if (pidx < 0) {
            throw SemanticError("foreign key references unknown column '" +
                                def.refColumn + "' in table '" + def.refTable + "'");
        }
        if (!comparable(def.type, parent->columns[pidx].type)) {
            throw SemanticError("foreign key type mismatch for column '" +
                                def.name + "'");
        }
        catalog_.addForeignKey(node.table, static_cast<int>(i), def.refTable,
                               def.refColumn);
    }

    for (const auto& def : node.columns) {
        if (def.checkExpr) {
            bindExpression(*def.checkExpr, node.table);
        }
    }
}

void SemanticAnalyzer::visit(parser::CreateIdxStatement& node) {
    const TableSchema* table = catalog_.getTable(node.table);
    if (table == nullptr) {
        throw SemanticError("unknown table '" + node.table + "'");
    }
    int idx = table->columnIndex(node.column);
    if (idx < 0) {
        throw SemanticError("unknown column '" + node.column + "' in table '" +
                            node.table + "'");
    }
    if (catalog_.hasIndex(node.indexName)) {
        throw SemanticError("index '" + node.indexName + "' already exists");
    }
    catalog_.createIndex(node.indexName, node.table, node.column);
    node.tableId = table->tableId;
    node.columnIndex = idx;
}

void SemanticAnalyzer::visit(parser::InsertStatement& node) {
    const TableSchema* table = catalog_.getTable(node.table);
    if (table == nullptr) {
        throw SemanticError("unknown table '" + node.table + "'");
    }
    currentTable_ = table;
    node.tableId = table->tableId;

    std::vector<int> targets;
    if (node.columns.empty()) {
        for (int i = 0; i < static_cast<int>(table->columns.size()); ++i) {
            targets.push_back(i);
        }
    } else {
        std::unordered_set<std::string> seen;
        for (const auto& name : node.columns) {
            if (!seen.insert(name).second) {
                throw SemanticError("duplicate column '" + name + "' in INSERT");
            }
            int idx = table->columnIndex(name);
            if (idx < 0) {
                throw SemanticError("unknown column '" + name + "' in table '" +
                                    node.table + "'");
            }
            targets.push_back(idx);
        }
    }

    if (node.select) {
        node.select->accept(*this);
        std::size_t selCols;
        if (node.select->selectStar) {
            const TableSchema* st = catalog_.getTable(node.select->table);
            selCols = st ? st->columns.size() : 0;
            if (!node.select->joinTable.empty()) {
                const TableSchema* jt = catalog_.getTable(node.select->joinTable);
                if (jt) selCols += jt->columns.size();
            }
        } else {
            selCols = node.select->columns.size() + node.select->aggregates.size();
        }
        if (selCols != targets.size()) {
            throw SemanticError("INSERT ... FETCH yields " + std::to_string(selCols) +
                                " column(s) but " + std::to_string(targets.size()) +
                                " target column(s)");
        }
        currentTable_ = nullptr;
        return;
    }

    for (auto& row : node.rows) {
        if (row.size() != targets.size()) {
            throw SemanticError("INSERT has " + std::to_string(row.size()) +
                                " value(s) but " + std::to_string(targets.size()) +
                                " target column(s)");
        }
        for (std::size_t i = 0; i < row.size(); ++i) {
            row[i]->accept(*this);
            auto* lit = dynamic_cast<LiteralExpr*>(row[i].get());
            if (lit == nullptr) {
                throw SemanticError("INSERT values must be literals");
            }
            const ColumnSchema& col = table->columns[targets[i]];
            if (!literalMatchesColumn(lit->kind, col.type)) {
                throw SemanticError("type mismatch for column '" + col.name +
                                    "': expected " + typeName(col.type));
            }
        }
    }

    currentTable_ = nullptr;
}

void SemanticAnalyzer::visit(parser::SelectStatement& node) {
    if (!node.joinTable.empty()) {
        const TableSchema* left = catalog_.getTable(node.table);
        if (left == nullptr) throw SemanticError("unknown table '" + node.table + "'");
        const TableSchema* right = catalog_.getTable(node.joinTable);
        if (right == nullptr) {
            throw SemanticError("unknown table '" + node.joinTable + "'");
        }
        if (!node.aggregates.empty()) {
            throw SemanticError("aggregates are not supported with JOIN");
        }
        node.tableId = left->tableId;
        node.joinTableId = right->tableId;
        joinMode_ = true;
        leftTable_ = left;
        rightTable_ = right;
        leftAlias_ = node.tableAlias;
        rightAlias_ = node.joinTableAlias;
        leftColumnCount_ = static_cast<int>(left->columns.size());
        if (!node.selectStar) {
            for (auto& col : node.columns) col->accept(*this);
        }
        for (auto& key : node.orderBy) key.column->accept(*this);
        if (node.joinOn) {
            node.joinOn->accept(*this);
            if (node.joinOn->resolvedType != DataType::Bool) {
                throw SemanticError("JOIN ON must be a boolean expression");
            }
        }
        if (node.where) {
            node.where->accept(*this);
            if (node.where->resolvedType != DataType::Bool) {
                throw SemanticError("WHERE clause must be a boolean expression");
            }
        }
        joinMode_ = false;
        leftTable_ = nullptr;
        rightTable_ = nullptr;
        leftAlias_.clear();
        rightAlias_.clear();
        return;
    }

    const TableSchema* table = catalog_.getTable(node.table);
    if (table == nullptr) {
        throw SemanticError("unknown table '" + node.table + "'");
    }
    currentTable_ = table;
    currentAlias_ = node.tableAlias;
    node.tableId = table->tableId;

    if (!node.selectStar) {
        for (auto& col : node.columns) {
            col->accept(*this);
        }
    }
    for (auto& fn : node.aggregates) {
        fn->accept(*this);
    }
    for (auto& g : node.groupBy) {
        g->accept(*this);
    }
    for (auto& key : node.orderBy) {
        key.column->accept(*this);
    }
    if (node.where) {
        checkPredicate(*node.where);
    }
    if (node.having) {
        node.having->accept(*this);
        if (node.having->resolvedType != DataType::Bool) {
            throw SemanticError("HAVING must be a boolean expression");
        }
    }

    currentTable_ = nullptr;
    currentAlias_.clear();
}

void SemanticAnalyzer::visit(parser::DeleteStatement& node) {
    const TableSchema* table = catalog_.getTable(node.table);
    if (table == nullptr) {
        throw SemanticError("unknown table '" + node.table + "'");
    }
    currentTable_ = table;
    node.tableId = table->tableId;

    if (node.where) {
        checkPredicate(*node.where);
    }

    currentTable_ = nullptr;
}

void SemanticAnalyzer::visit(parser::UpdateStatement& node) {
    const TableSchema* table = catalog_.getTable(node.table);
    if (table == nullptr) {
        throw SemanticError("unknown table '" + node.table + "'");
    }
    currentTable_ = table;
    node.tableId = table->tableId;

    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < node.targetColumns.size(); ++i) {
        const std::string& name = node.targetColumns[i];
        if (!seen.insert(name).second) {
            throw SemanticError("duplicate assignment to column '" + name + "'");
        }
        int idx = table->columnIndex(name);
        if (idx < 0) {
            throw SemanticError("unknown column '" + name + "' in table '" +
                                node.table + "'");
        }
        node.targetIndices.push_back(idx);
        node.values[i]->accept(*this);
        DataType col = table->columns[idx].type;
        if (node.values[i]->resolvedType &&
            !comparable(col, *node.values[i]->resolvedType)) {
            throw SemanticError("type mismatch for column '" + name + "'");
        }
    }
    if (node.where) {
        checkPredicate(*node.where);
    }
    currentTable_ = nullptr;
}

void SemanticAnalyzer::visit(parser::DropStatement& node) {
    if (node.isIndex) {
        if (!catalog_.hasIndex(node.name)) {
            throw SemanticError("unknown index '" + node.name + "'");
        }
        catalog_.dropIndex(node.name);
    } else {
        const TableSchema* t = catalog_.getTable(node.name);
        if (t == nullptr) {
            throw SemanticError("unknown table '" + node.name + "'");
        }
        node.tableId = t->tableId;
        catalog_.dropTable(node.name);
    }
}

void SemanticAnalyzer::visit(parser::TransactionStatement&) {}

void SemanticAnalyzer::visit(parser::SetOpStatement& node) {
    node.left->accept(*this);
    node.right->accept(*this);
}

void SemanticAnalyzer::visit(parser::AlterStatement& node) {
    const TableSchema* table = catalog_.getTable(node.table);
    if (table == nullptr) {
        throw SemanticError("unknown table '" + node.table + "'");
    }
    node.tableId = table->tableId;
    if (node.kind == parser::AlterStatement::Kind::AddColumn) {
        if (table->columnIndex(node.column.name) >= 0) {
            throw SemanticError("column '" + node.column.name + "' already exists");
        }
        if (node.column.type == DataType::Varchar && node.column.varcharLength <= 0) {
            throw SemanticError("VARCHAR length must be positive for column '" +
                                node.column.name + "'");
        }
        if (!node.column.refTable.empty()) {
            const TableSchema* parent = catalog_.getTable(node.column.refTable);
            if (parent == nullptr ||
                parent->columnIndex(node.column.refColumn) < 0) {
                throw SemanticError("foreign key references unknown table/column");
            }
        }
    } else {
        int idx = table->columnIndex(node.dropColumn);
        if (idx < 0) {
            throw SemanticError("unknown column '" + node.dropColumn + "'");
        }
        if (table->columns.size() <= 1) {
            throw SemanticError("cannot drop the only column of a table");
        }
    }
}

void SemanticAnalyzer::checkPredicate(parser::Expression& expr) {
    expr.accept(*this);
    if (expr.resolvedType != DataType::Bool) {
        throw SemanticError("WHERE clause must be a boolean expression");
    }
}

}
