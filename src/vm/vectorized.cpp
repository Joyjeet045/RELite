#include "vm/vectorized.hpp"

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

}  // namespace db::vm
