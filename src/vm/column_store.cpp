#include "vm/column_store.hpp"

#include <algorithm>
#include <thread>

#include "vm/table_manager.hpp"

namespace db::vm {

namespace {

Value columnValue(const Column& col, std::size_t row) {
    if (col.isNull[row]) return Value::null();
    switch (col.type) {
        case parser::DataType::Int:
            return Value::makeInt(col.ints[row]);
        case parser::DataType::Float:
            return Value::makeDouble(col.doubles[row]);
        case parser::DataType::Bool:
            return Value::makeBool(col.bools[row] != 0);
        default:
            return Value::makeText(col.texts[row]);
    }
}

/* Build one min/max zone map per block of kBlockRows for a column. */
void buildZones(Column& col, std::size_t rows) {
    const std::size_t block = TableColumns::kBlockRows;
    const std::size_t nblocks = (rows + block - 1) / block;
    col.zones.assign(nblocks, ColumnZone{});
    for (std::size_t b = 0; b < nblocks; ++b) {
        std::size_t start = b * block;
        std::size_t end = std::min(start + block, rows);
        ColumnZone& z = col.zones[b];
        for (std::size_t r = start; r < end; ++r) {
            if (col.isNull[r]) continue;
            Value v = columnValue(col, r);
            if (!z.hasNonNull) {
                z.min = v;
                z.max = v;
                z.hasNonNull = true;
            } else {
                auto lo = compareValues(v, z.min);
                auto hi = compareValues(v, z.max);
                if (lo.has_value() && *lo < 0) z.min = v;
                if (hi.has_value() && *hi > 0) z.max = v;
            }
        }
    }
}

/* True if a block's zone map proves no row in it can satisfy the term. */
bool blockExcludesTerm(const VecPredicate::Term& term, const ColumnZone& z) {
    if (!z.hasNonNull) return true; /* all-null block: comparison fails everywhere */
    auto cmpMin = compareValues(term.literal, z.min);
    auto cmpMax = compareValues(term.literal, z.max);
    if (!cmpMin.has_value() || !cmpMax.has_value()) return false;
    int lMin = *cmpMin; /* literal vs min */
    int lMax = *cmpMax; /* literal vs max */
    switch (term.op) {
        case parser::ComparisonOp::Eq:
            /* need min <= literal <= max */
            return lMin < 0 || lMax > 0;
        case parser::ComparisonOp::Lt:
            /* need a value < literal; impossible if min >= literal */
            return lMin <= 0;
        case parser::ComparisonOp::Leq:
            /* impossible if min > literal */
            return lMin < 0;
        case parser::ComparisonOp::Gt:
            /* need a value > literal; impossible if max <= literal */
            return lMax >= 0;
        case parser::ComparisonOp::Geq:
            /* impossible if max < literal */
            return lMax > 0;
        case parser::ComparisonOp::Neq: {
            /* only skippable if every value equals the literal */
            auto span = compareValues(z.min, z.max);
            return span.has_value() && *span == 0 && lMin == 0;
        }
    }
    return false;
}

bool blockSkippable(const VecPredicate& predicate, const TableColumns& table,
                    std::size_t blockIdx) {
    for (const VecPredicate::Term& term : predicate.terms) {
        if (term.column < 0 ||
            term.column >= static_cast<int>(table.columns.size())) {
            continue;
        }
        const Column& col = table.columns[term.column];
        if (blockIdx >= col.zones.size()) continue;
        if (blockExcludesTerm(term, col.zones[blockIdx])) return true;
    }
    return false;
}

}  // namespace

void ColumnStore::invalidate(int tableId) { cache_.erase(tableId); }

const TableColumns& ColumnStore::getOrBuild(int tableId, const Schema& schema,
                                            TableManager& tables) {
    auto it = cache_.find(tableId);
    if (it != cache_.end()) return it->second;

    TableColumns tc;
    tc.columns.resize(schema.size());
    for (std::size_t c = 0; c < schema.size(); ++c) tc.columns[c].type = schema[c];

    for (TableIterator iter(&tables, tableId); iter.valid(); iter.next()) {
        Tuple tuple = Tuple::deserialize(iter.bytes(), schema);
        for (std::size_t c = 0; c < schema.size(); ++c) {
            Column& col = tc.columns[c];
            if (c >= tuple.size() || tuple.at(static_cast<int>(c)).isNull()) {
                col.isNull.push_back(1);
                col.ints.push_back(0);
                col.doubles.push_back(0.0);
                col.bools.push_back(0);
                col.texts.emplace_back();
                continue;
            }
            const Value& v = tuple.at(static_cast<int>(c));
            col.isNull.push_back(0);
            col.ints.push_back(v.type == ValueType::Int ? v.intValue : 0);
            col.doubles.push_back(v.type == ValueType::Double ? v.doubleValue : 0.0);
            col.bools.push_back(v.type == ValueType::Bool ? (v.boolValue ? 1 : 0) : 0);
            col.texts.push_back(v.type == ValueType::Text ? v.textValue : std::string());
        }
        ++tc.rows;
    }

    for (Column& col : tc.columns) buildZones(col, tc.rows);

    auto [ins, ok] = cache_.emplace(tableId, std::move(tc));
    (void)ok;
    return ins->second;
}

namespace {

bool termPasses(const VecPredicate::Term& term, const Column& col, std::size_t row) {
    if (col.isNull[row]) return false;
    Value v = columnValue(col, row);
    auto cmp = compareValues(v, term.literal);
    if (!cmp.has_value()) return false;
    switch (term.op) {
        case parser::ComparisonOp::Eq: return *cmp == 0;
        case parser::ComparisonOp::Neq: return *cmp != 0;
        case parser::ComparisonOp::Lt: return *cmp < 0;
        case parser::ComparisonOp::Leq: return *cmp <= 0;
        case parser::ComparisonOp::Gt: return *cmp > 0;
        case parser::ComparisonOp::Geq: return *cmp >= 0;
    }
    return false;
}

}  // namespace

std::vector<Value> columnarAggregate(const TableColumns& table,
                                     const std::vector<VecAggregate>& aggregates,
                                     const std::optional<VecPredicate>& predicate,
                                     SkipStats* stats) {
    const std::size_t B = TableColumns::kBlockRows;
    const std::size_t nblocks = (table.rows + B - 1) / B;

    /* Data skipping: decide once which blocks the predicate can prune. */
    std::vector<char> skip(nblocks, 0);
    if (predicate) {
        for (std::size_t b = 0; b < nblocks; ++b) {
            skip[b] = blockSkippable(*predicate, table, b) ? 1 : 0;
        }
    }
    if (stats != nullptr) {
        stats->blocksTotal = nblocks;
        stats->blocksSkipped = 0;
        for (char s : skip) stats->blocksSkipped += (s != 0);
    }

    std::vector<Value> out;
    out.reserve(aggregates.size());

    for (const VecAggregate& agg : aggregates) {
        std::int64_t count = 0;
        std::int64_t isum = 0;
        double dsum = 0.0;
        bool isFloat = false;
        bool hasBest = false;
        Value best;

        const Column* col =
            agg.column >= 0 && agg.column < static_cast<int>(table.columns.size())
                ? &table.columns[agg.column]
                : nullptr;

        for (std::size_t b = 0; b < nblocks; ++b) {
            if (skip[b]) continue;
            std::size_t bstart = b * B;
            std::size_t bend = std::min(bstart + B, table.rows);
            for (std::size_t r = bstart; r < bend; ++r) {
            if (predicate) {
                bool ok = true;
                for (const VecPredicate::Term& term : predicate->terms) {
                    if (term.column < 0 ||
                        term.column >= static_cast<int>(table.columns.size()) ||
                        !termPasses(term, table.columns[term.column], r)) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) continue;
            }

            if (agg.kind == VecAggregate::Kind::CountStar) {
                ++count;
                continue;
            }
            if (col == nullptr || col->isNull[r]) continue;

            switch (agg.kind) {
                case VecAggregate::Kind::Count:
                    ++count;
                    break;
                case VecAggregate::Kind::Sum:
                case VecAggregate::Kind::Avg:
                    if (col->type == parser::DataType::Float) {
                        isFloat = true;
                        dsum += col->doubles[r];
                    } else {
                        isum += col->ints[r];
                    }
                    ++count;
                    break;
                case VecAggregate::Kind::Min:
                case VecAggregate::Kind::Max: {
                    Value v = columnValue(*col, r);
                    if (!hasBest) {
                        best = v;
                        hasBest = true;
                    } else {
                        auto cmp = compareValues(v, best);
                        if (cmp.has_value()) {
                            if (agg.kind == VecAggregate::Kind::Min && *cmp < 0) best = v;
                            if (agg.kind == VecAggregate::Kind::Max && *cmp > 0) best = v;
                        }
                    }
                    break;
                }
                case VecAggregate::Kind::CountStar:
                    break;
            }
            }
        }

        switch (agg.kind) {
            case VecAggregate::Kind::CountStar:
            case VecAggregate::Kind::Count:
                out.push_back(Value::makeInt(count));
                break;
            case VecAggregate::Kind::Sum:
                if (count == 0) out.push_back(Value::null());
                else if (isFloat) out.push_back(Value::makeDouble(dsum + static_cast<double>(isum)));
                else out.push_back(Value::makeInt(isum));
                break;
            case VecAggregate::Kind::Avg:
                if (count == 0) {
                    out.push_back(Value::null());
                } else {
                    double total = dsum + static_cast<double>(isum);
                    out.push_back(Value::makeDouble(total / static_cast<double>(count)));
                }
                break;
            case VecAggregate::Kind::Min:
            case VecAggregate::Kind::Max:
                out.push_back(hasBest ? best : Value::null());
                break;
        }
    }
    return out;
}

namespace {

/* Partial accumulator folded independently per worker, then merged. */
struct PartAcc {
    std::int64_t count = 0;
    std::int64_t isum = 0;
    double dsum = 0.0;
    bool isFloat = false;
    bool hasBest = false;
    Value best;
};

void accumulateRow(PartAcc& acc, const VecAggregate& agg, const TableColumns& table,
                   std::size_t r) {
    if (agg.kind == VecAggregate::Kind::CountStar) {
        ++acc.count;
        return;
    }
    const Column* col =
        agg.column >= 0 && agg.column < static_cast<int>(table.columns.size())
            ? &table.columns[agg.column]
            : nullptr;
    if (col == nullptr || col->isNull[r]) return;
    switch (agg.kind) {
        case VecAggregate::Kind::Count:
            ++acc.count;
            break;
        case VecAggregate::Kind::Sum:
        case VecAggregate::Kind::Avg:
            if (col->type == parser::DataType::Float) {
                acc.isFloat = true;
                acc.dsum += col->doubles[r];
            } else {
                acc.isum += col->ints[r];
            }
            ++acc.count;
            break;
        case VecAggregate::Kind::Min:
        case VecAggregate::Kind::Max: {
            Value v = columnValue(*col, r);
            if (!acc.hasBest) {
                acc.best = v;
                acc.hasBest = true;
            } else {
                auto cmp = compareValues(v, acc.best);
                if (cmp.has_value()) {
                    if (agg.kind == VecAggregate::Kind::Min && *cmp < 0) acc.best = v;
                    if (agg.kind == VecAggregate::Kind::Max && *cmp > 0) acc.best = v;
                }
            }
            break;
        }
        case VecAggregate::Kind::CountStar:
            break;
    }
}

void mergePartial(PartAcc& a, const PartAcc& b, VecAggregate::Kind kind) {
    a.count += b.count;
    a.isum += b.isum;
    a.dsum += b.dsum;
    a.isFloat = a.isFloat || b.isFloat;
    if ((kind == VecAggregate::Kind::Min || kind == VecAggregate::Kind::Max) &&
        b.hasBest) {
        if (!a.hasBest) {
            a.best = b.best;
            a.hasBest = true;
        } else {
            auto cmp = compareValues(b.best, a.best);
            if (cmp.has_value()) {
                if (kind == VecAggregate::Kind::Min && *cmp < 0) a.best = b.best;
                if (kind == VecAggregate::Kind::Max && *cmp > 0) a.best = b.best;
            }
        }
    }
}

Value finalizePartial(const VecAggregate& agg, const PartAcc& acc) {
    switch (agg.kind) {
        case VecAggregate::Kind::CountStar:
        case VecAggregate::Kind::Count:
            return Value::makeInt(acc.count);
        case VecAggregate::Kind::Sum:
            if (acc.count == 0) return Value::null();
            if (acc.isFloat) {
                return Value::makeDouble(acc.dsum + static_cast<double>(acc.isum));
            }
            return Value::makeInt(acc.isum);
        case VecAggregate::Kind::Avg:
            if (acc.count == 0) return Value::null();
            return Value::makeDouble((acc.dsum + static_cast<double>(acc.isum)) /
                                     static_cast<double>(acc.count));
        case VecAggregate::Kind::Min:
        case VecAggregate::Kind::Max:
            return acc.hasBest ? acc.best : Value::null();
    }
    return Value::null();
}

}  // namespace

std::vector<Value> parallelColumnarAggregate(
    const TableColumns& table, const std::vector<VecAggregate>& aggregates,
    const std::optional<VecPredicate>& predicate, unsigned numThreads,
    SkipStats* stats) {
    const std::size_t B = TableColumns::kBlockRows;
    const std::size_t nblocks = (table.rows + B - 1) / B;

    std::vector<char> skip(nblocks, 0);
    if (predicate) {
        for (std::size_t b = 0; b < nblocks; ++b) {
            skip[b] = blockSkippable(*predicate, table, b) ? 1 : 0;
        }
    }
    if (stats != nullptr) {
        stats->blocksTotal = nblocks;
        stats->blocksSkipped = 0;
        for (char s : skip) stats->blocksSkipped += (s != 0);
    }

    if (numThreads == 0) numThreads = 1;
    if (nblocks == 0) {
        std::vector<Value> out;
        PartAcc empty;
        for (const auto& agg : aggregates) out.push_back(finalizePartial(agg, empty));
        return out;
    }
    numThreads = std::min<unsigned>(numThreads, static_cast<unsigned>(nblocks));

    std::vector<std::vector<PartAcc>> partials(
        numThreads, std::vector<PartAcc>(aggregates.size()));

    auto worker = [&](unsigned t) {
        for (std::size_t b = t; b < nblocks; b += numThreads) {
            if (skip[b]) continue;
            std::size_t start = b * B;
            std::size_t end = std::min(start + B, table.rows);
            for (std::size_t r = start; r < end; ++r) {
                if (predicate) {
                    bool ok = true;
                    for (const VecPredicate::Term& term : predicate->terms) {
                        if (term.column < 0 ||
                            term.column >= static_cast<int>(table.columns.size()) ||
                            !termPasses(term, table.columns[term.column], r)) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok) continue;
                }
                for (std::size_t a = 0; a < aggregates.size(); ++a) {
                    accumulateRow(partials[t][a], aggregates[a], table, r);
                }
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(numThreads - 1);
    for (unsigned t = 1; t < numThreads; ++t) threads.emplace_back(worker, t);
    worker(0);
    for (auto& th : threads) th.join();

    std::vector<Value> out;
    out.reserve(aggregates.size());
    for (std::size_t a = 0; a < aggregates.size(); ++a) {
        PartAcc merged = partials[0][a];
        for (unsigned t = 1; t < numThreads; ++t) {
            mergePartial(merged, partials[t][a], aggregates[a].kind);
        }
        out.push_back(finalizePartial(aggregates[a], merged));
    }
    return out;
}

}  // namespace db::vm
