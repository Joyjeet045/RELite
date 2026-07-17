# Relite

Relite is a single-node relational database implemented from scratch in C++20. It
provides a complete database stack: a query language with a hand-written front end, a
Volcano-model execution engine, a cost-based optimizer, a paged storage engine with a
buffer pool, disk-backed B+ tree indexing, and ACID transactions with write-ahead
logging and crash recovery. It runs as an interactive REPL and persists data across
restarts.

## Features

### Data definition
- Tables: `BUILD RELATION`, and `BUILD RELATION t AS FETCH ...` to build a table from a
  query result
- Indexes: `BUILD INDEX` on one or more columns
- Views: `BUILD VIEW v AS FETCH ...`, with the definition persisted across restarts and
  materialized on read
- Schema changes: `RESHAPE RELATION ADD COLUMN` and `DISCARD COLUMN`
- Removal: `DISCARD RELATION` / `DISCARD INDEX` / `DISCARD VIEW`, and `TRUNCATE RELATION`
- Column types: `INT` (`BIGINT`, `SMALLINT`), `FLOAT` (`DOUBLE`, `REAL`),
  `DECIMAL(p,s)` / `NUMERIC`, `BOOL`, `TEXT`, `VARCHAR(n)`, `DATE`, `TIMESTAMP`
- Constraints: `PRIMARY KEY` (single-column and composite), `UNIQUE`, `NOT NULL`,
  `DEFAULT`, `CHECK`, `AUTO_INCREMENT`, `VARCHAR(n)` length enforcement, and foreign
  keys with `REFERENCES ... ON REMOVE` and `ON MODIFY` actions (`CASCADE`, `SET NULL`,
  `RESTRICT`)

### Data manipulation
- `PUT INTO`: multi-row `VALUES`, `DEFAULT VALUES`, and `PUT INTO t FETCH ...` to insert
  from a query
- Conflict handling: `PUT INTO t VALUES (...) ON CONFLICT (col) DO MODIFY SET ...` or
  `DO NOTHING`
- `MODIFY` with `SET` assignments, including `MODIFY t SET ... FROM u WHEN ...` to update
  a table joined against another
- `REMOVE` with `WHEN` predicates
- `RETURNING` on `PUT`, `MODIFY`, and `REMOVE` to return the affected rows

### Queries
- `FETCH` with projection, arithmetic such as `price * qty`, and column or table aliases
  via `AS`
- `WHEN` predicates: comparisons, arithmetic, `AND` / `OR` / `NOT`, `IS [NOT] NULL`,
  `[NOT] IN`, `BETWEEN`, `LIKE`
- Joins: inner, `LEFT`, `RIGHT`, `FULL`, and `CROSS` via `LINK`, chained across three or
  more tables
- Grouping: `GROUP BY`, `HAVING` (aggregates permitted), and
  `COUNT` / `SUM` / `AVG` / `MIN` / `MAX`
- `UNIQUEONLY` (distinct), `SORT BY`, and `TAKE ... SKIP` (limit and offset)
- Subqueries: scalar, `IN`, and `EXISTS`, correlated or uncorrelated, usable in `WHEN`,
  the `FETCH` list, and `MODIFY` / `REMOVE`
- Window functions: `ROW_NUMBER`, `RANK`, `DENSE_RANK`, and
  `SUM` / `COUNT` / `AVG` / `MIN` / `MAX` `OVER (PARTITION BY ... SORT BY ...)`
- Scalar functions: `UPPER`, `LOWER`, `LENGTH`, `SUBSTR`, `TRIM`, `ABS`, `ROUND`, `MOD`,
  `CEIL`, `FLOOR`, `COALESCE`, `NULLIF`, `CAST(x AS type)`, and
  `CASE WHEN ... THEN ... ELSE ... END`
- Set operations: `UNION`, `UNION ALL`, `INTERSECT`, `EXCEPT`
- `EXPLAIN FETCH ...` prints the query plan tree, including the chosen scan and join
  algorithms, sort, and limit

### Transactions and isolation
- `START` / `SAVE` / `UNDO` for begin, commit, and rollback
- Snapshot isolation: a transaction reads a stable snapshot of committed state together
  with its own writes
- Time travel: `FETCH ... FROM t AS OF <version>` reads a table at a past logical
  version; every committed write advances the version

## Architecture

- Front end: a hand-written lexer and recursive-descent parser (precedence climbing)
  producing an AST that is traversed with the visitor pattern, and a semantic analyzer
  that binds names and types against a per-database catalog
