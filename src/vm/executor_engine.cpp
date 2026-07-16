#include "vm/executor_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "vm/executor.hpp"
#include "vm/expression_eval.hpp"

namespace db::vm {

namespace {

std::string aggLabel(const parser::FunctionExpr& fn) {
    std::string arg = fn.star ? "*" : (fn.argument ? fn.argument->column : "");
    return fn.name + "(" + arg + ")";
}

parser::ComparisonOp flipOp(parser::ComparisonOp op) {
    using O = parser::ComparisonOp;
    switch (op) {
        case O::Lt: return O::Gt;
        case O::Leq: return O::Geq;
        case O::Gt: return O::Lt;
        case O::Geq: return O::Leq;
        default: return op;
    }
}

Value computeAggregate(const parser::FunctionExpr& fn,
                       const std::vector<const std::vector<Value>*>& rows) {
    if (fn.name == "COUNT" && fn.star && !fn.distinct) {
        return Value::makeInt(static_cast<std::int64_t>(rows.size()));
    }

    const int col = fn.argument ? fn.argument->columnIndex : -1;

    std::vector<const Value*> vals;
    std::unordered_set<std::string> seen;
    for (const auto* r : rows) {
        if (col < 0 || col >= static_cast<int>(r->size())) continue;
        const Value& v = (*r)[col];
        if (v.isNull()) continue;
        if (fn.distinct) {
            std::string key = std::to_string(static_cast<int>(v.type)) + ':' + v.toString();
            if (!seen.insert(key).second) continue;
        }
        vals.push_back(&v);
    }

    if (fn.name == "COUNT") {
        return Value::makeInt(static_cast<std::int64_t>(vals.size()));
    }

    if (fn.name == "SUM" || fn.name == "AVG") {
        std::int64_t isum = 0;
        double dsum = 0.0;
        std::int64_t n = 0;
        bool isFloat = false;
        for (const Value* v : vals) {
            if (v->type == ValueType::Double) {
                isFloat = true;
                dsum += v->doubleValue;
            } else {
                isum += v->intValue;
            }
            ++n;
        }
        if (n == 0) return Value::null();
        double total = dsum + static_cast<double>(isum);
        if (fn.name == "AVG") {
            return Value::makeDouble(total / static_cast<double>(n));
        }
        if (isFloat) return Value::makeDouble(total);
        return Value::makeInt(isum);
    }

    Value best = Value::null();
    for (const Value* v : vals) {
        if (best.isNull()) {
            best = *v;
            continue;
        }
        auto cmp = compareValues(*v, best);
        if (cmp.has_value()) {
            if (fn.name == "MIN" && *cmp < 0) best = *v;
            if (fn.name == "MAX" && *cmp > 0) best = *v;
        }
    }
    return best;
}

}

ExecutorEngine::ExecutorEngine(StorageEngine& storage, semantic::Catalog& catalog,
                               txn::TransactionManager* txnManager, int* currentTxn)
    : storage_(storage),
      catalog_(catalog),
      txnMgr_(txnManager),
      currentTxn_(currentTxn) {}

ResultSet ExecutorEngine::run(parser::ASTNode& statement) {
    result_ = ResultSet{};
    statement.accept(*this);
    return std::move(result_);
}

void ExecutorEngine::loadSchema(int tableId, Schema& schema,
                                std::vector<std::string>& names) const {
    const semantic::TableSchema* ts = catalog_.getTableById(tableId);
    if (ts == nullptr) return;
    for (const auto& col : ts->columns) {
        schema.push_back(col.type);
        names.push_back(col.name);
    }
}

std::vector<std::pair<RecordID, std::vector<Value>>> ExecutorEngine::gatherRows(
    int tableId, const Schema& schema, parser::Expression* where) {
    std::vector<std::pair<RecordID, std::vector<Value>>> rows;

    std::vector<RecordID> candidates;
    if (where != nullptr && indexCandidates(where, tableId, candidates)) {
        for (const RecordID& rid : candidates) {
            std::string bytes;
            if (!storage_.tables().getTuple(tableId, rid, bytes)) continue;
            Tuple t = Tuple::deserialize(bytes, schema);
            if (predicateTrue(*where, t)) {
                if (txnActive()) lockOrThrow(rid, /*exclusive=*/false);
                rows.emplace_back(rid, t.values());
            }
        }
        return rows;
    }

    std::unique_ptr<AbstractExecutor> exec =
        std::make_unique<SeqScanExecutor>(&storage_.tables(), tableId, schema);
    if (where != nullptr) {
        exec = std::make_unique<FilterExecutor>(std::move(exec), where);
    }
    exec->init();
    Tuple t;
    RecordID rid;
    while (exec->next(t, rid)) {
        if (txnActive()) lockOrThrow(rid, /*exclusive=*/false);
        rows.emplace_back(rid, t.values());
    }
    return rows;
}

bool ExecutorEngine::indexCandidates(parser::Expression* where, int tableId,
                                     std::vector<RecordID>& rids) {
    using namespace parser;
    if (where == nullptr) return false;

    if (auto* land = dynamic_cast<LogicalExpr*>(where)) {
        if (land->op == LogicalOp::And) {
            if (indexCandidates(land->left.get(), tableId, rids)) return true;
            if (indexCandidates(land->right.get(), tableId, rids)) return true;
        }
        return false;
    }

    const Tuple empty;

    if (auto* bin = dynamic_cast<BinaryExpr*>(where)) {
        auto* col = dynamic_cast<ColumnRef*>(bin->left.get());
        auto* lit = dynamic_cast<LiteralExpr*>(bin->right.get());
        ComparisonOp op = bin->op;
        if (col == nullptr || lit == nullptr) {
            col = dynamic_cast<ColumnRef*>(bin->right.get());
            lit = dynamic_cast<LiteralExpr*>(bin->left.get());
            op = flipOp(bin->op);
        }
        if (col == nullptr || lit == nullptr) return false;
        index::Index* idx = storage_.indexes().find(tableId, col->columnIndex);
        if (idx == nullptr) return false;
        Value key = evalExpression(*lit, empty);
        if (key.isNull()) return false;
        switch (op) {
            case ComparisonOp::Eq:
                rids = idx->lookup(key);
                return true;
            case ComparisonOp::Lt:
            case ComparisonOp::Leq:
                rids = idx->tree.rangeScan(nullptr, &key);
                return true;
            case ComparisonOp::Gt:
            case ComparisonOp::Geq:
                rids = idx->tree.rangeScan(&key, nullptr);
                return true;
            default:
                return false;
        }
    }

    if (auto* bt = dynamic_cast<BetweenExpr*>(where)) {
        if (bt->negated) return false;
        auto* col = dynamic_cast<ColumnRef*>(bt->value.get());
        auto* loLit = dynamic_cast<LiteralExpr*>(bt->lo.get());
        auto* hiLit = dynamic_cast<LiteralExpr*>(bt->hi.get());
        if (col == nullptr || loLit == nullptr || hiLit == nullptr) return false;
        index::Index* idx = storage_.indexes().find(tableId, col->columnIndex);
        if (idx == nullptr) return false;
        Value lo = evalExpression(*loLit, empty);
        Value hi = evalExpression(*hiLit, empty);
        if (lo.isNull() || hi.isNull()) return false;
        rids = idx->tree.rangeScan(&lo, &hi);
        return true;
    }

    return false;
}

namespace {

parser::CachedValue toCached(const Value& v) {
    parser::CachedValue cv;
    switch (v.type) {
        case ValueType::Int:
            cv.kind = parser::CachedValue::Kind::Int;
            cv.intValue = v.intValue;
            break;
        case ValueType::Bool:
            cv.kind = parser::CachedValue::Kind::Bool;
            cv.boolValue = v.boolValue;
            break;
        case ValueType::Text:
            cv.kind = parser::CachedValue::Kind::Text;
            cv.stringValue = v.textValue;
            break;
        case ValueType::Double:
            cv.kind = parser::CachedValue::Kind::Float;
            cv.doubleValue = v.doubleValue;
            break;
        case ValueType::Null:
            cv.kind = parser::CachedValue::Kind::Null;
            break;
    }
    return cv;
}

Schema schemaOf(const semantic::TableSchema& ts) {
    Schema s;
    for (const auto& c : ts.columns) s.push_back(c.type);
    return s;
}

Value fromCached(const parser::CachedValue& cv) {
    switch (cv.kind) {
        case parser::CachedValue::Kind::Int: return Value::makeInt(cv.intValue);
        case parser::CachedValue::Kind::Float: return Value::makeDouble(cv.doubleValue);
        case parser::CachedValue::Kind::Bool: return Value::makeBool(cv.boolValue);
        case parser::CachedValue::Kind::Text: return Value::makeText(cv.stringValue);
        case parser::CachedValue::Kind::Null: return Value::null();
    }
    return Value::null();
}

void dedupeRows(std::vector<std::vector<Value>>& rows) {
    std::unordered_set<std::string> seen;
    std::vector<std::vector<Value>> out;
    out.reserve(rows.size());
    for (auto& row : rows) {
        std::string key;
        for (const auto& v : row) {
            key += std::to_string(static_cast<int>(v.type)) + ':' + v.toString() + '\x1e';
        }
        if (seen.insert(key).second) {
            out.push_back(std::move(row));
        }
    }
    rows.swap(out);
}

}

void ExecutorEngine::materializeSubquery(parser::SubqueryExpr* sub) {
    if (sub->evaluated) return;
    ExecutorEngine inner(storage_, catalog_, txnMgr_, currentTxn_);
    ResultSet rs = inner.run(*sub->query);
    sub->results.clear();
    for (auto& row : rs.rows) {
        sub->results.push_back(row.empty() ? parser::CachedValue{} : toCached(row[0]));
    }
    sub->evaluated = true;
}

void ExecutorEngine::materializeSubqueries(parser::Expression* expr) {
    if (expr == nullptr) return;
    using namespace parser;
    if (auto* b = dynamic_cast<BinaryExpr*>(expr)) {
        materializeSubqueries(b->left.get());
        materializeSubqueries(b->right.get());
    } else if (auto* l = dynamic_cast<LogicalExpr*>(expr)) {
        materializeSubqueries(l->left.get());
        materializeSubqueries(l->right.get());
    } else if (auto* u = dynamic_cast<UnaryExpr*>(expr)) {
        materializeSubqueries(u->operand.get());
    } else if (auto* i = dynamic_cast<IsNullExpr*>(expr)) {
        materializeSubqueries(i->operand.get());
    } else if (auto* in = dynamic_cast<InExpr*>(expr)) {
        materializeSubqueries(in->value.get());
        for (auto& item : in->items) materializeSubqueries(item.get());
        if (in->subquery) materializeSubquery(in->subquery.get());
    } else if (auto* bt = dynamic_cast<BetweenExpr*>(expr)) {
        materializeSubqueries(bt->value.get());
        materializeSubqueries(bt->lo.get());
        materializeSubqueries(bt->hi.get());
    } else if (auto* lk = dynamic_cast<LikeExpr*>(expr)) {
        materializeSubqueries(lk->value.get());
        materializeSubqueries(lk->pattern.get());
    } else if (auto* sq = dynamic_cast<SubqueryExpr*>(expr)) {
        materializeSubquery(sq);
    }
}

bool ExecutorEngine::parentHasValue(const std::string& refTable,
                                    const std::string& refColumn, const Value& value) {
    const semantic::TableSchema* parent = catalog_.getTable(refTable);
    if (parent == nullptr) return false;
    int col = parent->columnIndex(refColumn);
    if (col < 0) return false;
    Schema pschema = schemaOf(*parent);

    index::Index* idx = storage_.indexes().find(parent->tableId, col);
    if (idx != nullptr) {
        for (const RecordID& rid : idx->lookup(value)) {
            std::string bytes;
            if (storage_.tables().getTuple(parent->tableId, rid, bytes)) {
                Tuple t = Tuple::deserialize(bytes, pschema);
                if (col < static_cast<int>(t.size())) {
                    auto c = compareValues(t.at(col), value);
                    if (c.has_value() && *c == 0) return true;
                }
            }
        }
        return false;
    }

    SeqScanExecutor scan(&storage_.tables(), parent->tableId, pschema);
    scan.init();
    Tuple t;
    RecordID rid;
    while (scan.next(t, rid)) {
        if (col < static_cast<int>(t.size())) {
            auto c = compareValues(t.at(col), value);
            if (c.has_value() && *c == 0) return true;
        }
    }
    return false;
}

void ExecutorEngine::checkForeignKeys(const semantic::TableSchema& schema,
                                      const std::vector<Value>& row) {
    for (const auto& fk : schema.foreignKeys) {
        if (fk.columnIndex >= static_cast<int>(row.size())) continue;
        const Value& v = row[fk.columnIndex];
        if (v.isNull()) continue;
        if (!parentHasValue(fk.refTable, fk.refColumn, v)) {
            throw std::runtime_error("foreign key violation: value not present in " +
                                     fk.refTable + "(" + fk.refColumn + ")");
        }
    }
}

void ExecutorEngine::checkDeleteRestrict(const semantic::TableSchema& schema,
                                         const std::vector<Value>& row) {
    for (const semantic::TableSchema* child : catalog_.allTables()) {
        for (const auto& fk : child->foreignKeys) {
            if (fk.refTable != schema.name) continue;
            int pcol = schema.columnIndex(fk.refColumn);
            if (pcol < 0 || pcol >= static_cast<int>(row.size())) continue;
            const Value& parentVal = row[pcol];
            if (parentVal.isNull()) continue;

            Schema cschema = schemaOf(*child);
            SeqScanExecutor scan(&storage_.tables(), child->tableId, cschema);
            scan.init();
            Tuple t;
            RecordID rid;
            while (scan.next(t, rid)) {
                if (fk.columnIndex < static_cast<int>(t.size())) {
                    const Value& cv = t.at(fk.columnIndex);
                    if (cv.isNull()) continue;
                    auto cmp = compareValues(cv, parentVal);
                    if (cmp.has_value() && *cmp == 0) {
                        throw std::runtime_error(
                            "foreign key violation: row is referenced by table '" +
                            child->name + "'");
                    }
                }
            }
        }
    }
}

void ExecutorEngine::lockOrThrow(const RecordID& rid, bool exclusive) {
    bool ok = exclusive ? txnMgr_->lockExclusive(*currentTxn_, rid)
                        : txnMgr_->lockShared(*currentTxn_, rid);
    if (!ok) {
        throw std::runtime_error("lock wait timeout (possible deadlock)");
    }
}

bool ExecutorEngine::valueExists(int tableId, int columnIndex, const Value& value,
                                 const semantic::TableSchema& schema,
                                 const RecordID* excludeRid) {
    Schema sch = schemaOf(schema);

    index::Index* idx = storage_.indexes().find(tableId, columnIndex);
    if (idx != nullptr) {
        for (const RecordID& rid : idx->lookup(value)) {
            if (excludeRid != nullptr && rid == *excludeRid) continue;
            std::string bytes;
            if (!storage_.tables().getTuple(tableId, rid, bytes)) continue;
            Tuple t = Tuple::deserialize(bytes, sch);
            if (columnIndex < static_cast<int>(t.size())) {
                auto c = compareValues(t.at(columnIndex), value);
                if (c.has_value() && *c == 0) return true;
            }
        }
        return false;
    }

    SeqScanExecutor scan(&storage_.tables(), tableId, sch);
    scan.init();
    Tuple t;
    RecordID rid;
    while (scan.next(t, rid)) {
        if (excludeRid != nullptr && rid == *excludeRid) continue;
        if (columnIndex < static_cast<int>(t.size())) {
            auto c = compareValues(t.at(columnIndex), value);
            if (c.has_value() && *c == 0) return true;
        }
    }
    return false;
}

void ExecutorEngine::enforceConstraints(const semantic::TableSchema& schema, int tableId,
                                        const std::vector<Value>& row,
                                        const RecordID* excludeRid) {
    Tuple rowTuple(row);
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
        const semantic::ColumnSchema& col = schema.columns[i];
        const Value& v = (i < row.size()) ? row[i] : Value::null();

        if (col.notNull && v.isNull()) {
            throw std::runtime_error("NOT NULL constraint failed: " + schema.name +
                                     "." + col.name);
        }
        if (col.type == parser::DataType::Varchar && col.varcharLength > 0 &&
            v.type == ValueType::Text &&
            static_cast<int>(v.textValue.size()) > col.varcharLength) {
            throw std::runtime_error("value too long for column '" + col.name +
                                     "' (max " + std::to_string(col.varcharLength) +
                                     ")");
        }
        if (col.checkExpr && !predicateTrue(*col.checkExpr, rowTuple)) {
            throw std::runtime_error("CHECK constraint failed: " + schema.name + "." +
                                     col.name);
        }
        if ((col.primaryKey || col.unique) && !v.isNull()) {
            if (valueExists(tableId, static_cast<int>(i), v, schema, excludeRid)) {
                throw std::runtime_error("UNIQUE constraint failed: " + schema.name +
                                         "." + col.name);
            }
        }
    }
}

