# Relite

[![CI](https://github.com/Joyjeet045/Relite/actions/workflows/ci.yml/badge.svg)](https://github.com/Joyjeet045/Relite/actions/workflows/ci.yml)

A relational database built from scratch in modern C++ (C++20). Relite implements the
full stack of a small single-node RDBMS: a front-end for its **own query language**
(Relite QL, not SQL), a Volcano-model execution engine, a paged storage engine with a
buffer pool, B+ tree indexing, and ACID transactions with write-ahead logging and crash
recovery.

It runs as an interactive REPL and persists data across restarts.

## Features

**Query language (Relite QL, not SQL)**
- Definitions: `BUILD RELATION` / `BUILD INDEX` (single- or multi-column) / `BUILD VIEW`,
  `DISCARD RELATION` / `DISCARD INDEX`, `RESHAPE RELATION ADD/DISCARD COLUMN`
- Data changes: `PUT INTO` (including `PUT INTO t FETCH ...`), `MODIFY`, `REMOVE`
- Queries: `FETCH` with projection (arithmetic such as `price * qty`, column/table
  aliases via `AS`), `WHEN` (`=, !=, <, <=, >, >=`, `+ - * /`, `AND/OR/NOT`,
  `IS [NOT] NULL`, `[NOT] IN`, `BETWEEN`, `LIKE`), inner/`LEFT`/`RIGHT`/`FULL`/`CROSS`
  `LINK` joins (chained for 3+ tables), `GROUP BY`/`HAVING` (including aggregates in
  `HAVING`), aggregates (`COUNT/SUM/AVG/MIN/MAX`), `UNIQUEONLY`, `SORT BY`,
  `TAKE ... SKIP` (limit/offset), and correlated or uncorrelated scalar/`IN`/`EXISTS`
  subqueries (in `WHEN`, the `FETCH` list, and `MODIFY`/`REMOVE`)
- Window functions: `fn() OVER (PARTITION BY ... SORT BY ...)` with `ROW_NUMBER`,
  `RANK`, `DENSE_RANK`, and partition-total `SUM/COUNT/AVG/MIN/MAX`
- Scalar functions & expressions: `UPPER/LOWER/LENGTH/SUBSTR/TRIM`, `ABS/ROUND/MOD/CEIL/FLOOR`,
  `COALESCE/NULLIF`, `CAST(x AS type)`, and `CASE WHEN ... THEN ... ELSE ... END`
- Set operations: `UNION` / `UNION ALL` / `INTERSECT` / `EXCEPT`
- `EXPLAIN FETCH ...` prints the query plan tree (scan/index, join algorithm, sort, limit)
- Column types: `INT` (aka `BIGINT`/`SMALLINT`), `FLOAT` (aka `DOUBLE`/`REAL`),
  `DECIMAL(p,s)`/`NUMERIC`, `BOOL`, `TEXT`, `VARCHAR(n)`, `DATE`, `TIMESTAMP`
- Constraints: `PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `DEFAULT`, `CHECK`, `AUTO_INCREMENT`,
  foreign keys (`REFERENCES ... ON REMOVE CASCADE / SET NULL / RESTRICT`), and
  `VARCHAR(n)` length enforcement
- Views: `BUILD VIEW v AS FETCH ...` (materialized on read)
- Transactions: `START` / `SAVE` / `UNDO`

The full keyword vocabulary and SQL-to-Relite mapping are in
[docs/grammar.txt](docs/grammar.txt).

**Engine internals**
- Hand-written lexer + recursive-descent parser (precedence climbing) → AST (visitor pattern)
- Semantic analyzer binding names/types against a per-database catalog
- Volcano/iterator execution engine with a first-pass optimizer (index range scans) and hash join
- 4 KB slotted pages, disk manager, LRU buffer pool with page guards, page compaction
- Thread-safe B+ tree + Bloom filter indexes
- Write-ahead log with `fsync` durability, group-commit, checkpointing, and crash recovery
- Row-level lock manager (two-phase locking) and a transaction manager with undo

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

Eleven self-contained suites run via CTest:

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
docs/         grammar.txt (SQL grammar reference)
```

## Grammar

The accepted query language (and its SQL-to-Relite keyword mapping) is documented in
[docs/grammar.txt](docs/grammar.txt). The authoritative grammar is the recursive-descent
parser in `src/frontend/parser.cpp`.

## Benchmarks

A self-contained harness (`tools/benchmark.cpp`) drives the same
lexer → parser → analyzer → executor pipeline the REPL uses, against a real
storage engine, WAL, and lock manager.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target relite_bench
./build-release/relite_bench
```

Representative results (Release build, single thread, warm cache, commodity
laptop; 50K-row table):

| Workload                          | Throughput      | Latency        |
| --------------------------------- | --------------- | -------------- |
| Insert (single-row statements)    | ~215K rows/s    | ~4.6 µs/row    |
| Point lookup (B+ tree index)      | ~120K lookups/s | ~8.4 µs/lookup |
| Scan + aggregate (`COUNT`/`SUM`)  | ~1.7M rows/s    | ~29 ms / 50K   |
| Durable commit (`fsync` per txn)  | ~1.3K commits/s | ~0.77 ms/commit |

Insert/lookup figures are end-to-end (they include SQL parsing and planning on
every statement); commit throughput is bounded by `fsync`. Numbers vary with
hardware and dataset size — run the harness locally for your own baseline.

## Roadmap

Larger items not yet implemented: ARIES-style redo + `pageLSN`, MVCC / isolation levels,
a paged (disk-backed) B+ tree, and a cost-based optimizer with merge join and
external-sort spill.