- Optimization: rule-based rewrites run before execution (constant folding, boolean
  short-circuiting, and arithmetic identities), and a cost-based optimizer selects index
  range scans and, by cardinality, between hash join and sort-merge join. The sort-merge
  path uses an external merge sort that spills to disk, and `EXPLAIN` reports the chosen
  plan
- Execution: a Volcano/iterator engine over the row store, with the vectorized operators
  below for analytical aggregates
- Vectorized execution and data skipping: a push-based, batch-at-a-time operator set
  (filter, aggregate, hash GROUP BY, and hash join) over column batches with selection
  vectors. Ungrouped aggregates run over a cached typed columnar store of contiguous
  primitive arrays with null bitmaps, and per-block zone maps (min/max) let a predicate
  skip whole blocks it cannot satisfy. Large aggregate scans are parallelized
  morsel-by-morsel across worker threads, each folding its blocks into a private
  accumulator before a final merge. `EXPLAIN` marks the aggregate path as
  `Aggregate (Vectorized)` or `Aggregate (Vectorized, Data Skipping)`
- Storage: 4 KB slotted pages, a disk manager, and an LRU buffer pool with page guards
  and page compaction
- Indexing: page-resident, disk-backed B+ tree indexes whose nodes live in buffer-pool
  pages and are evictable, together with Bloom filters
- Durability: a write-ahead log with `fsync`, group commit, and checkpointing. Recovery
  follows ARIES (analysis, redo, undo) with per-page LSNs and idempotent redo, so
  committed work survives under a no-force buffer policy
- Concurrency: a row-level lock manager (two-phase locking, shared and exclusive)
  exercised under real threads, with a transaction manager that maintains undo
- Multiversion store: snapshot reads and `AS OF` time travel are served from a version
  store kept beside the heap. History is garbage-collected into per-table baselines and
  persisted across restarts

## Build

Requires CMake ≥ 3.16 and a C++20 compiler.

```sh
cmake -S . -B build
cmake --build build
```

## Run

```sh
./build/relite        # (build/relite.exe on Windows)
```

```
relite=# BUILD RELATION users (id INT PRIMARY KEY, name VARCHAR(16) NOT NULL);
relite=# PUT INTO users VALUES (1, 'ann');
relite=# FETCH * FROM users;
```

Type `\h` for help and `\q` to quit.

## Tests

Seventeen self-contained suites run via CTest:

```sh
ctest --test-dir build --output-on-failure
```

## Project layout

```
include/ , src/
  frontend/   lexer, parser, AST, catalog, semantic analyzer, db/REPL
  vm/         tuple, values, table manager, executor + engine (Volcano)
  backend/    page, disk manager, buffer pool, durability
  index/      B+ tree, bloom filter, index manager
  txn/        WAL, lock manager, transaction manager
tests/        unit + integration suites
docs/         grammar.txt (grammar reference)
```

## Grammar

The accepted grammar is documented in [docs/grammar.txt](docs/grammar.txt). The
authoritative definition is the recursive-descent parser in
`src/frontend/parser.cpp`.

## Benchmarks

A self-contained harness (`tools/benchmark.cpp`) drives the same
lexer, parser, analyzer, and executor pipeline the REPL uses, against a real
storage engine, WAL, and lock manager.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target relite_bench
./build-release/relite_bench
```

Representative results (Release build, single thread, otherwise-idle commodity
laptop; 50K-row table):

| Workload                          | Throughput      | Latency        |
| --------------------------------- | --------------- | -------------- |
| Insert (single-row statements)    | ~180K rows/s    | ~5.5 µs/row    |
| Point lookup (disk B+ tree index) | ~30K lookups/s  | ~35 µs/lookup  |
| Scan + aggregate (`COUNT`/`SUM`)  | ~15M+ rows/s    | ~2 ms / 50K    |
| Durable commit (`fsync` per txn)  | ~1K commits/s   | ~1 ms/commit   |

Insert and lookup figures are end-to-end. They include parsing and planning on every
statement, plus multiversion bookkeeping for time travel, and point lookups go through
the page-resident B+ tree. The scan and aggregate figure uses the columnar vectorized
path: the first scan builds a typed column cache, and subsequent aggregates run as tight
loops over primitive arrays, roughly an order of magnitude faster than the row path.
Commit throughput is bounded by `fsync`. Throughput is sensitive to background load and
dataset size, so run the harness locally for your own baseline.

## Roadmap

Larger items not yet implemented: serializable isolation with predicate locking,
and an on-disk (rather than cached in-memory) columnar format.
