#include "vm/vectorized.hpp"

#include <string>
#include <unordered_map>

#include "vm/executor.hpp"
#include "vm/storage_engine.hpp"

namespace db::vm {

namespace {

bool termPasses(const VecPredicate::Term& term, const Value& v) {
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

/* Narrows a batch's selection vector to the rows passing every term. */
class VectorFilter : public BatchSink {
public:
    VectorFilter(const VecPredicate& pred, BatchSink* next)
        : pred_(pred), next_(next) {}

    void consume(Batch& batch) override {
        std::vector<std::uint32_t> kept;
        kept.reserve(batch.selection.size());
        for (std::uint32_t r : batch.selection) {
            bool ok = true;
            for (const auto& term : pred_.terms) {
                if (!termPasses(term, batch.columns[term.column][r])) {
                    ok = false;
                    break;
                }
            }
            if (ok) kept.push_back(r);
        }
        batch.selection.swap(kept);
        next_->consume(batch);
    }

    void finish() override { next_->finish(); }

private:
    VecPredicate pred_;
    BatchSink* next_;
};

/* Folds the selected rows of each batch into per-aggregate accumulators. */
class VectorAggregator : public BatchSink {
public:
    explicit VectorAggregator(const std::vector<VecAggregate>& aggs)
        : aggs_(aggs), acc_(aggs.size()) {}

    void consume(Batch& batch) override {
        for (std::size_t a = 0; a < aggs_.size(); ++a) {
            const VecAggregate& agg = aggs_[a];
            Acc& acc = acc_[a];
            if (agg.kind == VecAggregate::Kind::CountStar) {
                acc.count += batch.selection.size();
                continue;
            }
            for (std::uint32_t r : batch.selection) {
                const Value& v = batch.columns[agg.column][r];
                if (v.isNull()) continue;
                switch (agg.kind) {
                    case VecAggregate::Kind::Count:
                        ++acc.count;
                        break;
                    case VecAggregate::Kind::Sum:
                    case VecAggregate::Kind::Avg:
                        if (v.type == ValueType::Double) {
                            acc.isFloat = true;
                            acc.dsum += v.doubleValue;
                        } else {
                            acc.isum += v.intValue;
                        }
                        ++acc.count;
                        break;
                    case VecAggregate::Kind::Min:
                    case VecAggregate::Kind::Max: {
                        if (!acc.hasBest) {
                            acc.best = v;
                            acc.hasBest = true;
                        } else {
                            auto cmp = compareValues(v, acc.best);
                            if (cmp.has_value()) {
                                if (agg.kind == VecAggregate::Kind::Min && *cmp < 0) {
                                    acc.best = v;
                                }
                                if (agg.kind == VecAggregate::Kind::Max && *cmp > 0) {
                                    acc.best = v;
                                }
                            }
                        }
                        break;
                    }
                    case VecAggregate::Kind::CountStar:
                        break;
                }
            }
        }
    }

    void finish() override {}

    std::vector<Value> result() const {
        std::vector<Value> out;
        out.reserve(aggs_.size());
        for (std::size_t a = 0; a < aggs_.size(); ++a) {
            const VecAggregate& agg = aggs_[a];
            const Acc& acc = acc_[a];
            switch (agg.kind) {
                case VecAggregate::Kind::CountStar:
                case VecAggregate::Kind::Count:
                    out.push_back(Value::makeInt(static_cast<std::int64_t>(acc.count)));
                    break;
                case VecAggregate::Kind::Sum:
                    if (acc.count == 0) {
                        out.push_back(Value::null());
                    } else if (acc.isFloat) {
                        out.push_back(Value::makeDouble(
                            acc.dsum + static_cast<double>(acc.isum)));
                    } else {
                        out.push_back(Value::makeInt(acc.isum));
                    }
                    break;
                case VecAggregate::Kind::Avg:
                    if (acc.count == 0) {
                        out.push_back(Value::null());
                    } else {
                        double total = acc.dsum + static_cast<double>(acc.isum);
                        out.push_back(
                            Value::makeDouble(total / static_cast<double>(acc.count)));
                    }
                    break;
                case VecAggregate::Kind::Min:
                case VecAggregate::Kind::Max:
                    out.push_back(acc.hasBest ? acc.best : Value::null());
                    break;
            }
        }
        return out;
    }

private:
    struct Acc {
        std::int64_t count = 0;
        std::int64_t isum = 0;
        double dsum = 0.0;
        bool isFloat = false;
        bool hasBest = false;
        Value best;
    };

    const std::vector<VecAggregate>& aggs_;
    std::vector<Acc> acc_;
};

/* Encodes a value into a hash-table key (type-tagged so different types never
 * collide). */
std::string keyOf(const Value& v) {
    return std::to_string(static_cast<int>(v.type)) + ':' + v.toString();
}

/* Per-group aggregate accumulator (mirrors VectorAggregator's Acc semantics). */
struct GAcc {
    std::int64_t count = 0;
    std::int64_t isum = 0;
    double dsum = 0.0;
    bool isFloat = false;
    bool hasBest = false;
    Value best;
};

void gAccumulate(GAcc& acc, VecAggregate::Kind kind, const Value& v) {
    if (kind == VecAggregate::Kind::CountStar) {
        ++acc.count;
        return;
    }
    if (v.isNull()) return;
    switch (kind) {
        case VecAggregate::Kind::Count:
            ++acc.count;
            break;
        case VecAggregate::Kind::Sum:
        case VecAggregate::Kind::Avg:
            if (v.type == ValueType::Double) {
                acc.isFloat = true;
                acc.dsum += v.doubleValue;
            } else {
                acc.isum += v.intValue;
            }
            ++acc.count;
            break;
        case VecAggregate::Kind::Min:
        case VecAggregate::Kind::Max: {
            if (!acc.hasBest) {
                acc.best = v;
                acc.hasBest = true;
            } else {
                auto cmp = compareValues(v, acc.best);
                if (cmp.has_value()) {
                    if (kind == VecAggregate::Kind::Min && *cmp < 0) acc.best = v;
                    if (kind == VecAggregate::Kind::Max && *cmp > 0) acc.best = v;
                }
            }
            break;
        }
        case VecAggregate::Kind::CountStar:
            break;
    }
}

Value gFinalize(VecAggregate::Kind kind, const GAcc& acc) {
    switch (kind) {
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

/* Hash GROUP BY: folds each batch into per-group accumulators. */
class VectorGroupAggregator : public BatchSink {
public:
    VectorGroupAggregator(const std::vector<int>& groupCols,
                          const std::vector<VecAggregate>& aggs)
        : groupCols_(groupCols), aggs_(aggs) {}

    void consume(Batch& batch) override {
        for (std::uint32_t r : batch.selection) {
            std::string key;
            for (int gc : groupCols_) {
                key += keyOf(batch.columns[gc][r]);
                key += '\x1e';
            }
            auto it = groups_.find(key);
            if (it == groups_.end()) {
                Group g;
                g.accs.resize(aggs_.size());
                for (int gc : groupCols_) g.keyVals.push_back(batch.columns[gc][r]);
                it = groups_.emplace(std::move(key), std::move(g)).first;
                order_.push_back(it->first);
            }
            Group& g = it->second;
            for (std::size_t a = 0; a < aggs_.size(); ++a) {
                const VecAggregate& agg = aggs_[a];
                const Value& v = agg.column >= 0 ? batch.columns[agg.column][r]
                                                 : Value::null();
                gAccumulate(g.accs[a], agg.kind, v);
            }
        }
    }

    void finish() override {}

    std::vector<std::vector<Value>> result() const {
        std::vector<std::vector<Value>> out;
        out.reserve(order_.size());
        for (const std::string& key : order_) {
            const Group& g = groups_.at(key);
            std::vector<Value> row = g.keyVals;
            for (std::size_t a = 0; a < aggs_.size(); ++a) {
                row.push_back(gFinalize(aggs_[a].kind, g.accs[a]));
            }
            out.push_back(std::move(row));
        }
        return out;
    }

private:
    struct Group {
        std::vector<Value> keyVals;
        std::vector<GAcc> accs;
    };
    std::vector<int> groupCols_;
    std::vector<VecAggregate> aggs_;
    std::unordered_map<std::string, Group> groups_;
    std::vector<std::string> order_;
};

using HashTable = std::unordered_map<std::string, std::vector<std::vector<Value>>>;

/* Probe side of a hash join: emits probe-row ++ build-row for every match. */
class VectorHashProbe : public BatchSink {
public:
    VectorHashProbe(const HashTable& ht, int probeKeyCol,
                    std::vector<std::vector<Value>>& out)
        : ht_(ht), probeKeyCol_(probeKeyCol), out_(out) {}

    void consume(Batch& batch) override {
        const std::size_t ncols = batch.columns.size();
        for (std::uint32_t r : batch.selection) {
            const Value& k = batch.columns[probeKeyCol_][r];
            if (k.isNull()) continue;
            auto it = ht_.find(keyOf(k));
            if (it == ht_.end()) continue;
            std::vector<Value> probeRow;
            probeRow.reserve(ncols);
            for (std::size_t c = 0; c < ncols; ++c) {
                probeRow.push_back(batch.columns[c][r]);
            }
            for (const auto& buildRow : it->second) {
                std::vector<Value> combined = probeRow;
                combined.insert(combined.end(), buildRow.begin(), buildRow.end());
                out_.push_back(std::move(combined));
            }
        }
    }

    void finish() override {}

private:
    const HashTable& ht_;
    int probeKeyCol_;
    std::vector<std::vector<Value>>& out_;
};

/* Scans a table into fixed-size column batches and pushes them into a sink. */
void scanIntoSink(StorageEngine& storage, int tableId, const Schema& schema,
                  BatchSink* root, std::size_t batchSize) {
    const std::size_t ncols = schema.size();
    Batch batch;
    batch.columns.resize(ncols);
    for (auto& c : batch.columns) c.reserve(batchSize);
    batch.selection.reserve(batchSize);

    SeqScanExecutor scan(&storage.tables(), tableId, schema);
    scan.init();
    Tuple tuple;
    RecordID rid;
    std::size_t filled = 0;
    while (scan.next(tuple, rid)) {
        for (std::size_t c = 0; c < ncols; ++c) {
            batch.columns[c].push_back(c < tuple.size() ? tuple.at(static_cast<int>(c))
                                                        : Value::null());
        }
        batch.selection.push_back(static_cast<std::uint32_t>(filled));
        ++filled;
        if (filled == batchSize) {
            root->consume(batch);
            for (auto& c : batch.columns) c.clear();
            batch.selection.clear();
            filled = 0;
        }
    }
    if (filled > 0) root->consume(batch);
    root->finish();
}

}  // namespace

std::vector<Value> runVectorizedAggregate(StorageEngine& storage, int tableId,
                                          const Schema& schema,
                                          const std::optional<VecPredicate>& predicate,
                                          const std::vector<VecAggregate>& aggregates,
                                          std::size_t batchSize) {
    VectorAggregator aggregator(aggregates);
    VectorFilter filter(predicate ? *predicate : VecPredicate{}, &aggregator);
    BatchSink* root = predicate ? static_cast<BatchSink*>(&filter)
                                : static_cast<BatchSink*>(&aggregator);

    const std::size_t ncols = schema.size();
    Batch batch;
    batch.columns.resize(ncols);
    for (auto& c : batch.columns) c.reserve(batchSize);
    batch.selection.reserve(batchSize);

    SeqScanExecutor scan(&storage.tables(), tableId, schema);
    scan.init();
    Tuple tuple;
    RecordID rid;
    std::size_t filled = 0;
    while (scan.next(tuple, rid)) {
        for (std::size_t c = 0; c < ncols; ++c) {
            batch.columns[c].push_back(c < tuple.size() ? tuple.at(static_cast<int>(c))
                                                        : Value::null());
        }
        batch.selection.push_back(static_cast<std::uint32_t>(filled));
        ++filled;
        if (filled == batchSize) {
            root->consume(batch);
            for (auto& c : batch.columns) c.clear();
            batch.selection.clear();
            filled = 0;
        }
    }
    if (filled > 0) {
        root->consume(batch);
    }
    root->finish();
    return aggregator.result();
}

std::vector<std::vector<Value>> runVectorizedGroupAggregate(
    StorageEngine& storage, int tableId, const Schema& schema,
    const std::optional<VecPredicate>& predicate,
    const std::vector<int>& groupColumns,
    const std::vector<VecAggregate>& aggregates, std::size_t batchSize) {
    VectorGroupAggregator aggregator(groupColumns, aggregates);
    VectorFilter filter(predicate ? *predicate : VecPredicate{}, &aggregator);
    BatchSink* root = predicate ? static_cast<BatchSink*>(&filter)
                                : static_cast<BatchSink*>(&aggregator);
    scanIntoSink(storage, tableId, schema, root, batchSize);
    return aggregator.result();
}

std::vector<std::vector<Value>> runVectorizedHashJoin(
    StorageEngine& storage, int buildTableId, const Schema& buildSchema,
    int buildKeyColumn, int probeTableId, const Schema& probeSchema,
    int probeKeyColumn, std::size_t batchSize) {
    HashTable ht;
    {
        SeqScanExecutor scan(&storage.tables(), buildTableId, buildSchema);
        scan.init();
        Tuple tuple;
        RecordID rid;
        while (scan.next(tuple, rid)) {
            if (buildKeyColumn >= static_cast<int>(tuple.size())) continue;
            const Value& k = tuple.at(buildKeyColumn);
            if (k.isNull()) continue;
            ht[keyOf(k)].push_back(tuple.values());
        }
    }

    std::vector<std::vector<Value>> out;
    VectorHashProbe probe(ht, probeKeyColumn, out);
    scanIntoSink(storage, probeTableId, probeSchema, &probe, batchSize);
    return out;
}

}  // namespace db::vm