void ExecutorEngine::visit(parser::CreateStatement& node) {
    storage_.tables().registerTable(node.tableId);

    const semantic::TableSchema* ts = catalog_.getTableById(node.tableId);
    if (ts != nullptr) {
        for (std::size_t i = 0; i < ts->columns.size(); ++i) {
            const auto& col = ts->columns[i];
            if (!(col.primaryKey || col.unique)) continue;
            std::string idxName = "__uq_" + ts->name + "_" + col.name;
            if (!catalog_.hasIndex(idxName)) {
                catalog_.createIndex(idxName, ts->name, col.name);
                storage_.indexes().create(idxName, node.tableId, static_cast<int>(i));
            }
        }
    }
    result_.message = "BUILD RELATION";
}

void ExecutorEngine::visit(parser::CreateIdxStatement& node) {
    index::Index* idx =
        storage_.indexes().create(node.indexName, node.tableId, node.columnIndex);
    if (idx != nullptr) {
        Schema schema;
        std::vector<std::string> names;
        loadSchema(node.tableId, schema, names);
        SeqScanExecutor scan(&storage_.tables(), node.tableId, schema);
        scan.init();
        Tuple t;
        RecordID rid;
        while (scan.next(t, rid)) {
            if (node.columnIndex < static_cast<int>(t.size())) {
                idx->add(t.at(node.columnIndex), rid);
            }
        }
    }
    result_.message = "BUILD INDEX";
}

