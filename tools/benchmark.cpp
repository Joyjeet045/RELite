/*
 * Relite micro-benchmark. Drives the full lexer -> parser -> analyzer ->
 * executor pipeline against a real storage engine, WAL, and lock manager,
 * the same path the REPL uses. Reports throughput for bulk inserts, indexed
 * point lookups, full-table scans, and durable (fsync) commits.
 *
 * This target is intentionally NOT registered with CTest; it is an opt-in
 * measurement tool. Build in Release for representative numbers.
 */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "frontend/catalog.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semantic_analyzer.hpp"
#include "txn/lock_manager.hpp"
#include "txn/transaction_manager.hpp"
#include "txn/wal.hpp"
#include "vm/executor_engine.hpp"
#include "vm/storage_engine.hpp"

using namespace db;
using Clock = std::chrono::steady_clock;

namespace {

double seconds(Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::duration<double>>(d).count();
}

struct Engine {
    vm::StorageEngine se;
    txn::WriteAheadLog wal;
    txn::LockManager locks;
    txn::TransactionManager tm;
    int cur = 0;

    Engine()
        : se("relite_bench.db", true), wal("relite_bench.wal", true), locks(),
          tm(&wal, &locks) {
        se.bufferPool().setPreEvictHook([this] { wal.flush(); });
    }

    vm::ResultSet run(const std::string& sql) {
        parser::Lexer lexer(sql);
        parser::Parser parser(lexer.tokenize());
        auto stmt = parser.parseStatement();
        semantic::SemanticAnalyzer analyzer(semantic::Catalog::instance());
        analyzer.analyze(*stmt);
        vm::ExecutorEngine engine(se, semantic::Catalog::instance(), &tm, &cur);
        return engine.run(*stmt);
    }
};

void line() { std::printf("  %-26s %14s  %14s\n", "", "", ""); }

}  // namespace

int main() {
    semantic::Catalog::instance().reset();

    const int kInserts = 50000;
    const int kLookups = 50000;
    const int kScans = 20;
    const int kCommits = 5000;

#ifdef NDEBUG
    const char* buildKind = "Release (NDEBUG)";
#else
    const char* buildKind = "Debug (asserts on)";
#endif

    std::printf("Relite benchmark  [build: %s]\n", buildKind);
    std::printf("%s\n", "-----------------------------------------------------------");
    std::printf("  %-26s %14s  %14s\n", "workload", "throughput", "latency");
    line();

    Engine eng;
    eng.run("BUILD RELATION bench (id INT, val INT, name TEXT);");

    /* 1. Insert throughput: full parse + plan + execute per single-row statement. */
    {
        auto t0 = Clock::now();
        for (int i = 0; i < kInserts; ++i) {
            std::string sql = "PUT INTO bench VALUES (" + std::to_string(i) + ", " +
                              std::to_string((i * 2654435761u) % 100000u) +
                              ", 'row');";
            eng.run(sql);
        }
        double s = seconds(Clock::now() - t0);
        std::printf("  %-26s %10.0f/s  %11.2f us\n", "insert (single-row stmt)",
                    kInserts / s, s / kInserts * 1e6);
    }

    /* 2. Indexed point lookups. */
    eng.run("BUILD INDEX bench_id ON bench (id);");
    {
        std::mt19937 rng(12345);
        std::uniform_int_distribution<int> pick(0, kInserts - 1);
        std::uint64_t hits = 0;
        auto t0 = Clock::now();
        for (int i = 0; i < kLookups; ++i) {
            std::string sql =
                "FETCH id, val FROM bench WHEN id = " + std::to_string(pick(rng)) + ";";
            hits += eng.run(sql).rows.size();
        }
        double s = seconds(Clock::now() - t0);
        std::printf("  %-26s %10.0f/s  %11.2f us\n", "point lookup (B+ index)",
                    kLookups / s, s / kLookups * 1e6);
        (void)hits;
    }

    /* 3. Full-table scan + aggregate. */
    {
        auto t0 = Clock::now();
        for (int i = 0; i < kScans; ++i) {
            eng.run("FETCH COUNT(*), SUM(val) FROM bench;");
        }
        double s = seconds(Clock::now() - t0);
        double rowsScanned = static_cast<double>(kInserts) * kScans;
        std::printf("  %-26s %10.0f/s  %11.2f ms\n", "scan+aggregate (rows)",
                    rowsScanned / s, s / kScans * 1e3);
    }

    /* 4. Durable commit throughput (each SAVE forces an fsync of the WAL). */
    {
        auto t0 = Clock::now();
        for (int i = 0; i < kCommits; ++i) {
            eng.run("START;");
            std::string sql = "PUT INTO bench VALUES (" +
                              std::to_string(kInserts + i) + ", 0, 'txn');";
            eng.run(sql);
            eng.run("SAVE;");
        }
        double s = seconds(Clock::now() - t0);
        std::printf("  %-26s %10.0f/s  %11.2f us\n", "durable commit (fsync)",
                    kCommits / s, s / kCommits * 1e6);
    }

    std::printf("%s\n", "-----------------------------------------------------------");
    std::printf("  rows: %d inserted, %d lookups, %d scans, %d commits\n", kInserts,
                kLookups, kScans, kCommits);
    return 0;
}
