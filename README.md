# PRQLite

A relational database built from scratch in modern C++ (C++20). PRQLite implements the
full stack of a small single-node RDBMS: a front-end for its **own query language**
(PRQLite QL, not SQL), a Volcano-model execution engine, a paged storage engine with a
buffer pool, B+ tree indexing, and ACID transactions with write-ahead logging and crash
recovery.

It runs as an interactive REPL and persists data across restarts.

## Features

**Query language (PRQLite QL, not SQL)**
- Definitions: `BUILD RELATION` / `BUILD INDEX`, `DISCARD RELATION` / `DISCARD INDEX`,
  `RESHAPE RELATION ADD/DISCARD COLUMN`
- Data changes: `PUT INTO`, `MODIFY`, `REMOVE`
- Queries: `FETCH` with projection, `WHEN` (`=, !=, <, <=, >, >=`, `AND/OR/NOT`,
  `IS [NOT] NULL`, `[NOT] IN`, `BETWEEN`, `LIKE`), `LINK ... ON` (inner join),
  `GROUP BY`/`HAVING`, aggregates (`COUNT/SUM/AVG/MIN/MAX`), `UNIQUEONLY` /
  `COUNT(UNIQUEONLY ...)`, `SORT BY`, `TAKE`, and uncorrelated scalar/`IN`/`EXISTS`
  subqueries
- Constraints: `PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `DEFAULT`, `CHECK`, foreign keys
  (`REFERENCES`), and `VARCHAR(n)` length enforcement
- Transactions: `START` / `SAVE` / `UNDO`

The full keyword vocabulary and SQL-to-PRQLite mapping are in
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
./build/prqlite        # (build/prqlite.exe on Windows)
```

```
prqlite=# BUILD RELATION users (id INT PRIMARY KEY, name VARCHAR(16) NOT NULL);
prqlite=# PUT INTO users VALUES (1, 'ann');
prqlite=# FETCH * FROM users;
```

Type `\h` for help and `\q` to quit.

## Tests

Ten self-contained suites run via CTest:

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

The accepted query language (and its SQL-to-PRQLite keyword mapping) is documented in
[docs/grammar.txt](docs/grammar.txt). The authoritative grammar is the recursive-descent
parser in `src/frontend/parser.cpp`.

## Roadmap

Larger items not yet implemented: ARIES-style redo + `pageLSN`, MVCC / isolation levels,
a paged (disk-backed) B+ tree, a cost-based optimizer with merge join and external-sort
spill, outer/multi-way joins, correlated subqueries, and a `FLOAT` column type.
