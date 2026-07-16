#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "frontend/ast.hpp"
#include "vm/tuple.hpp"
#include "vm/value.hpp"

namespace db::vm {

class StorageEngine;

/*
 * A minimal push-based, batch-at-a-time (vectorized) execution path for
 * ungrouped aggregate queries. Rows are decoded into columnar batches; a scan
 * operator pushes each batch downstream to an optional filter (which narrows
 * the batch's selection vector) and then to an aggregator. This is the
 * execution model used by analytical engines (e.g. Photon); on this row-major
 * store it primarily demonstrates the batch/push dataflow rather than a raw
 * speedup, and it falls back to the row-at-a-time engine for anything it does
 * not support.
 */

/* One column-major block of rows plus a selection vector of live row indices. */
struct Batch {
    std::vector<std::vector<Value>> columns;
    std::vector<std::uint32_t> selection;
};

/* A conjunction (AND) of simple  column <op> literal  comparisons. */
struct VecPredicate {
    struct Term {
        int column = -1;
        parser::ComparisonOp op = parser::ComparisonOp::Eq;
        Value literal;
    };
    std::vector<Term> terms;
};

struct VecAggregate {
    enum class Kind { CountStar, Count, Sum, Avg, Min, Max };
    Kind kind = Kind::CountStar;
    int column = -1;
};

/* Downstream consumer of pushed batches. */
class BatchSink {
public:
    virtual ~BatchSink() = default;
    virtual void consume(Batch& batch) = 0;
    virtual void finish() = 0;
};

/*
 * Run a vectorized aggregate over a single table. Returns one Value per
 * aggregate, matching the semantics of the row-at-a-time aggregator.
 */
std::vector<Value> runVectorizedAggregate(StorageEngine& storage, int tableId,
                                          const Schema& schema,
                                          const std::optional<VecPredicate>& predicate,
                                          const std::vector<VecAggregate>& aggregates,
                                          std::size_t batchSize = 1024);

}
