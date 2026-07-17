#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "frontend/ast.hpp"
#include "vm/tuple.hpp"
#include "vm/value.hpp"
#include "vm/vectorized.hpp"

namespace db::vm {

class TableManager;

/*
 * A cached, read-optimized columnar copy of a table. Each column is a
 * contiguous typed array plus a null bitmap, so analytical scans touch only the
 * columns they need and vectorized aggregation runs as tight loops over
 * primitive arrays (no per-row tuple decoding). The copy is built lazily from
 * the row heap and invalidated on write, so it accelerates repeated aggregates
 * over an unchanged table.
 */
struct ColumnZone {
    bool hasNonNull = false;
    Value min;
    Value max;
};

struct Column {
    parser::DataType type = parser::DataType::Int;
    std::vector<std::int64_t> ints;
    std::vector<double> doubles;
    std::vector<std::uint8_t> bools;
    std::vector<std::string> texts;
    std::vector<std::uint8_t> isNull;
    /* One min/max summary per fixed-size block of rows (a "zone map"), used to
     * skip whole blocks that cannot satisfy a predicate (Delta-style data
     * skipping). */
    std::vector<ColumnZone> zones;
};

struct TableColumns {
    std::vector<Column> columns;
    std::size_t rows = 0;

    /* Rows per zone-map block. */
    static constexpr std::size_t kBlockRows = 1024;
};

/* Number of blocks scanned vs. skipped by data skipping, for EXPLAIN and tests. */
struct SkipStats {
    std::size_t blocksTotal = 0;
    std::size_t blocksSkipped = 0;
};

class ColumnStore {
public:
    void invalidate(int tableId);
    void clear() { cache_.clear(); }

    const TableColumns& getOrBuild(int tableId, const Schema& schema,
                                   TableManager& tables);

private:
    std::unordered_map<int, TableColumns> cache_;
};

/* Columnar aggregate over a built table: one Value per aggregate, matching the
 * row engine's semantics. When a predicate is present, whole blocks whose zone
 * maps cannot satisfy it are skipped; if stats is non-null it receives the
 * block scan/skip counts. */
std::vector<Value> columnarAggregate(const TableColumns& table,
                                     const std::vector<VecAggregate>& aggregates,
                                     const std::optional<VecPredicate>& predicate,
                                     SkipStats* stats = nullptr);

/* Morsel-driven parallel version of columnarAggregate: blocks are striped across
 * worker threads, each folding its blocks into a private partial accumulator,
 * after which the partials are merged. Integer aggregates match the serial
 * result exactly; floating-point sums may differ only by summation order. */
std::vector<Value> parallelColumnarAggregate(
    const TableColumns& table, const std::vector<VecAggregate>& aggregates,
    const std::optional<VecPredicate>& predicate, unsigned numThreads,
    SkipStats* stats = nullptr);

}