void ExecutorEngine::visit(parser::InsertStatement& node) {
    Schema schema;
    std::vector<std::string> names;
    loadSchema(node.tableId, schema, names);
    const std::size_t ncols = schema.size();

    const semantic::TableSchema* ts = catalog_.getTableById(node.tableId);
    std::vector<int> targets;
    if (node.columns.empty()) {
        for (int i = 0; i < static_cast<int>(ncols); ++i) targets.push_back(i);
    } else {
        for (const auto& name : node.columns) {
            targets.push_back(ts->columnIndex(name));
        }
    }

    std::vector<index::Index*> tableIndexes = storage_.indexes().forTable(node.tableId);
    const Tuple empty;
    std::vector<std::vector<Value>> providedRows;
    if (node.select) {
        ResultSet sel = run(*node.select);
        providedRows = std::move(sel.rows);
    } else {
        for (auto& row : node.rows) {
            std::vector<Value> pv;
            pv.reserve(row.size());
            for (auto& e : row) pv.push_back(evalExpression(*e, empty));
            providedRows.push_back(std::move(pv));
        }
    }
    result_ = ResultSet{};
    int count = 0;
    for (auto& provided : providedRows) {
        std::vector<Value> vals(ncols, Value::null());
        std::vector<bool> isSet(ncols, false);
        for (std::size_t i = 0; i < provided.size() && i < targets.size(); ++i) {
            vals[targets[i]] = provided[i];
            isSet[targets[i]] = true;
        }
        if (ts != nullptr) {
            for (std::size_t c = 0; c < ncols; ++c) {
                if (!isSet[c] && ts->columns[c].hasDefault) {
                    vals[c] = fromCached(ts->columns[c].defaultValue);
                }
            }
        }
        Tuple tup(std::move(vals));
        if (ts != nullptr) {
            checkForeignKeys(*ts, tup.values());
            enforceConstraints(*ts, node.tableId, tup.values(), nullptr);
        }
        std::string bytes = tup.serialize(schema);
        RecordID rid = storage_.tables().insertTuple(node.tableId, bytes);
        for (index::Index* idx : tableIndexes) {
            if (idx->columnIndex < static_cast<int>(tup.size())) {
                idx->add(tup.at(idx->columnIndex), rid);
            }
        }
        if (txnActive()) {
            lockOrThrow(rid, /*exclusive=*/true);
            StorageEngine* se = &storage_;
            int tid = node.tableId;
            std::vector<std::pair<index::Index*, Value>> idxKeys;
            for (index::Index* idx : tableIndexes) {
                if (idx->columnIndex < static_cast<int>(tup.size())) {
                    idxKeys.emplace_back(idx, tup.at(idx->columnIndex));
                }
            }
            txnMgr_->logInsert(*currentTxn_, tid, rid, bytes);
            txnMgr_->registerUndo(*currentTxn_, [se, tid, rid, idxKeys] {
                for (const auto& [idx, key] : idxKeys) idx->remove(key, rid);
                se->tables().eraseTuple(tid, rid);
            });
        }
        ++count;
    }
    result_.message = "PUT 0 " + std::to_string(count);
}

