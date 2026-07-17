#include "vm/executor_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "vm/executor.hpp"
#include "vm/expression_eval.hpp"
#include "vm/column_store.hpp"
#include "vm/optimizer.hpp"
#include "vm/rewrite.hpp"
#include "vm/vectorized.hpp"

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

using GroupRows = std::vector<const std::vector<Value>*>;

Value evalHavingValue(const parser::Expression& e, const GroupRows& rows,
                      const Tuple& rep) {
    if (auto* fn = dynamic_cast<const parser::FunctionExpr*>(&e)) {
        return computeAggregate(*fn, rows);
    }
    if (auto* ar = dynamic_cast<const parser::ArithmeticExpr*>(&e)) {
        Value l = evalHavingValue(*ar->left, rows, rep);
        Value r = evalHavingValue(*ar->right, rows, rep);
        bool ln = l.type == ValueType::Int || l.type == ValueType::Double;
        bool rn = r.type == ValueType::Int || r.type == ValueType::Double;
        if (!ln || !rn) return Value::null();
        if (l.type == ValueType::Double || r.type == ValueType::Double) {
            double a = l.type == ValueType::Double ? l.doubleValue
                                                   : static_cast<double>(l.intValue);
            double b = r.type == ValueType::Double ? r.doubleValue
                                                   : static_cast<double>(r.intValue);
            switch (ar->op) {
                case parser::ArithmeticOp::Add: return Value::makeDouble(a + b);
                case parser::ArithmeticOp::Sub: return Value::makeDouble(a - b);
                case parser::ArithmeticOp::Mul: return Value::makeDouble(a * b);
                case parser::ArithmeticOp::Div:
                    return b == 0.0 ? Value::null() : Value::makeDouble(a / b);
            }
        }
        std::int64_t a = l.intValue;
        std::int64_t b = r.intValue;
        switch (ar->op) {
            case parser::ArithmeticOp::Add: return Value::makeInt(a + b);
            case parser::ArithmeticOp::Sub: return Value::makeInt(a - b);
            case parser::ArithmeticOp::Mul: return Value::makeInt(a * b);
            case parser::ArithmeticOp::Div:
                return b == 0 ? Value::null() : Value::makeInt(a / b);
        }
    }
    return evalExpression(e, rep);
}

bool evalHavingBool(const parser::Expression& e, const GroupRows& rows,
                    const Tuple& rep) {
    if (auto* log = dynamic_cast<const parser::LogicalExpr*>(&e)) {
        bool l = evalHavingBool(*log->left, rows, rep);
        bool r = evalHavingBool(*log->right, rows, rep);
        return log->op == parser::LogicalOp::And ? (l && r) : (l || r);
    }
    if (auto* un = dynamic_cast<const parser::UnaryExpr*>(&e)) {
        return !evalHavingBool(*un->operand, rows, rep);
    }
    if (auto* bin = dynamic_cast<const parser::BinaryExpr*>(&e)) {
        Value l = evalHavingValue(*bin->left, rows, rep);
        Value r = evalHavingValue(*bin->right, rows, rep);
        auto cmp = compareValues(l, r);
        if (!cmp.has_value()) return false;
        switch (bin->op) {
            case parser::ComparisonOp::Eq: return *cmp == 0;
            case parser::ComparisonOp::Neq: return *cmp != 0;
            case parser::ComparisonOp::Lt: return *cmp < 0;
            case parser::ComparisonOp::Leq: return *cmp <= 0;
            case parser::ComparisonOp::Gt: return *cmp > 0;
            case parser::ComparisonOp::Geq: return *cmp >= 0;
        }
    }
    Value v = evalHavingValue(e, rows, rep);
    return v.type == ValueType::Bool && v.boolValue;
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

std::vector<std::pair<RecordID, std::vector<Value>>> ExecutorEngine::gatherBaseRows(
    int tableId, const Schema& schema, parser::Expression* where) {
    const semantic::TableSchema* ts = catalog_.getTableById(tableId);
    if (ts != nullptr && ts->isView && ts->viewQuery) {
        ResultSet saved = std::move(result_);
        ResultSet viewResult = run(*ts->viewQuery);
        result_ = std::move(saved);
        std::vector<std::pair<RecordID, std::vector<Value>>> rows;
        for (auto& row : viewResult.rows) {
            if (where != nullptr) {
                Tuple t(row);
                if (!predicateTrue(*where, t)) continue;
            }
            rows.emplace_back(RecordID{}, std::move(row));
        }
        return rows;
    }
    return gatherRows(tableId, schema, where);
}

std::vector<std::pair<RecordID, std::vector<Value>>> ExecutorEngine::sourceRows(
    parser::SelectStatement& node, const Schema& schema, parser::Expression* where) {
    if (!node.asOf) {
        if (txnActive()) {
            const semantic::TableSchema* ts = catalog_.getTableById(node.tableId);
            std::uint64_t snap = 0;
            if ((ts == nullptr || !ts->isView) &&
                storage_.versions().snapshotVersionOf(*currentTxn_, snap)) {
                std::vector<std::pair<RecordID, std::vector<Value>>> rows;
                for (std::string& bytes :
                     storage_.versions().snapshotForTxn(node.tableId, snap)) {
                    Tuple t = Tuple::deserialize(bytes, schema);
                    if (where != nullptr && !predicateTrue(*where, t)) continue;
                    rows.emplace_back(RecordID{}, t.values());
                }
                return rows;
            }
        }
        return gatherBaseRows(node.tableId, schema, where);
    }
    std::vector<std::pair<RecordID, std::vector<Value>>> rows;
    for (std::string& bytes : storage_.versions().snapshotAsOf(node.tableId,
                                                               node.asOfVersion)) {
        Tuple t = Tuple::deserialize(bytes, schema);
        if (where != nullptr && !predicateTrue(*where, t)) continue;
        rows.emplace_back(RecordID{}, t.values());
    }
    return rows;
}

std::vector<std::pair<RecordID, std::vector<Value>>> ExecutorEngine::gatherJoinInput(
    parser::SelectStatement& node, int tableId, const Schema& schema) {
    if (node.asOf) {
        std::vector<std::pair<RecordID, std::vector<Value>>> rows;
        for (std::string& bytes :
             storage_.versions().snapshotAsOf(tableId, node.asOfVersion)) {
            Tuple t = Tuple::deserialize(bytes, schema);
            rows.emplace_back(RecordID{}, t.values());
        }
        return rows;
    }
    if (txnActive()) {
        const semantic::TableSchema* ts = catalog_.getTableById(tableId);
        std::uint64_t snap = 0;
        if ((ts == nullptr || !ts->isView) &&
            storage_.versions().snapshotVersionOf(*currentTxn_, snap)) {
            std::vector<std::pair<RecordID, std::vector<Value>>> rows;
            for (std::string& bytes :
                 storage_.versions().snapshotForTxn(tableId, snap)) {
                Tuple t = Tuple::deserialize(bytes, schema);
                rows.emplace_back(RecordID{}, t.values());
            }
            return rows;
        }
    }
    return gatherBaseRows(tableId, schema, nullptr);
}

std::vector<std::pair<RecordID, std::vector<Value>>> ExecutorEngine::joinTwo(
    const std::vector<std::pair<RecordID, std::vector<Value>>>& leftRows,
    int leftWidth,
    const std::vector<std::pair<RecordID, std::vector<Value>>>& rightRows,
    int rightWidth, parser::SelectStatement::JoinKind kind, parser::Expression* on) {
    using JK = parser::SelectStatement::JoinKind;
    std::vector<std::pair<RecordID, std::vector<Value>>> rows;
    const std::vector<Value> nullRight(rightWidth, Value::null());

    if (kind == JK::Cross) {
        for (auto& lp : leftRows) {
            for (auto& rp : rightRows) {
                std::vector<Value> combined = lp.second;
                combined.insert(combined.end(), rp.second.begin(), rp.second.end());
                rows.emplace_back(RecordID{}, std::move(combined));
            }
        }
        return rows;
    }

    int leftKey = -1;
    int rightKey = -1;
    if (auto* bin = dynamic_cast<parser::BinaryExpr*>(on)) {
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

    if (kind == JK::Inner && leftKey >= 0 && rightKey >= 0) {
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
                rows.emplace_back(RecordID{}, std::move(combined));
            }
        }
        return rows;
    }

    const bool keepLeft = kind == JK::Left || kind == JK::Full;
    const bool keepRight = kind == JK::Right || kind == JK::Full;
    std::vector<char> rightMatched(rightRows.size(), 0);
    for (auto& lp : leftRows) {
        bool matched = false;
        for (std::size_t ri = 0; ri < rightRows.size(); ++ri) {
            std::vector<Value> combined = lp.second;
            combined.insert(combined.end(), rightRows[ri].second.begin(),
                            rightRows[ri].second.end());
            if (on != nullptr) {
                Tuple ct(combined);
                if (!predicateTrue(*on, ct)) continue;
            }
            matched = true;
            rightMatched[ri] = 1;
            rows.emplace_back(RecordID{}, std::move(combined));
        }
        if (!matched && keepLeft) {
            std::vector<Value> combined = lp.second;
            combined.insert(combined.end(), nullRight.begin(), nullRight.end());
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
            rows.emplace_back(RecordID{}, std::move(combined));
        }
    }
    return rows;
}