void ExecutorEngine::explainSelect(parser::SelectStatement& node) {
    result_.isQuery = true;
    result_.columns = {"plan"};
    std::vector<std::string> lines;
    int depth = 0;
    auto emit = [&](const std::string& s) {
        lines.push_back(std::string(static_cast<std::size_t>(depth) * 2, ' ') + s);
    };

    if (node.hasLimit || node.offset > 0) {
        std::string s = "Limit";
        if (node.hasLimit) s += " " + std::to_string(node.limit);
        if (node.offset > 0) s += " offset " + std::to_string(node.offset);
        emit(s);
        ++depth;
    }
    if (node.distinct) { emit("Unique"); ++depth; }
    if (!node.orderBy.empty()) { emit("Sort"); ++depth; }
    if (!node.aggregates.empty() || !node.groupBy.empty()) {
        emit(node.groupBy.empty() ? "Aggregate" : "GroupAggregate");
        ++depth;
    }

    if (!node.joinTable.empty()) {
        const semantic::TableSchema* lt = catalog_.getTableById(node.tableId);
        const semantic::TableSchema* rt = catalog_.getTableById(node.joinTableId);
        const char* kind = "Inner";
        switch (node.joinType) {
            case parser::SelectStatement::JoinKind::Left: kind = "Left"; break;
            case parser::SelectStatement::JoinKind::Right: kind = "Right"; break;
            case parser::SelectStatement::JoinKind::Full: kind = "Full"; break;
            case parser::SelectStatement::JoinKind::Cross: kind = "Cross"; break;
            default: break;
        }
        bool equi = false;
        if (auto* bin = dynamic_cast<parser::BinaryExpr*>(node.joinOn.get())) {
            equi = bin->op == parser::ComparisonOp::Eq &&
                   dynamic_cast<parser::ColumnRef*>(bin->left.get()) != nullptr &&
                   dynamic_cast<parser::ColumnRef*>(bin->right.get()) != nullptr;
        }
        bool hash =
            node.joinType == parser::SelectStatement::JoinKind::Inner && equi;
        emit(std::string(kind) + (hash ? " Join (Hash)" : " Join (Nested Loop)"));
        ++depth;
        emit("Seq Scan on " + (lt ? lt->name : std::string("?")));
        emit("Seq Scan on " + (rt ? rt->name : std::string("?")));
    } else {
        if (node.where) { emit("Filter"); ++depth; }
        std::vector<RecordID> tmp;
        bool idx =
            node.where != nullptr && indexCandidates(node.where.get(), node.tableId, tmp);
        const semantic::TableSchema* t = catalog_.getTableById(node.tableId);
        emit((idx ? "Index Scan on " : "Seq Scan on ") +
             (t ? t->name : std::string("?")));
    }

    for (const auto& line : lines) {
        result_.rows.push_back(std::vector<Value>{Value::makeText(line)});
    }
}