bool ExecutorEngine::indexCandidates(parser::Expression* where, int tableId,
                                     std::vector<RecordID>& rids) {
    using namespace parser;
    if (where == nullptr) return false;

    {
        std::unordered_map<int, Value> eqs;
        std::function<void(parser::Expression*)> collect = [&](parser::Expression* e) {
            if (auto* land = dynamic_cast<LogicalExpr*>(e)) {
                if (land->op == LogicalOp::And) {
                    collect(land->left.get());
                    collect(land->right.get());
                }
                return;
            }
            if (auto* bin = dynamic_cast<BinaryExpr*>(e)) {
                if (bin->op != ComparisonOp::Eq) return;
                auto* col = dynamic_cast<ColumnRef*>(bin->left.get());
                auto* lit = dynamic_cast<LiteralExpr*>(bin->right.get());
                if (col == nullptr || lit == nullptr) {
                    col = dynamic_cast<ColumnRef*>(bin->right.get());
                    lit = dynamic_cast<LiteralExpr*>(bin->left.get());
                }
                if (col == nullptr || lit == nullptr) return;
                const Tuple none;
                Value v = evalExpression(*lit, none);
                if (!v.isNull()) eqs[col->columnIndex] = v;
            }
        };
        collect(where);
        if (!eqs.empty()) {
            for (index::Index* idx : storage_.indexes().forTable(tableId)) {
                if (!idx->isComposite()) continue;
                std::vector<Value> keyVals;
                bool covered = true;
                for (int c : idx->columns) {
                    auto it = eqs.find(c);
                    if (it == eqs.end()) { covered = false; break; }
                    keyVals.push_back(it->second);
                }
                if (covered) {
                    rids = idx->lookup(idx->keyFromValues(keyVals));
                    return true;
                }
            }
        }
    }

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
        if (in->subquery && !in->subquery->correlated) {
            materializeSubquery(in->subquery.get());
        }
    } else if (auto* bt = dynamic_cast<BetweenExpr*>(expr)) {
        materializeSubqueries(bt->value.get());
        materializeSubqueries(bt->lo.get());
        materializeSubqueries(bt->hi.get());
    } else if (auto* lk = dynamic_cast<LikeExpr*>(expr)) {
        materializeSubqueries(lk->value.get());
        materializeSubqueries(lk->pattern.get());
    } else if (auto* sq = dynamic_cast<SubqueryExpr*>(expr)) {
        if (!sq->correlated) materializeSubquery(sq);
    }
}

bool ExecutorEngine::hasCorrelatedSubquery(parser::Expression* expr) const {
    if (expr == nullptr) return false;
    using namespace parser;
    if (auto* b = dynamic_cast<BinaryExpr*>(expr)) {
        return hasCorrelatedSubquery(b->left.get()) ||
               hasCorrelatedSubquery(b->right.get());
    } else if (auto* a = dynamic_cast<ArithmeticExpr*>(expr)) {
        return hasCorrelatedSubquery(a->left.get()) ||
               hasCorrelatedSubquery(a->right.get());
    } else if (auto* l = dynamic_cast<LogicalExpr*>(expr)) {
        return hasCorrelatedSubquery(l->left.get()) ||
               hasCorrelatedSubquery(l->right.get());
    } else if (auto* u = dynamic_cast<UnaryExpr*>(expr)) {
        return hasCorrelatedSubquery(u->operand.get());
    } else if (auto* i = dynamic_cast<IsNullExpr*>(expr)) {
        return hasCorrelatedSubquery(i->operand.get());
    } else if (auto* in = dynamic_cast<InExpr*>(expr)) {
        if (in->subquery && in->subquery->correlated) return true;
        if (hasCorrelatedSubquery(in->value.get())) return true;
        for (auto& item : in->items) {
            if (hasCorrelatedSubquery(item.get())) return true;
        }
        return false;
    } else if (auto* bt = dynamic_cast<BetweenExpr*>(expr)) {
        return hasCorrelatedSubquery(bt->value.get()) ||
               hasCorrelatedSubquery(bt->lo.get()) ||
               hasCorrelatedSubquery(bt->hi.get());
    } else if (auto* lk = dynamic_cast<LikeExpr*>(expr)) {
        return hasCorrelatedSubquery(lk->value.get()) ||
               hasCorrelatedSubquery(lk->pattern.get());
    } else if (auto* call = dynamic_cast<CallExpr*>(expr)) {
        for (auto& arg : call->args) {
            if (hasCorrelatedSubquery(arg.get())) return true;
        }
        return false;
    } else if (auto* cs = dynamic_cast<CaseExpr*>(expr)) {
        for (auto& br : cs->branches) {
            if (hasCorrelatedSubquery(br.when.get()) ||
                hasCorrelatedSubquery(br.then.get())) {
                return true;
            }
        }
        return hasCorrelatedSubquery(cs->elseExpr.get());
    } else if (auto* col = dynamic_cast<ColumnRef*>(expr)) {
        return hasCorrelatedSubquery(col->computed.get());
    } else if (auto* sq = dynamic_cast<SubqueryExpr*>(expr)) {
        return sq->correlated;
    }
    return false;
}

void ExecutorEngine::bindCorrelated(parser::Expression* expr,
                                    const std::vector<Value>& outerRow) {
    if (expr == nullptr) return;
    using namespace parser;
    if (auto* b = dynamic_cast<BinaryExpr*>(expr)) {
        bindCorrelated(b->left.get(), outerRow);
        bindCorrelated(b->right.get(), outerRow);
    } else if (auto* a = dynamic_cast<ArithmeticExpr*>(expr)) {
        bindCorrelated(a->left.get(), outerRow);
        bindCorrelated(a->right.get(), outerRow);
    } else if (auto* l = dynamic_cast<LogicalExpr*>(expr)) {
        bindCorrelated(l->left.get(), outerRow);
        bindCorrelated(l->right.get(), outerRow);
    } else if (auto* u = dynamic_cast<UnaryExpr*>(expr)) {
        bindCorrelated(u->operand.get(), outerRow);
    } else if (auto* i = dynamic_cast<IsNullExpr*>(expr)) {
        bindCorrelated(i->operand.get(), outerRow);
    } else if (auto* in = dynamic_cast<InExpr*>(expr)) {
        bindCorrelated(in->value.get(), outerRow);
        for (auto& item : in->items) bindCorrelated(item.get(), outerRow);
        if (in->subquery && in->subquery->correlated) {
            bindSubquery(in->subquery.get(), outerRow);
        }
    } else if (auto* bt = dynamic_cast<BetweenExpr*>(expr)) {
        bindCorrelated(bt->value.get(), outerRow);
        bindCorrelated(bt->lo.get(), outerRow);
        bindCorrelated(bt->hi.get(), outerRow);
    } else if (auto* lk = dynamic_cast<LikeExpr*>(expr)) {
        bindCorrelated(lk->value.get(), outerRow);
        bindCorrelated(lk->pattern.get(), outerRow);
    } else if (auto* call = dynamic_cast<CallExpr*>(expr)) {
        for (auto& arg : call->args) bindCorrelated(arg.get(), outerRow);
    } else if (auto* cs = dynamic_cast<CaseExpr*>(expr)) {
        for (auto& br : cs->branches) {
            bindCorrelated(br.when.get(), outerRow);
            bindCorrelated(br.then.get(), outerRow);
        }
        bindCorrelated(cs->elseExpr.get(), outerRow);
    } else if (auto* col = dynamic_cast<ColumnRef*>(expr)) {
        bindCorrelated(col->computed.get(), outerRow);
    } else if (auto* sq = dynamic_cast<SubqueryExpr*>(expr)) {
        if (sq->correlated) bindSubquery(sq, outerRow);
    }
}