void ExecutorEngine::visit(parser::SelectStatement& node) {
    if (node.explain) {
        explainSelect(node);
        return;
    }
    result_.isQuery = true;
    materializeSubqueries(node.where.get());
    materializeSubqueries(node.having.get());
    materializeSubqueries(node.joinOn.get());
    std::vector<std::string> names;
    std::vector<std::pair<RecordID, std::vector<Value>>> rows;

    if (!node.joinTable.empty()) {
        Schema lschema, rschema;
        std::vector<std::string> lnames, rnames;
        loadSchema(node.tableId, lschema, lnames);
        loadSchema(node.joinTableId, rschema, rnames);
        names = lnames;
        names.insert(names.end(), rnames.begin(), rnames.end());
        const int leftWidth = static_cast<int>(lnames.size());
        const int rightWidth = static_cast<int>(rnames.size());

        auto leftRows = gatherRows(node.tableId, lschema, nullptr);
        auto rightRows = gatherRows(node.joinTableId, rschema, nullptr);

        const std::vector<Value> nullRight(rightWidth, Value::null());

        if (node.joinType == parser::SelectStatement::JoinKind::Cross) {
            for (auto& lp : leftRows) {
                for (auto& rp : rightRows) {
                    std::vector<Value> combined = lp.second;
                    combined.insert(combined.end(), rp.second.begin(), rp.second.end());
                    Tuple ct(combined);
                    if (node.where && !predicateTrue(*node.where, ct)) continue;
                    rows.emplace_back(RecordID{}, std::move(combined));
                }
            }
        } else {
            int leftKey = -1;
            int rightKey = -1;
            if (auto* bin = dynamic_cast<parser::BinaryExpr*>(node.joinOn.get())) {
                if (bin->op == parser::ComparisonOp::Eq) {
                    auto* a = dynamic_cast<parser::ColumnRef*>(bin->left.get());
                    auto* b = dynamic_cast<parser::ColumnRef*>(bin->right.get());
                    if (a != nullptr && b != nullptr) {
                        int ai = a->columnIndex;
                        int bi = b->columnIndex;
                        if (ai < leftWidth && bi >= leftWidth) {
                            leftKey = ai;
                            rightKey = bi - leftWidth;
                        } else if (bi < leftWidth && ai >= leftWidth) {
                            leftKey = bi;
                            rightKey = ai - leftWidth;
                        }
                    }
                }
            }

            auto keyOf = [](const Value& v) {
                return std::to_string(static_cast<int>(v.type)) + ':' + v.toString();
            };

            const bool inner =
                node.joinType == parser::SelectStatement::JoinKind::Inner;
            if (inner && leftKey >= 0 && rightKey >= 0) {
                std::unordered_map<std::string, std::vector<const std::vector<Value>*>> ht;
                for (auto& rp : rightRows) {
                    const Value& kv = rp.second[rightKey];
                    if (kv.isNull()) continue;
                    ht[keyOf(kv)].push_back(&rp.second);
                }
                for (auto& lp : leftRows) {
                    const Value& kv = lp.second[leftKey];
                    if (kv.isNull()) continue;
                    auto it = ht.find(keyOf(kv));
                    if (it == ht.end()) continue;
                    for (const std::vector<Value>* rrow : it->second) {
                        std::vector<Value> combined = lp.second;
                        combined.insert(combined.end(), rrow->begin(), rrow->end());
                        Tuple ct(combined);
                        if (node.where && !predicateTrue(*node.where, ct)) continue;
                        rows.emplace_back(RecordID{}, std::move(combined));
                    }
                }
            } else {
                using JoinKind = parser::SelectStatement::JoinKind;
                const bool keepLeft =
                    node.joinType == JoinKind::Left || node.joinType == JoinKind::Full;
                const bool keepRight =
                    node.joinType == JoinKind::Right || node.joinType == JoinKind::Full;
                std::vector<char> rightMatched(rightRows.size(), 0);
                for (auto& lp : leftRows) {
                    bool matched = false;
                    for (std::size_t ri = 0; ri < rightRows.size(); ++ri) {
                        std::vector<Value> combined = lp.second;
                        combined.insert(combined.end(), rightRows[ri].second.begin(),
                                        rightRows[ri].second.end());
                        Tuple ct(combined);
                        if (node.joinOn && !predicateTrue(*node.joinOn, ct)) continue;
                        matched = true;
                        rightMatched[ri] = 1;
                        if (node.where && !predicateTrue(*node.where, ct)) continue;
                        rows.emplace_back(RecordID{}, std::move(combined));
                    }
                    if (!matched && keepLeft) {
                        std::vector<Value> combined = lp.second;
                        combined.insert(combined.end(), nullRight.begin(), nullRight.end());
                        Tuple ct(combined);
                        if (node.where && !predicateTrue(*node.where, ct)) continue;
                        rows.emplace_back(RecordID{}, std::move(combined));
                    }
                }
                if (keepRight) {
                    const std::vector<Value> nullLeft(leftWidth, Value::null());
                    for (std::size_t ri = 0; ri < rightRows.size(); ++ri) {
                        if (rightMatched[ri]) continue;
                        std::vector<Value> combined = nullLeft;
                        combined.insert(combined.end(), rightRows[ri].second.begin(),
                                        rightRows[ri].second.end());
                        Tuple ct(combined);
                        if (node.where && !predicateTrue(*node.where, ct)) continue;
                        rows.emplace_back(RecordID{}, std::move(combined));
                    }
                }
            }
        }
    } else {
        Schema schema;
        loadSchema(node.tableId, schema, names);

        if (!node.aggregates.empty() || !node.groupBy.empty()) {
            auto arows = gatherRows(node.tableId, schema, node.where.get());
            std::vector<int> gcols;
            for (const auto& g : node.groupBy) gcols.push_back(g->columnIndex);

            struct Group {
                std::vector<const std::vector<Value>*> rows;
            };
            std::vector<Group> groups;
            std::map<std::string, int> groupIndex;
            for (auto& [rid, row] : arows) {
                (void)rid;
                std::string key;
                for (int gc : gcols) {
                    const Value& v = row[gc];
                    key += std::to_string(static_cast<int>(v.type)) + ':' + v.toString() + '\x1e';
                }
                auto it = groupIndex.find(key);
                int gi;
                if (it == groupIndex.end()) {
                    gi = static_cast<int>(groups.size());
                    groupIndex[key] = gi;
                    groups.emplace_back();
                } else {
                    gi = it->second;
                }
                groups[gi].rows.push_back(&row);
            }
            if (gcols.empty() && groups.empty()) {
                groups.emplace_back();
            }

            for (const auto& col : node.columns)
                result_.columns.push_back(col->alias.empty() ? col->column : col->alias);
            for (const auto& fn : node.aggregates)
                result_.columns.push_back(fn->alias.empty() ? aggLabel(*fn) : fn->alias);

            const std::vector<Value> nullRow(schema.size(), Value::null());
            for (const auto& g : groups) {
                const std::vector<Value>& rep = g.rows.empty() ? nullRow : *g.rows.front();
                if (node.having) {
                    Tuple repTuple(rep);
                    if (!predicateTrue(*node.having, repTuple)) continue;
                }
                std::vector<Value> outRow;
                for (const auto& col : node.columns) {
                    if (col->computed) {
                        Tuple repTuple(rep);
                        outRow.push_back(evalExpression(*col->computed, repTuple));
                        continue;
                    }
                    int ci = col->columnIndex;
                    outRow.push_back((ci >= 0 && ci < static_cast<int>(rep.size()))
                                         ? rep[ci]
                                         : Value::null());
                }
                for (const auto& fn : node.aggregates) {
                    outRow.push_back(computeAggregate(*fn, g.rows));
                }
                result_.rows.push_back(std::move(outRow));
            }
            if (node.offset > 0) {
                std::size_t off = static_cast<std::size_t>(node.offset);
                if (off >= result_.rows.size()) result_.rows.clear();
                else result_.rows.erase(result_.rows.begin(), result_.rows.begin() + off);
            }
            if (node.hasLimit && node.limit >= 0 &&
                static_cast<std::size_t>(node.limit) < result_.rows.size()) {
                result_.rows.resize(static_cast<std::size_t>(node.limit));
            }
            return;
        }

        rows = gatherRows(node.tableId, schema, node.where.get());
    }

    if (!node.orderBy.empty()) {
        std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
            for (const auto& key : node.orderBy) {
                int ci = key.column->columnIndex;
                const Value& va = a.second[ci];
                const Value& vb = b.second[ci];
                if (valueLess(va, vb)) return key.ascending;
                if (valueLess(vb, va)) return !key.ascending;
            }
            return false;
        });
    }

    if (node.selectStar) {
        result_.columns = names;
        for (auto& pr : rows) {
            result_.rows.push_back(pr.second);
        }
    } else {
        for (const auto& col : node.columns) {
            result_.columns.push_back(col->alias.empty() ? col->column : col->alias);
        }
        for (auto& pr : rows) {
            const auto& src = pr.second;
            std::vector<Value> projected;
            projected.reserve(node.columns.size());
            for (const auto& col : node.columns) {
                if (col->computed) {
                    Tuple ct(src);
                    projected.push_back(evalExpression(*col->computed, ct));
                    continue;
                }
                int idx = col->columnIndex;
                projected.push_back((idx >= 0 && idx < static_cast<int>(src.size()))
                                        ? src[idx]
                                        : Value::null());
            }
            result_.rows.push_back(std::move(projected));
        }
    }

    if (node.distinct) {
        dedupeRows(result_.rows);
    }
    if (node.offset > 0) {
        std::size_t off = static_cast<std::size_t>(node.offset);
        if (off >= result_.rows.size()) result_.rows.clear();
        else result_.rows.erase(result_.rows.begin(), result_.rows.begin() + off);
    }
    if (node.hasLimit) {
        std::size_t lim = node.limit < 0 ? 0 : static_cast<std::size_t>(node.limit);
        if (result_.rows.size() > lim) {
            result_.rows.resize(lim);
        }
    }
}