void ExecutorEngine::bindSubquery(parser::SubqueryExpr* sub,
                                  const std::vector<Value>& outerRow) {
    for (parser::ColumnRef* ref : sub->outerRefs) {
        if (ref->columnIndex >= 0 &&
            ref->columnIndex < static_cast<int>(outerRow.size())) {
            ref->boundValue = toCached(outerRow[ref->columnIndex]);
            ref->bound = true;
        } else {
            ref->boundValue = parser::CachedValue{};
            ref->bound = true;
        }
    }
    sub->evaluated = false;
    materializeSubquery(sub);
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

void ExecutorEngine::applyReferentialActions(const semantic::TableSchema& parent,
                                             const std::vector<Value>& parentRow) {
    for (const semantic::TableSchema* child : catalog_.allTables()) {
        for (const auto& fk : child->foreignKeys) {
            if (fk.refTable != parent.name) continue;
            int pcol = parent.columnIndex(fk.refColumn);
            if (pcol < 0 || pcol >= static_cast<int>(parentRow.size())) continue;
            const Value& parentVal = parentRow[pcol];
            if (parentVal.isNull()) continue;

            Schema cschema = schemaOf(*child);
            std::vector<index::Index*> cindexes =
                storage_.indexes().forTable(child->tableId);

            std::vector<std::pair<RecordID, std::vector<Value>>> matches;
            SeqScanExecutor scan(&storage_.tables(), child->tableId, cschema);
            scan.init();
            Tuple t;
            RecordID rid;
            while (scan.next(t, rid)) {
                if (fk.columnIndex >= static_cast<int>(t.size())) continue;
                const Value& cv = t.at(fk.columnIndex);
                if (cv.isNull()) continue;
                auto cmp = compareValues(cv, parentVal);
                if (cmp.has_value() && *cmp == 0) matches.emplace_back(rid, t.values());
            }
            if (matches.empty()) continue;

            if (fk.onDelete == semantic::ForeignKey::Action::Restrict) {
                throw std::runtime_error(
                    "foreign key violation: row is referenced by table '" +
                    child->name + "'");
            }

            for (auto& [crid, crow] : matches) {
                Tuple ctuple(crow);
                if (fk.onDelete == semantic::ForeignKey::Action::Cascade) {
                    applyReferentialActions(*child, crow);
                    if (txnActive()) lockOrThrow(crid, true);
                    std::string bytes = ctuple.serialize(cschema);
                    std::vector<std::pair<index::Index*, Value>> idxKeys;
                    for (index::Index* idx : cindexes) {
                        if (idx->coversRow(ctuple.size())) {
                            Value key = idx->keyOf(ctuple.values());
                            idxKeys.emplace_back(idx, key);
                            idx->remove(key, crid);
                        }
                    }
                    if (txnActive()) {
                        StorageEngine* se = &storage_;
                        int tid = child->tableId;
                        txnMgr_->logDelete(*currentTxn_, tid, crid, bytes);
                        txnMgr_->registerUndo(*currentTxn_, [se, tid, bytes, idxKeys] {
                            RecordID nr = se->tables().insertTuple(tid, bytes);
                            for (const auto& [idx, key] : idxKeys) idx->add(key, nr);
                        });
                    }
                    storage_.tables().eraseTuple(child->tableId, crid);
                } else {
                    std::vector<Value> nv = crow;
                    nv[fk.columnIndex] = Value::null();
                    Tuple newTup(nv);
                    if (txnActive()) lockOrThrow(crid, true);
                    for (index::Index* idx : cindexes) {
                        if (idx->coversRow(ctuple.size())) {
                            idx->remove(idx->keyOf(ctuple.values()), crid);
                        }
                    }
                    std::string oldBytes = ctuple.serialize(cschema);
                    std::string newBytes = newTup.serialize(cschema);
                    RecordID nr =
                        storage_.tables().updateTuple(child->tableId, crid, newBytes);
                    if (txnActive()) lockOrThrow(nr, true);
                    for (index::Index* idx : cindexes) {
                        if (idx->coversRow(newTup.size())) {
                            idx->add(idx->keyOf(newTup.values()), nr);
                        }
                    }
                    if (txnActive()) {
                        StorageEngine* se = &storage_;
                        int tid = child->tableId;
                        txnMgr_->logDelete(*currentTxn_, tid, crid, oldBytes);
                        txnMgr_->logInsert(*currentTxn_, tid, nr, newBytes);
                        std::vector<std::pair<index::Index*, Value>> oldKeys, newKeys;
                        for (index::Index* idx : cindexes) {
                            if (idx->coversRow(ctuple.size())) {
                                oldKeys.emplace_back(idx, idx->keyOf(ctuple.values()));
                                newKeys.emplace_back(idx, idx->keyOf(newTup.values()));
                            }
                        }
                        txnMgr_->registerUndo(
                            *currentTxn_, [se, tid, nr, oldBytes, oldKeys, newKeys] {
                                for (const auto& [idx, key] : newKeys) idx->remove(key, nr);
                                se->tables().eraseTuple(tid, nr);
                                RecordID rr = se->tables().insertTuple(tid, oldBytes);
                                for (const auto& [idx, key] : oldKeys) idx->add(key, rr);
                            });
                    }
                }
            }
        }
    }
}

void ExecutorEngine::applyUpdateReferentialActions(
    const semantic::TableSchema& parent,
    const std::vector<Value>& oldParentRow,
    const std::vector<Value>& newParentRow) {
    for (const semantic::TableSchema* child : catalog_.allTables()) {
        for (const auto& fk : child->foreignKeys) {
            if (fk.refTable != parent.name) continue;
            int pcol = parent.columnIndex(fk.refColumn);
            if (pcol < 0 || pcol >= static_cast<int>(oldParentRow.size())) continue;
            const Value& oldVal = oldParentRow[pcol];
            Value newVal = (pcol < static_cast<int>(newParentRow.size()))
                               ? newParentRow[pcol]
                               : Value::null();
            if (oldVal.isNull()) continue;
            auto same = compareValues(oldVal, newVal);
            if (same.has_value() && *same == 0) continue;

            Schema cschema = schemaOf(*child);
            std::vector<index::Index*> cindexes =
                storage_.indexes().forTable(child->tableId);

            std::vector<std::pair<RecordID, std::vector<Value>>> matches;
            {
                SeqScanExecutor scan(&storage_.tables(), child->tableId, cschema);
                scan.init();
                Tuple t;
                RecordID rid;
                while (scan.next(t, rid)) {
                    if (fk.columnIndex >= static_cast<int>(t.size())) continue;
                    const Value& cv = t.at(fk.columnIndex);
                    if (cv.isNull()) continue;
                    auto cmp = compareValues(cv, oldVal);
                    if (cmp.has_value() && *cmp == 0) {
                        matches.emplace_back(rid, t.values());
                    }
                }
            }
            if (matches.empty()) continue;

            if (fk.onUpdate == semantic::ForeignKey::Action::Restrict) {
                throw std::runtime_error(
                    "foreign key violation: row is referenced by table '" +
                    child->name + "'");
            }

            for (auto& [crid, crow] : matches) {
                Tuple ctuple(crow);
                std::vector<Value> nv = crow;
                nv[fk.columnIndex] =
                    (fk.onUpdate == semantic::ForeignKey::Action::Cascade)
                        ? newVal
                        : Value::null();
                Tuple newTup(nv);

                if (fk.onUpdate == semantic::ForeignKey::Action::Cascade) {
                    applyUpdateReferentialActions(*child, crow, nv);
                }

                if (txnActive()) lockOrThrow(crid, /*exclusive=*/true);
                for (index::Index* idx : cindexes) {
                    if (idx->coversRow(ctuple.size())) {
                        idx->remove(idx->keyOf(ctuple.values()), crid);
                    }
                }
                std::string oldBytes = ctuple.serialize(cschema);
                std::string newBytes = newTup.serialize(cschema);
                RecordID nr =
                    storage_.tables().updateTuple(child->tableId, crid, newBytes);
                if (txnActive()) lockOrThrow(nr, /*exclusive=*/true);
                for (index::Index* idx : cindexes) {
                    if (idx->coversRow(newTup.size())) {
                        idx->add(idx->keyOf(newTup.values()), nr);
                    }
                }
                storage_.versions().stageDelete(child->tableId, crid);
                storage_.versions().stageInsert(child->tableId, nr, newBytes);
                if (txnActive()) {
                    StorageEngine* se = &storage_;
                    int tid = child->tableId;
                    txnMgr_->logDelete(*currentTxn_, tid, crid, oldBytes);
                    txnMgr_->logInsert(*currentTxn_, tid, nr, newBytes);
                    std::vector<std::pair<index::Index*, Value>> oldKeys, newKeys;
                    for (index::Index* idx : cindexes) {
                        if (idx->coversRow(ctuple.size())) {
                            oldKeys.emplace_back(idx, idx->keyOf(ctuple.values()));
                            newKeys.emplace_back(idx, idx->keyOf(newTup.values()));
                        }
                    }
                    txnMgr_->registerUndo(
                        *currentTxn_, [se, tid, nr, oldBytes, oldKeys, newKeys] {
                            for (const auto& [idx, key] : newKeys) idx->remove(key, nr);
                            se->tables().eraseTuple(tid, nr);
                            RecordID rr = se->tables().insertTuple(tid, oldBytes);
                            for (const auto& [idx, key] : oldKeys) idx->add(key, rr);
                        });
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

    if (schema.primaryKey.size() >= 2) {
        bool anyNull = false;
        for (int ci : schema.primaryKey) {
            if (ci >= static_cast<int>(row.size()) || row[ci].isNull()) {
                anyNull = true;
                break;
            }
        }
        if (!anyNull) {
            Schema sch = schemaOf(schema);
            SeqScanExecutor scan(&storage_.tables(), tableId, sch);
            scan.init();
            Tuple t;
            RecordID rid;
            while (scan.next(t, rid)) {
                if (excludeRid != nullptr && rid == *excludeRid) continue;
                bool allEq = true;
                for (int ci : schema.primaryKey) {
                    if (ci >= static_cast<int>(t.size())) {
                        allEq = false;
                        break;
                    }
                    auto c = compareValues(t.at(ci), row[ci]);
                    if (!c.has_value() || *c != 0) {
                        allEq = false;
                        break;
                    }
                }
                if (allEq) {
                    throw std::runtime_error("PRIMARY KEY constraint failed: " +
                                             schema.name);
                }
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
                catalog_.createIndex(idxName, ts->name, {col.name});
                storage_.indexes().create(idxName, node.tableId,
                                          {static_cast<int>(i)});
            }
        }
        if (ts->primaryKey.size() >= 2) {
            std::string idxName = "__pk_" + ts->name;
            std::vector<std::string> cols;
            std::vector<int> idxCols;
            for (int ci : ts->primaryKey) {
                cols.push_back(ts->columns[ci].name);
                idxCols.push_back(ci);
            }
            if (!catalog_.hasIndex(idxName)) {
                catalog_.createIndex(idxName, ts->name, cols);
                storage_.indexes().create(idxName, node.tableId, idxCols);
            }
        }
    }

    if (node.asQuery) {
        Schema schema;
        std::vector<std::string> names;
        loadSchema(node.tableId, schema, names);
        ResultSet rs = run(*node.asQuery);
        result_ = ResultSet{};
        storage_.columns().clear();
        std::vector<index::Index*> idxs = storage_.indexes().forTable(node.tableId);
        long long inserted = 0;
        for (auto& row : rs.rows) {
            Tuple tup(row);
            std::string bytes = tup.serialize(schema);
            RecordID rid = storage_.tables().insertTuple(node.tableId, bytes);
            for (index::Index* idx : idxs) {
                if (idx->coversRow(tup.size())) idx->add(idx->keyOf(tup.values()), rid);
            }
            storage_.versions().stageInsert(node.tableId, rid, bytes);
            ++inserted;
        }
        storage_.versions().commitPending();
        result_.message = "BUILD RELATION AS (" + std::to_string(inserted) + " rows)";
        return;
    }
    result_.message = "BUILD RELATION";
}

void ExecutorEngine::visit(parser::CreateIdxStatement& node) {
    index::Index* idx =
        storage_.indexes().create(node.indexName, node.tableId, node.columnIndices);
    if (idx != nullptr) {
        Schema schema;
        std::vector<std::string> names;
        loadSchema(node.tableId, schema, names);
        SeqScanExecutor scan(&storage_.tables(), node.tableId, schema);
        scan.init();
        Tuple t;
        RecordID rid;
        while (scan.next(t, rid)) {
            if (idx->coversRow(t.size())) {
                idx->add(idx->keyOf(t.values()), rid);
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
    storage_.columns().clear();
    if (!txnActive()) storage_.versions().discardPending();

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

    std::unordered_map<int, long long> autoNext;
    if (ts != nullptr) {
        for (std::size_t c = 0; c < ncols; ++c) {
            if (!ts->columns[c].autoIncrement) continue;
            long long maxVal = 0;
            SeqScanExecutor scan(&storage_.tables(), node.tableId, schema);
            scan.init();
            Tuple t;
            RecordID rid;
            while (scan.next(t, rid)) {
                const Value& v = t.at(static_cast<int>(c));
                if (v.type == ValueType::Int && v.intValue > maxVal) maxVal = v.intValue;
            }
            autoNext[static_cast<int>(c)] = maxVal + 1;
        }
    }

    int count = 0;
    const bool wantReturn = node.returningStar || !node.returning.empty();
    std::vector<std::vector<Value>> returned;

    std::vector<int> conflictIdx;
    for (auto& col : node.conflictColumns) conflictIdx.push_back(col->columnIndex);
    std::vector<int> setIdx;
    for (auto& nm : node.conflictSetColumns) {
        setIdx.push_back(ts != nullptr ? ts->columnIndex(nm) : -1);
    }
    auto findConflict = [&](const Tuple& cand, RecordID& outRid,
                            std::vector<Value>& outRow) -> bool {
        SeqScanExecutor scan(&storage_.tables(), node.tableId, schema);
        scan.init();
        Tuple t;
        RecordID rid;
        while (scan.next(t, rid)) {
            bool allEq = true;
            for (int ci : conflictIdx) {
                auto c = compareValues(t.at(ci), cand.at(ci));
                if (!c.has_value() || *c != 0) {
                    allEq = false;
                    break;
                }
            }
            if (allEq) {
                outRid = rid;
                outRow = t.values();
                return true;
            }
        }
        return false;
    };

    for (auto& provided : providedRows) {
        std::vector<Value> vals(ncols, Value::null());
        std::vector<bool> isSet(ncols, false);
        for (std::size_t i = 0; i < provided.size() && i < targets.size(); ++i) {
            vals[targets[i]] = provided[i];
            isSet[targets[i]] = true;
        }
        for (auto& [col, next] : autoNext) {
            if (!isSet[col] || vals[col].isNull()) {
                vals[col] = Value::makeInt(next);
                isSet[col] = true;
                ++next;
            }
        }
        if (ts != nullptr) {
            for (std::size_t c = 0; c < ncols; ++c) {
                if (!isSet[c] && ts->columns[c].hasDefault) {
                    vals[c] = fromCached(ts->columns[c].defaultValue);
                }
            }
        }
        Tuple tup(std::move(vals));
        if (node.hasOnConflict) {
            RecordID exRid;
            std::vector<Value> exRow;
            if (findConflict(tup, exRid, exRow)) {
                if (node.conflictDoNothing) continue;
                Tuple oldTup(exRow);
                std::vector<Value> newVals = exRow;
                for (std::size_t i = 0; i < setIdx.size(); ++i) {
                    newVals[setIdx[i]] =
                        evalExpression(*node.conflictSetValues[i], oldTup);
                }
                Tuple newTup(newVals);
                if (ts != nullptr) {
                    checkForeignKeys(*ts, newTup.values());
                    enforceConstraints(*ts, node.tableId, newTup.values(), &exRid);
                }
                if (txnActive()) lockOrThrow(exRid, /*exclusive=*/true);
                std::string oldBytes = oldTup.serialize(schema);
                std::string newBytes = newTup.serialize(schema);
                for (index::Index* idx : tableIndexes) {
                    if (idx->coversRow(oldTup.size())) {
                        idx->remove(idx->keyOf(oldTup.values()), exRid);
                    }
                }
                RecordID nr =
                    storage_.tables().updateTuple(node.tableId, exRid, newBytes);
                if (txnActive()) lockOrThrow(nr, /*exclusive=*/true);
                for (index::Index* idx : tableIndexes) {
                    if (idx->coversRow(newTup.size())) {
                        idx->add(idx->keyOf(newTup.values()), nr);
                    }
                }
                storage_.versions().stageDelete(node.tableId, exRid);
                storage_.versions().stageInsert(node.tableId, nr, newBytes);
                if (txnActive()) {
                    StorageEngine* se = &storage_;
                    int tid = node.tableId;
                    txnMgr_->logDelete(*currentTxn_, tid, exRid, oldBytes);
                    txnMgr_->logInsert(*currentTxn_, tid, nr, newBytes);
                    std::vector<std::pair<index::Index*, Value>> oldKeys, newKeys;
                    for (index::Index* idx : tableIndexes) {
                        if (idx->coversRow(oldTup.size())) {
                            oldKeys.emplace_back(idx, idx->keyOf(oldTup.values()));
                            newKeys.emplace_back(idx, idx->keyOf(newTup.values()));
                        }
                    }
                    txnMgr_->registerUndo(*currentTxn_,
                        [se, tid, nr, oldBytes, oldKeys, newKeys] {
                            for (const auto& [idx, key] : newKeys) idx->remove(key, nr);
                            se->tables().eraseTuple(tid, nr);
                            RecordID rr = se->tables().insertTuple(tid, oldBytes);
                            for (const auto& [idx, key] : oldKeys) idx->add(key, rr);
                        });
                }
                if (wantReturn) {
                    if (node.returningStar) {
                        returned.push_back(newTup.values());
                    } else {
                        std::vector<Value> proj;
                        proj.reserve(node.returning.size());
                        for (auto& col : node.returning) {
                            proj.push_back(newTup.at(col->columnIndex));
                        }
                        returned.push_back(std::move(proj));
                    }
                }
                ++count;
                continue;
            }
        }
        if (ts != nullptr) {
            checkForeignKeys(*ts, tup.values());
            enforceConstraints(*ts, node.tableId, tup.values(), nullptr);
        }
        std::string bytes = tup.serialize(schema);
        RecordID rid = storage_.tables().insertTuple(node.tableId, bytes);
        for (index::Index* idx : tableIndexes) {
            if (idx->coversRow(tup.size())) {
                idx->add(idx->keyOf(tup.values()), rid);
            }
        }
        storage_.versions().stageInsert(node.tableId, rid, bytes);
        if (txnActive()) {
            lockOrThrow(rid, /*exclusive=*/true);
            StorageEngine* se = &storage_;
            int tid = node.tableId;
            std::vector<std::pair<index::Index*, Value>> idxKeys;
            for (index::Index* idx : tableIndexes) {
                if (idx->coversRow(tup.size())) {
                    idxKeys.emplace_back(idx, idx->keyOf(tup.values()));
                }
            }
            txnMgr_->logInsert(*currentTxn_, tid, rid, bytes);
            txnMgr_->registerUndo(*currentTxn_, [se, tid, rid, idxKeys] {
                for (const auto& [idx, key] : idxKeys) idx->remove(key, rid);
                se->tables().eraseTuple(tid, rid);
            });
        }
        if (wantReturn) {
            if (node.returningStar) {
                returned.push_back(tup.values());
            } else {
                std::vector<Value> proj;
                proj.reserve(node.returning.size());
                for (auto& col : node.returning) {
                    proj.push_back(tup.at(col->columnIndex));
                }
                returned.push_back(std::move(proj));
            }
        }
        ++count;
    }
    if (!txnActive()) storage_.versions().commitPending();
    if (wantReturn) {
        result_.isQuery = true;
        if (node.returningStar) {
            result_.columns = names;
        } else {
            for (auto& col : node.returning) {
                result_.columns.push_back(names[col->columnIndex]);
            }
        }
        result_.rows = std::move(returned);
    } else {
        result_.message = "PUT 0 " + std::to_string(count);
    }
}

void ExecutorEngine::runWindowQuery(parser::SelectStatement& node) {
    Schema schema;
    std::vector<std::string> names;
    loadSchema(node.tableId, schema, names);
    auto rows = gatherBaseRows(node.tableId, schema, node.where.get());
    const int n = static_cast<int>(rows.size());

    auto valKey = [](const Value& v) {
        return std::to_string(static_cast<int>(v.type)) + ':' + v.toString();
    };

    std::vector<std::vector<Value>> winValues;
    for (auto& col : node.columns) {
        auto* w = col->computed
                      ? dynamic_cast<parser::WindowExpr*>(col->computed.get())
                      : nullptr;
        if (w == nullptr) {
            winValues.emplace_back();
            continue;
        }
        std::vector<Value> result(static_cast<std::size_t>(n));
        std::unordered_map<std::string, std::vector<int>> partitions;
        std::vector<std::string> partOrder;
        for (int i = 0; i < n; ++i) {
            std::string k;
            for (auto& pc : w->partitionBy) {
                k += valKey(rows[i].second[pc->columnIndex]) + '\x1e';
            }
            if (partitions.find(k) == partitions.end()) partOrder.push_back(k);
            partitions[k].push_back(i);
        }
        auto sameOrder = [&](int a, int b) {
            for (auto& ok : w->orderBy) {
                const Value& va = rows[a].second[ok.column->columnIndex];
                const Value& vb = rows[b].second[ok.column->columnIndex];
                if (valueLess(va, vb) || valueLess(vb, va)) return false;
            }
            return true;
        };
        for (auto& k : partOrder) {
            auto& idxs = partitions[k];
            if (!w->orderBy.empty()) {
                std::stable_sort(idxs.begin(), idxs.end(), [&](int a, int b) {
                    for (auto& ok : w->orderBy) {
                        const Value& va = rows[a].second[ok.column->columnIndex];
                        const Value& vb = rows[b].second[ok.column->columnIndex];
                        if (valueLess(va, vb)) return ok.ascending;
                        if (valueLess(vb, va)) return !ok.ascending;
                    }
                    return false;
                });
            }
            const std::string& fn = w->name;
            if (fn == "ROW_NUMBER") {
                for (std::size_t r = 0; r < idxs.size(); ++r) {
                    result[idxs[r]] = Value::makeInt(static_cast<long long>(r) + 1);
                }
            } else if (fn == "RANK" || fn == "DENSE_RANK") {
                long long rank = 0;
                long long dense = 0;
                for (std::size_t r = 0; r < idxs.size(); ++r) {
                    if (r == 0 || !sameOrder(idxs[r - 1], idxs[r])) {
                        dense++;
                        rank = static_cast<long long>(r) + 1;
                    }
                    result[idxs[r]] = Value::makeInt(fn == "RANK" ? rank : dense);
                }
            } else {
                int argCol = w->argument ? w->argument->columnIndex : -1;
                double dsum = 0.0;
                long long isum = 0;
                long long cnt = 0;
                bool isFloat = false;
                Value best = Value::null();
                for (int idx : idxs) {
                    if (argCol < 0) {
                        cnt++;
                        continue;
                    }
                    const Value& v = rows[idx].second[argCol];
                    if (v.isNull()) continue;
                    cnt++;
                    if (v.type == ValueType::Double) {
                        isFloat = true;
                        dsum += v.doubleValue;
                    } else if (v.type == ValueType::Int) {
                        isum += v.intValue;
                    }
                    if (best.isNull()) {
                        best = v;
                    } else {
                        auto c = compareValues(v, best);
                        if (c.has_value()) {
                            if (fn == "MIN" && *c < 0) best = v;
                            if (fn == "MAX" && *c > 0) best = v;
                        }
                    }
                }
                Value agg;
                if (fn == "COUNT") {
                    agg = Value::makeInt(cnt);
                } else if (fn == "SUM") {
                    agg = isFloat ? Value::makeDouble(dsum + static_cast<double>(isum))
                                  : Value::makeInt(isum);
                } else if (fn == "AVG") {
                    agg = cnt == 0 ? Value::null()
                                   : Value::makeDouble((dsum + static_cast<double>(isum)) /
                                                       static_cast<double>(cnt));
                } else {
                    agg = best;
                }
                for (int idx : idxs) result[idx] = agg;
            }
        }
        winValues.push_back(std::move(result));
    }

    for (auto& col : node.columns) {
        result_.columns.push_back(col->alias.empty() ? col->column : col->alias);
    }

    std::vector<int> order(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) order[i] = i;
    if (!node.orderBy.empty()) {
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            for (auto& key : node.orderBy) {
                int ci = key.column->columnIndex;
                const Value& va = rows[a].second[ci];
                const Value& vb = rows[b].second[ci];
                if (valueLess(va, vb)) return key.ascending;
                if (valueLess(vb, va)) return !key.ascending;
            }
            return false;
        });
    }

    for (int oi : order) {
        std::vector<Value> outRow;
        outRow.reserve(node.columns.size());
        for (std::size_t c = 0; c < node.columns.size(); ++c) {
            auto& col = node.columns[c];
            if (!winValues[c].empty()) {
                outRow.push_back(winValues[c][oi]);
                continue;
            }
            if (col->computed) {
                Tuple t(rows[oi].second);
                outRow.push_back(evalExpression(*col->computed, t));
                continue;
            }
            int ci = col->columnIndex;
            outRow.push_back((ci >= 0 && ci < static_cast<int>(rows[oi].second.size()))
                                 ? rows[oi].second[ci]
                                 : Value::null());
        }
        result_.rows.push_back(std::move(outRow));
    }

    if (node.distinct) dedupeRows(result_.rows);
    if (node.offset > 0) {
        std::size_t off = static_cast<std::size_t>(node.offset);
        if (off >= result_.rows.size()) result_.rows.clear();
        else result_.rows.erase(result_.rows.begin(), result_.rows.begin() + off);
    }
    if (node.hasLimit) {
        std::size_t lim = node.limit < 0 ? 0 : static_cast<std::size_t>(node.limit);
        if (result_.rows.size() > lim) result_.rows.resize(lim);
    }
}

std::size_t ExecutorEngine::estimateRows(int tableId) {
    Schema schema;
    std::vector<std::string> names;
    loadSchema(tableId, schema, names);
    std::size_t n = 0;
    for (TableIterator it(&storage_.tables(), tableId); it.valid(); it.next()) ++n;
    return n;
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
        std::string label = node.groupBy.empty() ? "Aggregate" : "GroupAggregate";
        if (isVectorizableAggregate(node)) {
            label += node.where ? " (Vectorized, Data Skipping)" : " (Vectorized)";
        }
        emit(label);
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
        std::string algoLabel = " Join (Nested Loop)";
        if (hash) {
            CostModel cm;
            JoinAlgorithm algo = cm.chooseEquiJoin(estimateRows(node.tableId),
                                                   estimateRows(node.joinTableId));
            algoLabel = (algo == JoinAlgorithm::Merge) ? " Join (Merge)" : " Join (Hash)";
        }
        emit(std::string(kind) + algoLabel);
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

namespace {

bool buildVecPredicate(const parser::Expression* e, VecPredicate& out) {
    using namespace parser;
    if (auto* log = dynamic_cast<const LogicalExpr*>(e)) {
        if (log->op != LogicalOp::And) return false;
        return buildVecPredicate(log->left.get(), out) &&
               buildVecPredicate(log->right.get(), out);
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(e)) {
        const ColumnRef* col = dynamic_cast<const ColumnRef*>(bin->left.get());
        const LiteralExpr* lit = dynamic_cast<const LiteralExpr*>(bin->right.get());
        ComparisonOp op = bin->op;
        if (col == nullptr || lit == nullptr) {
            col = dynamic_cast<const ColumnRef*>(bin->right.get());
            lit = dynamic_cast<const LiteralExpr*>(bin->left.get());
            op = flipOp(op);
        }
        if (col == nullptr || lit == nullptr) return false;
        if (col->outerRef || col->computed || col->columnIndex < 0) return false;
        VecPredicate::Term term;
        term.column = col->columnIndex;
        term.op = op;
        term.literal = evalExpression(*lit, Tuple{});
        out.terms.push_back(std::move(term));
        return true;
    }
    return false;
}

}  // namespace

bool ExecutorEngine::isVectorizableAggregate(
    const parser::SelectStatement& node) const {
    if (node.aggregates.empty()) return false;
    if (!node.groupBy.empty() || node.having) return false;
    if (!node.columns.empty()) return false;
    if (node.distinct || node.asOf) return false;
    if (txnActive()) return false;

    const semantic::TableSchema* ts = catalog_.getTableById(node.tableId);
    if (ts != nullptr && ts->isView) return false;

    for (const auto& fn : node.aggregates) {
        if (fn->distinct) return false;
        if (fn->name == "COUNT" && fn->star) continue;
        if (!fn->argument || fn->argument->columnIndex < 0) return false;
        if (fn->name != "COUNT" && fn->name != "SUM" && fn->name != "AVG" &&
            fn->name != "MIN" && fn->name != "MAX") {
            return false;
        }
    }
    if (node.where) {
        if (hasCorrelatedSubquery(node.where.get())) return false;
        VecPredicate pred;
        if (!buildVecPredicate(node.where.get(), pred)) return false;
    }
    return true;
}

bool ExecutorEngine::tryVectorizedAggregate(parser::SelectStatement& node,
                                            const Schema& schema) {
    if (!isVectorizableAggregate(node)) return false;

    std::vector<VecAggregate> aggs;
    for (const auto& fn : node.aggregates) {
        VecAggregate va;
        if (fn->name == "COUNT" && fn->star) {
            va.kind = VecAggregate::Kind::CountStar;
        } else {
            va.column = fn->argument->columnIndex;
            if (fn->name == "COUNT") {
                va.kind = VecAggregate::Kind::Count;
            } else if (fn->name == "SUM") {
                va.kind = VecAggregate::Kind::Sum;
            } else if (fn->name == "AVG") {
                va.kind = VecAggregate::Kind::Avg;
            } else if (fn->name == "MIN") {
                va.kind = VecAggregate::Kind::Min;
            } else {
                va.kind = VecAggregate::Kind::Max;
            }
        }
        aggs.push_back(va);
    }

    std::optional<VecPredicate> predicate;
    if (node.where) {
        VecPredicate pred;
        buildVecPredicate(node.where.get(), pred);
        predicate = std::move(pred);
    }

    const TableColumns& cols =
        storage_.columns().getOrBuild(node.tableId, schema, storage_.tables());
    std::vector<Value> row = columnarAggregate(cols, aggs, predicate);

    for (const auto& fn : node.aggregates) {
        result_.columns.push_back(fn->alias.empty() ? aggLabel(*fn) : fn->alias);
    }
    result_.rows.push_back(std::move(row));

    if (node.offset > 0) {
        std::size_t off = static_cast<std::size_t>(node.offset);
        if (off >= result_.rows.size()) {
            result_.rows.clear();
        } else {
            result_.rows.erase(result_.rows.begin(), result_.rows.begin() + off);
        }
    }
    if (node.hasLimit && node.limit >= 0 &&
        static_cast<std::size_t>(node.limit) < result_.rows.size()) {
        result_.rows.resize(static_cast<std::size_t>(node.limit));
    }
    return true;
}

void ExecutorEngine::visit(parser::SelectStatement& node) {
    optimizeSelect(node);
    if (node.explain) {
        explainSelect(node);
        return;
    }
    result_.isQuery = true;
    materializeSubqueries(node.where.get());
    materializeSubqueries(node.having.get());
    materializeSubqueries(node.joinOn.get());
    for (auto& jc : node.extraJoins) materializeSubqueries(jc.on.get());

    bool hasWindow = false;
    for (auto& col : node.columns) {
        if (col->computed &&
            dynamic_cast<parser::WindowExpr*>(col->computed.get()) != nullptr) {
            hasWindow = true;
            break;
        }
    }
    if (hasWindow) {
        runWindowQuery(node);
        return;
    }

    std::vector<std::string> names;
    std::vector<std::pair<RecordID, std::vector<Value>>> rows;

    if (!node.joinTable.empty() && !node.extraJoins.empty()) {
        Schema bschema;
        std::vector<std::string> bnames;
        loadSchema(node.tableId, bschema, bnames);
        names = bnames;
        auto acc = gatherJoinInput(node, node.tableId, bschema);
        int accWidth = static_cast<int>(bnames.size());

        Schema fschema;
        std::vector<std::string> fnames;
        loadSchema(node.joinTableId, fschema, fnames);
        names.insert(names.end(), fnames.begin(), fnames.end());
        auto firstRight = gatherJoinInput(node, node.joinTableId, fschema);
        acc = joinTwo(acc, accWidth, firstRight, static_cast<int>(fnames.size()),
                      node.joinType, node.joinOn.get());
        accWidth += static_cast<int>(fnames.size());

        for (auto& jc : node.extraJoins) {
            Schema jschema;
            std::vector<std::string> jnames;
            loadSchema(jc.tableId, jschema, jnames);
            names.insert(names.end(), jnames.begin(), jnames.end());
            auto rightRows = gatherJoinInput(node, jc.tableId, jschema);
            acc = joinTwo(acc, accWidth, rightRows, static_cast<int>(jnames.size()),
                          jc.kind, jc.on.get());
            accWidth += static_cast<int>(jnames.size());
        }

        if (node.where) {
            std::vector<std::pair<RecordID, std::vector<Value>>> filtered;
            for (auto& pr : acc) {
                Tuple t(pr.second);
                if (predicateTrue(*node.where, t)) filtered.push_back(std::move(pr));
            }
            rows = std::move(filtered);
        } else {
            rows = std::move(acc);
        }
    } else if (!node.joinTable.empty()) {
        Schema lschema, rschema;
        std::vector<std::string> lnames, rnames;
        loadSchema(node.tableId, lschema, lnames);
        loadSchema(node.joinTableId, rschema, rnames);
        names = lnames;
        names.insert(names.end(), rnames.begin(), rnames.end());
        const int leftWidth = static_cast<int>(lnames.size());
        const int rightWidth = static_cast<int>(rnames.size());

        auto leftRows = gatherJoinInput(node, node.tableId, lschema);
        auto rightRows = gatherJoinInput(node, node.joinTableId, rschema);

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
                CostModel cm;
                JoinAlgorithm algo =
                    cm.chooseEquiJoin(leftRows.size(), rightRows.size());
                if (algo == JoinAlgorithm::Merge) {
                    std::vector<std::vector<Value>> lv, rv;
                    lv.reserve(leftRows.size());
                    for (auto& lp : leftRows) lv.push_back(std::move(lp.second));
                    rv.reserve(rightRows.size());
                    for (auto& rp : rightRows) rv.push_back(std::move(rp.second));
                    std::vector<std::vector<Value>> joined =
                        mergeJoinInner(std::move(lv), leftKey, std::move(rv), rightKey);
                    for (auto& combined : joined) {
                        Tuple ct(combined);
                        if (node.where && !predicateTrue(*node.where, ct)) continue;
                        rows.emplace_back(RecordID{}, std::move(combined));
                    }
                } else {
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

        if (!node.aggregates.empty() || !node.groupBy.empty() || node.having) {
            if (tryVectorizedAggregate(node, schema)) return;
            std::vector<std::pair<RecordID, std::vector<Value>>> arows;
            if (node.where && hasCorrelatedSubquery(node.where.get())) {
                auto allRows = sourceRows(node, schema, nullptr);
                for (auto& pr : allRows) {
                    bindCorrelated(node.where.get(), pr.second);
                    Tuple t(pr.second);
                    if (predicateTrue(*node.where, t)) arows.push_back(std::move(pr));
                }
            } else {
                arows = sourceRows(node, schema, node.where.get());
            }
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
                    if (!evalHavingBool(*node.having, g.rows, repTuple)) continue;
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

        if (node.where && hasCorrelatedSubquery(node.where.get())) {
            auto allRows = sourceRows(node, schema, nullptr);
            for (auto& pr : allRows) {
                bindCorrelated(node.where.get(), pr.second);
                Tuple t(pr.second);
                if (predicateTrue(*node.where, t)) rows.push_back(std::move(pr));
            }
        } else {
            rows = sourceRows(node, schema, node.where.get());
        }
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
                    if (hasCorrelatedSubquery(col->computed.get())) {
                        bindCorrelated(col->computed.get(), src);
                    }
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
    storage_.columns().clear();
    if (!txnActive()) storage_.versions().discardPending();

    std::vector<index::Index*> tableIndexes = storage_.indexes().forTable(node.tableId);

    std::vector<std::pair<RecordID, Tuple>> victims;
    const bool correlated = node.where && hasCorrelatedSubquery(node.where.get());
    {
        SeqScanExecutor scan(&storage_.tables(), node.tableId, schema);
        scan.init();
        Tuple t;
        RecordID rid;
        while (scan.next(t, rid)) {
            if (node.where) {
                if (correlated) bindCorrelated(node.where.get(), t.values());
                if (!predicateTrue(*node.where, t)) continue;
            }
            victims.emplace_back(rid, t);
        }
    }

    if (ts != nullptr) {
        for (auto& [vrid, vtuple] : victims) {
            (void)vrid;
            applyReferentialActions(*ts, vtuple.values());
        }
    }

    for (auto& [vrid, vtuple] : victims) {
        if (txnActive()) lockOrThrow(vrid, /*exclusive=*/true);
        for (index::Index* idx : tableIndexes) {
            if (idx->coversRow(vtuple.size())) {
                idx->remove(idx->keyOf(vtuple.values()), vrid);
            }
        }
        if (txnActive()) {
            StorageEngine* se = &storage_;
            int tid = node.tableId;
            std::string bytes = vtuple.serialize(schema);
            std::vector<std::pair<index::Index*, Value>> idxKeys;
            for (index::Index* idx : tableIndexes) {
                if (idx->coversRow(vtuple.size())) {
                    idxKeys.emplace_back(idx, idx->keyOf(vtuple.values()));
                }
            }
            txnMgr_->logDelete(*currentTxn_, tid, vrid, bytes);
            txnMgr_->registerUndo(*currentTxn_, [se, tid, bytes, idxKeys] {
                RecordID nr = se->tables().insertTuple(tid, bytes);
                for (const auto& [idx, key] : idxKeys) idx->add(key, nr);
            });
        }
        storage_.tables().eraseTuple(node.tableId, vrid);
        storage_.versions().stageDelete(node.tableId, vrid);
    }
    if (!txnActive()) storage_.versions().commitPending();
    if (node.returningStar || !node.returning.empty()) {
        result_.isQuery = true;
        if (node.returningStar) {
            result_.columns = names;
        } else {
            for (auto& col : node.returning) {
                result_.columns.push_back(names[col->columnIndex]);
            }
        }
        for (auto& [vrid, vtuple] : victims) {
            (void)vrid;
            if (node.returningStar) {
                result_.rows.push_back(vtuple.values());
            } else {
                std::vector<Value> proj;
                proj.reserve(node.returning.size());
                for (auto& col : node.returning) {
                    proj.push_back(vtuple.at(col->columnIndex));
                }
                result_.rows.push_back(std::move(proj));
            }
        }
    } else {
        result_.message = "REMOVE " + std::to_string(victims.size());
    }
}

void ExecutorEngine::visit(parser::UpdateStatement& node) {
    Schema schema;
    std::vector<std::string> names;
    loadSchema(node.tableId, schema, names);
    materializeSubqueries(node.where.get());
    const semantic::TableSchema* ts = catalog_.getTableById(node.tableId);
    storage_.columns().clear();
    if (!txnActive()) storage_.versions().discardPending();

    std::vector<index::Index*> tableIndexes = storage_.indexes().forTable(node.tableId);
    std::vector<std::pair<RecordID, std::vector<Value>>> rows;
    std::vector<std::vector<Value>> matchFrom;
    const bool joinUpdate = !node.fromTable.empty();
    if (joinUpdate) {
        Schema fromSchema;
        std::vector<std::string> fromNames;
        loadSchema(node.fromTableId, fromSchema, fromNames);
        auto fromRows = gatherRows(node.fromTableId, fromSchema, nullptr);
        auto targetRows = gatherRows(node.tableId, schema, nullptr);
        for (auto& tr : targetRows) {
            for (auto& fr : fromRows) {
                std::vector<Value> combined = tr.second;
                combined.insert(combined.end(), fr.second.begin(), fr.second.end());
                Tuple ct(std::move(combined));
                if (!node.where || predicateTrue(*node.where, ct)) {
                    rows.push_back(tr);
                    matchFrom.push_back(fr.second);
                    break;
                }
            }
        }
    } else if (node.where && hasCorrelatedSubquery(node.where.get())) {
        auto allRows = gatherRows(node.tableId, schema, nullptr);
        for (auto& pr : allRows) {
            bindCorrelated(node.where.get(), pr.second);
            Tuple t(pr.second);
            if (predicateTrue(*node.where, t)) rows.push_back(std::move(pr));
        }
    } else {
        rows = gatherRows(node.tableId, schema, node.where.get());
    }

    int count = 0;
    const bool wantReturn = node.returningStar || !node.returning.empty();
    std::vector<std::vector<Value>> returned;
    std::size_t updIdx = 0;
    for (auto& [rid, row] : rows) {
        if (txnActive()) lockOrThrow(rid, /*exclusive=*/true);
        Tuple oldTup(row);
        std::vector<Value> newVals = row;
        if (joinUpdate) {
            std::vector<Value> combined = row;
            combined.insert(combined.end(), matchFrom[updIdx].begin(),
                            matchFrom[updIdx].end());
            Tuple ct(std::move(combined));
            for (std::size_t i = 0; i < node.targetIndices.size(); ++i) {
                newVals[node.targetIndices[i]] =
                    evalExpression(*node.values[i], ct);
            }
        } else {
            for (std::size_t i = 0; i < node.targetIndices.size(); ++i) {
                newVals[node.targetIndices[i]] =
                    evalExpression(*node.values[i], oldTup);
            }
        }
        ++updIdx;
        Tuple newTup(newVals);
        if (ts != nullptr) {
            checkForeignKeys(*ts, newTup.values());
            enforceConstraints(*ts, node.tableId, newTup.values(), &rid);
            applyUpdateReferentialActions(*ts, row, newVals);
        }
        std::string oldBytes = oldTup.serialize(schema);
        std::string newBytes = newTup.serialize(schema);

        for (index::Index* idx : tableIndexes) {
            if (idx->coversRow(oldTup.size())) {
                idx->remove(idx->keyOf(oldTup.values()), rid);
            }
        }
        RecordID nr = storage_.tables().updateTuple(node.tableId, rid, newBytes);
        if (txnActive()) lockOrThrow(nr, /*exclusive=*/true);
        for (index::Index* idx : tableIndexes) {
            if (idx->coversRow(newTup.size())) {
                idx->add(idx->keyOf(newTup.values()), nr);
            }
        }
        storage_.versions().stageDelete(node.tableId, rid);
        storage_.versions().stageInsert(node.tableId, nr, newBytes);
        if (txnActive()) {
            StorageEngine* se = &storage_;
            int tid = node.tableId;
            txnMgr_->logDelete(*currentTxn_, tid, rid, oldBytes);
            txnMgr_->logInsert(*currentTxn_, tid, nr, newBytes);
            std::vector<std::pair<index::Index*, Value>> oldKeys, newKeys;
            for (index::Index* idx : tableIndexes) {
                if (idx->coversRow(oldTup.size())) {
                    oldKeys.emplace_back(idx, idx->keyOf(oldTup.values()));
                    newKeys.emplace_back(idx, idx->keyOf(newTup.values()));
                }
            }
            txnMgr_->registerUndo(*currentTxn_, [se, tid, nr, oldBytes, oldKeys, newKeys] {
                for (const auto& [idx, key] : newKeys) idx->remove(key, nr);
                se->tables().eraseTuple(tid, nr);
                RecordID rr = se->tables().insertTuple(tid, oldBytes);
                for (const auto& [idx, key] : oldKeys) idx->add(key, rr);
            });
        }
        if (wantReturn) {
            if (node.returningStar) {
                returned.push_back(newTup.values());
            } else {
                std::vector<Value> proj;
                proj.reserve(node.returning.size());
                for (auto& col : node.returning) {
                    proj.push_back(newTup.at(col->columnIndex));
                }
                returned.push_back(std::move(proj));
            }
        }
        ++count;
    }
    if (!txnActive()) storage_.versions().commitPending();
    if (wantReturn) {
        result_.isQuery = true;
        if (node.returningStar) {
            result_.columns = names;
        } else {
            for (auto& col : node.returning) {
                result_.columns.push_back(names[col->columnIndex]);
            }
        }
        result_.rows = std::move(returned);
    } else {
        result_.message = "MODIFY " + std::to_string(count);
    }
}

void ExecutorEngine::visit(parser::DropStatement& node) {
    storage_.columns().clear();
    if (node.truncate) {
        Schema schema;
        std::vector<std::string> names;
        loadSchema(node.tableId, schema, names);
        if (!txnActive()) storage_.versions().discardPending();
        std::vector<index::Index*> idxs = storage_.indexes().forTable(node.tableId);
        SeqScanExecutor scan(&storage_.tables(), node.tableId, schema);
        scan.init();
        Tuple t;
        RecordID rid;
        while (scan.next(t, rid)) {
            for (index::Index* idx : idxs) {
                if (idx->coversRow(t.size())) idx->remove(idx->keyOf(t.values()), rid);
            }
            storage_.versions().stageDelete(node.tableId, rid);
        }
        storage_.tables().truncateTable(node.tableId);
        if (!txnActive()) storage_.versions().commitPending();
        result_.message = "TRUNCATE RELATION";
        return;
    }
    if (node.isIndex) {
        storage_.indexes().drop(node.name);
        result_.message = "DISCARD INDEX";
    } else if (node.isView) {
        result_.message = "DISCARD VIEW";
    } else {
        storage_.tables().dropTable(node.tableId);
        storage_.indexes().dropTable(node.tableId);
        result_.message = "DISCARD RELATION";
    }
}

void ExecutorEngine::visit(parser::AlterStatement& node) {
    storage_.columns().clear();
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
            semantic::ForeignKey::Action act =
                node.column.refOnDelete == 1   ? semantic::ForeignKey::Action::Cascade
                : node.column.refOnDelete == 2 ? semantic::ForeignKey::Action::SetNull
                                               : semantic::ForeignKey::Action::Restrict;
            semantic::ForeignKey::Action actUpd =
                node.column.refOnUpdate == 1   ? semantic::ForeignKey::Action::Cascade
                : node.column.refOnUpdate == 2 ? semantic::ForeignKey::Action::SetNull
                                               : semantic::ForeignKey::Action::Restrict;
            catalog_.addForeignKey(node.table,
                                   static_cast<int>(newSchema.size()) - 1,
                                   node.column.refTable, node.column.refColumn, act,
                                   actUpd);
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
                storage_.versions().beginSnapshot(*currentTxn_);
                result_.message = "START";
            }
            break;
        case parser::TransactionStatement::Kind::Commit:
            if (*currentTxn_ == 0) {
                result_.message = "WARNING: no transaction in progress";
            } else {
                int tid = *currentTxn_;
                txnMgr_->commit(tid);
                *currentTxn_ = 0;
                storage_.versions().commitPending();
                storage_.versions().endSnapshot(tid);
                result_.message = "SAVE";
            }
            break;
        case parser::TransactionStatement::Kind::Rollback:
            if (*currentTxn_ == 0) {
                result_.message = "WARNING: no transaction in progress";
            } else {
                int tid = *currentTxn_;
                txnMgr_->rollback(tid);
                *currentTxn_ = 0;
                storage_.versions().discardPending();
                storage_.versions().endSnapshot(tid);
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

void ExecutorEngine::visit(parser::CreateViewStatement& node) {
    (void)node;
    result_.message = "BUILD VIEW";
}

}