void ExecutorEngine::visit(parser::DeleteStatement& node) {
    Schema schema;
    std::vector<std::string> names;
    loadSchema(node.tableId, schema, names);
    materializeSubqueries(node.where.get());
    const semantic::TableSchema* ts = catalog_.getTableById(node.tableId);

    std::unique_ptr<AbstractExecutor> exec =
        std::make_unique<SeqScanExecutor>(&storage_.tables(), node.tableId, schema);
    if (node.where) {
        exec = std::make_unique<FilterExecutor>(std::move(exec), node.where.get());
    }

    std::vector<index::Index*> tableIndexes = storage_.indexes().forTable(node.tableId);

    std::vector<std::pair<RecordID, Tuple>> victims;
    exec->init();
    Tuple t;
    RecordID rid;
    while (exec->next(t, rid)) {
        victims.emplace_back(rid, t);
    }

    if (ts != nullptr) {
        for (auto& [vrid, vtuple] : victims) {
            (void)vrid;
            checkDeleteRestrict(*ts, vtuple.values());
        }
    }

    for (auto& [vrid, vtuple] : victims) {
        if (txnActive()) lockOrThrow(vrid, /*exclusive=*/true);
        for (index::Index* idx : tableIndexes) {
            if (idx->columnIndex < static_cast<int>(vtuple.size())) {
                idx->remove(vtuple.at(idx->columnIndex), vrid);
            }
        }
        if (txnActive()) {
            StorageEngine* se = &storage_;
            int tid = node.tableId;
            std::string bytes = vtuple.serialize(schema);
            std::vector<std::pair<index::Index*, Value>> idxKeys;
            for (index::Index* idx : tableIndexes) {
                if (idx->columnIndex < static_cast<int>(vtuple.size())) {
                    idxKeys.emplace_back(idx, vtuple.at(idx->columnIndex));
                }
            }
            txnMgr_->logDelete(*currentTxn_, tid, vrid, bytes);
            txnMgr_->registerUndo(*currentTxn_, [se, tid, bytes, idxKeys] {
                RecordID nr = se->tables().insertTuple(tid, bytes);
                for (const auto& [idx, key] : idxKeys) idx->add(key, nr);
            });
        }
        storage_.tables().eraseTuple(node.tableId, vrid);
    }
    result_.message = "REMOVE " + std::to_string(victims.size());
}

void ExecutorEngine::visit(parser::UpdateStatement& node) {
    Schema schema;
    std::vector<std::string> names;
    loadSchema(node.tableId, schema, names);
    materializeSubqueries(node.where.get());
    const semantic::TableSchema* ts = catalog_.getTableById(node.tableId);

    std::vector<index::Index*> tableIndexes = storage_.indexes().forTable(node.tableId);
    auto rows = gatherRows(node.tableId, schema, node.where.get());

    int count = 0;
    for (auto& [rid, row] : rows) {
        if (txnActive()) lockOrThrow(rid, /*exclusive=*/true);
        Tuple oldTup(row);
        std::vector<Value> newVals = row;
        for (std::size_t i = 0; i < node.targetIndices.size(); ++i) {
            int ci = node.targetIndices[i];
            newVals[ci] = evalExpression(*node.values[i], oldTup);
        }
        Tuple newTup(newVals);
        if (ts != nullptr) {
            checkForeignKeys(*ts, newTup.values());
            enforceConstraints(*ts, node.tableId, newTup.values(), &rid);
        }
        std::string oldBytes = oldTup.serialize(schema);
        std::string newBytes = newTup.serialize(schema);

        for (index::Index* idx : tableIndexes) {
            if (idx->columnIndex < static_cast<int>(oldTup.size())) {
                idx->remove(oldTup.at(idx->columnIndex), rid);
            }
        }
        RecordID nr = storage_.tables().updateTuple(node.tableId, rid, newBytes);
        if (txnActive()) lockOrThrow(nr, /*exclusive=*/true);
        for (index::Index* idx : tableIndexes) {
            if (idx->columnIndex < static_cast<int>(newTup.size())) {
                idx->add(newTup.at(idx->columnIndex), nr);
            }
        }
        if (txnActive()) {
            StorageEngine* se = &storage_;
            int tid = node.tableId;
            txnMgr_->logDelete(*currentTxn_, tid, rid, oldBytes);
            txnMgr_->logInsert(*currentTxn_, tid, nr, newBytes);
            std::vector<std::pair<index::Index*, Value>> oldKeys, newKeys;
            for (index::Index* idx : tableIndexes) {
                if (idx->columnIndex < static_cast<int>(oldTup.size())) {
                    oldKeys.emplace_back(idx, oldTup.at(idx->columnIndex));
                    newKeys.emplace_back(idx, newTup.at(idx->columnIndex));
                }
            }
            txnMgr_->registerUndo(*currentTxn_, [se, tid, nr, oldBytes, oldKeys, newKeys] {
                for (const auto& [idx, key] : newKeys) idx->remove(key, nr);
                se->tables().eraseTuple(tid, nr);
                RecordID rr = se->tables().insertTuple(tid, oldBytes);
                for (const auto& [idx, key] : oldKeys) idx->add(key, rr);
            });
        }
        ++count;
    }
    result_.message = "MODIFY " + std::to_string(count);
}

void ExecutorEngine::visit(parser::DropStatement& node) {
    if (node.isIndex) {
        storage_.indexes().drop(node.name);
        result_.message = "DISCARD INDEX";
    } else {
        storage_.tables().dropTable(node.tableId);
        storage_.indexes().dropTable(node.tableId);
        result_.message = "DISCARD RELATION";
    }
}

void ExecutorEngine::visit(parser::AlterStatement& node) {
    Schema oldSchema;
    std::vector<std::string> oldNames;
    loadSchema(node.tableId, oldSchema, oldNames);
    auto rows = gatherRows(node.tableId, oldSchema, nullptr);

    Schema newSchema = oldSchema;
    int dropIdx = -1;
    if (node.kind == parser::AlterStatement::Kind::AddColumn) {
        newSchema.push_back(node.column.type);
    } else {
        for (int i = 0; i < static_cast<int>(oldNames.size()); ++i) {
            if (oldNames[i] == node.dropColumn) {
                dropIdx = i;
                break;
            }
        }
        if (dropIdx >= 0) newSchema.erase(newSchema.begin() + dropIdx);
    }

    storage_.indexes().dropTable(node.tableId);
    storage_.tables().dropTable(node.tableId);
    storage_.tables().registerTable(node.tableId);
    for (auto& [rid, vals] : rows) {
        (void)rid;
        std::vector<Value> nv = vals;
        if (node.kind == parser::AlterStatement::Kind::AddColumn) {
            nv.push_back(Value::null());
        } else if (dropIdx >= 0 && dropIdx < static_cast<int>(nv.size())) {
            nv.erase(nv.begin() + dropIdx);
        }
        Tuple tup(std::move(nv));
        storage_.tables().insertTuple(node.tableId, tup.serialize(newSchema));
    }

    if (node.kind == parser::AlterStatement::Kind::AddColumn) {
        catalog_.addColumn(node.table,
                           semantic::ColumnSchema{node.column.name, node.column.type,
                                                  node.column.varcharLength});
        if (!node.column.refTable.empty()) {
            catalog_.addForeignKey(node.table,
                                   static_cast<int>(newSchema.size()) - 1,
                                   node.column.refTable, node.column.refColumn);
        }
        result_.message = "RESHAPE RELATION (ADD COLUMN)";
    } else {
        catalog_.dropColumn(node.table, node.dropColumn);
        result_.message = "RESHAPE RELATION (DISCARD COLUMN)";
    }
}

void ExecutorEngine::visit(parser::TransactionStatement& node) {
    if (txnMgr_ == nullptr || currentTxn_ == nullptr) {
        result_.message = "WARNING: transactions unavailable";
        return;
    }
    switch (node.kind) {
        case parser::TransactionStatement::Kind::Begin:
            if (*currentTxn_ != 0) {
                result_.message = "WARNING: already in a transaction";
            } else {
                *currentTxn_ = txnMgr_->begin();
                result_.message = "START";
            }
            break;
        case parser::TransactionStatement::Kind::Commit:
            if (*currentTxn_ == 0) {
                result_.message = "WARNING: no transaction in progress";
            } else {
                txnMgr_->commit(*currentTxn_);
                *currentTxn_ = 0;
                result_.message = "SAVE";
            }
            break;
        case parser::TransactionStatement::Kind::Rollback:
            if (*currentTxn_ == 0) {
                result_.message = "WARNING: no transaction in progress";
            } else {
                txnMgr_->rollback(*currentTxn_);
                *currentTxn_ = 0;
                result_.message = "UNDO";
            }
            break;
    }
}

void ExecutorEngine::visit(parser::SetOpStatement& node) {
    ResultSet left = run(*node.left);
    ResultSet right = run(*node.right);
    if (left.columns.size() != right.columns.size()) {
        throw std::runtime_error("set operation requires matching column counts");
    }

    ResultSet out;
    out.isQuery = true;
    out.columns = left.columns;

    auto rowKey = [](const std::vector<Value>& row) {
        std::string k;
        for (const auto& v : row) {
            k += std::to_string(static_cast<int>(v.type)) + ':' + v.toString() + '\x1e';
        }
        return k;
    };

    if (node.op == parser::SetOpStatement::Op::Union) {
        out.rows = std::move(left.rows);
        for (auto& row : right.rows) out.rows.push_back(std::move(row));
        if (!node.all) dedupeRows(out.rows);
    } else {
        std::unordered_set<std::string> rightKeys;
        for (const auto& row : right.rows) rightKeys.insert(rowKey(row));
        std::unordered_set<std::string> emitted;
        const bool wantIntersect = node.op == parser::SetOpStatement::Op::Intersect;
        for (auto& row : left.rows) {
            std::string k = rowKey(row);
            bool inRight = rightKeys.count(k) != 0;
            if (inRight == wantIntersect && emitted.insert(k).second) {
                out.rows.push_back(std::move(row));
            }
        }
    }

    result_ = std::move(out);
}

}
