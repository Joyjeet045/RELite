#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "frontend/catalog.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semantic_analyzer.hpp"
#include "vm/column_store.hpp"
#include "vm/executor_engine.hpp"
#include "vm/storage_engine.hpp"

using namespace db;

namespace {

vm::ResultSet exec(vm::StorageEngine& se, const std::string& sql) {
    parser::Lexer lexer(sql);
    parser::Parser parser(lexer.tokenize());
    auto stmt = parser.parseStatement();
    semantic::SemanticAnalyzer analyzer(semantic::Catalog::instance());
    analyzer.analyze(*stmt);
    vm::ExecutorEngine engine(se, semantic::Catalog::instance());
    return engine.run(*stmt);
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

/*
 * Verifies columnar aggregation (used by the vectorized aggregate path): the
 * cached typed columns match the table, aggregates over them match the row
 * engine's semantics (null handling included), predicates work, and writes
 * invalidate the cache.
 */
void run() {
    semantic::Catalog::instance().reset();
    vm::StorageEngine se("relite_test_colstore.db", /*truncate=*/true);

    exec(se, "BUILD RELATION t (id INT, cat TEXT, amt FLOAT);");
    exec(se,
         "PUT INTO t VALUES (1,'a',10.0),(2,'a',20.0),(3,'b',30.0),"
         "(4,'b',NULL),(5,'a',5.0);");

    /* SQL aggregates route through the columnar path. */
    auto r = exec(se,
                  "FETCH COUNT(*), COUNT(amt), SUM(amt), MIN(amt), MAX(amt), "
                  "AVG(amt) FROM t;");
    assert(r.rows.size() == 1);
    assert(r.rows[0][0].intValue == 5);
    assert(r.rows[0][1].intValue == 4);
    assert(near(r.rows[0][2].doubleValue, 65.0));
    assert(near(r.rows[0][3].doubleValue, 5.0));
    assert(near(r.rows[0][4].doubleValue, 30.0));
    assert(near(r.rows[0][5].doubleValue, 16.25));

    auto rf = exec(se, "FETCH COUNT(*), SUM(amt) FROM t WHEN cat = 'a';");
    assert(rf.rows[0][0].intValue == 3 && near(rf.rows[0][1].doubleValue, 35.0));

    /* Direct columnar API: typed columns + null bitmap. */
    const semantic::TableSchema* ts = semantic::Catalog::instance().getTable("t");
    vm::Schema schema;
    for (const auto& c : ts->columns) schema.push_back(c.type);
    const vm::TableColumns& tc =
        se.columns().getOrBuild(ts->tableId, schema, se.tables());
    assert(tc.rows == 5);
    assert(tc.columns[0].type == parser::DataType::Int);
    assert(tc.columns[0].ints[0] == 1);
    assert(tc.columns[1].texts[2] == "b");
    assert(tc.columns[2].isNull[3] == 1);
    assert(tc.columns[2].isNull[0] == 0);

    std::vector<vm::VecAggregate> aggs = {
        {vm::VecAggregate::Kind::CountStar, -1},
        {vm::VecAggregate::Kind::Sum, 2},
    };
    auto out = vm::columnarAggregate(tc, aggs, std::nullopt);
    assert(out[0].intValue == 5 && near(out[1].doubleValue, 65.0));

    /* A write invalidates the cache; the rebuild reflects the new row. */
    exec(se, "PUT INTO t VALUES (6,'a',100.0);");
    const vm::TableColumns& tc2 =
        se.columns().getOrBuild(ts->tableId, schema, se.tables());
    assert(tc2.rows == 6);
    auto out2 = vm::columnarAggregate(tc2, {{vm::VecAggregate::Kind::Sum, 2}},
                                      std::nullopt);
    assert(near(out2[0].doubleValue, 165.0));

    /* Data skipping: zone maps prune whole blocks a predicate cannot satisfy. */
    {
        exec(se, "BUILD RELATION big (id INT);");
        const int N = 2500;
        std::string put = "PUT INTO big VALUES ";
        for (int i = 0; i < N; ++i) {
            if (i) put += ",";
            put += "(" + std::to_string(i) + ")";
        }
        put += ";";
        exec(se, put);

        const semantic::TableSchema* bt = semantic::Catalog::instance().getTable("big");
        vm::Schema bs;
        for (const auto& c : bt->columns) bs.push_back(c.type);
        const vm::TableColumns& btc =
            se.columns().getOrBuild(bt->tableId, bs, se.tables());
        assert(btc.rows == static_cast<std::size_t>(N));
        /* 2500 rows / 1024 per block = 3 blocks. */
        assert(btc.columns[0].zones.size() == 3);

        vm::VecPredicate pred;
        pred.terms.push_back({0, parser::ComparisonOp::Geq, vm::Value::makeInt(2100)});
        vm::SkipStats stats;
        auto cnt = vm::columnarAggregate(
            btc, {{vm::VecAggregate::Kind::CountStar, -1}}, pred, &stats);
        assert(cnt[0].intValue == 400);       /* ids 2100..2499 */
        assert(stats.blocksTotal == 3);
        assert(stats.blocksSkipped == 2);     /* blocks 0 and 1 pruned */

        /* Result still matches the row engine end to end. */
        auto viaSql = exec(se, "FETCH COUNT(*) FROM big WHEN id >= 2100;");
        assert(viaSql.rows[0][0].intValue == 400);
    }

    /* Morsel-driven parallel aggregation matches the serial result. */
    {
        const semantic::TableSchema* bt = semantic::Catalog::instance().getTable("big");
        vm::Schema bs;
        for (const auto& c : bt->columns) bs.push_back(c.type);
        const vm::TableColumns& btc =
            se.columns().getOrBuild(bt->tableId, bs, se.tables());

        std::vector<vm::VecAggregate> aggs = {
            {vm::VecAggregate::Kind::CountStar, -1},
            {vm::VecAggregate::Kind::Sum, 0},
            {vm::VecAggregate::Kind::Min, 0},
            {vm::VecAggregate::Kind::Max, 0},
        };
        auto serial = vm::columnarAggregate(btc, aggs, std::nullopt);
        auto par = vm::parallelColumnarAggregate(btc, aggs, std::nullopt, 4);
        assert(par.size() == serial.size());
        assert(par[0].intValue == serial[0].intValue && par[0].intValue == 2500);
        assert(par[1].intValue == serial[1].intValue && par[1].intValue == 3123750);
        assert(par[2].intValue == serial[2].intValue && par[2].intValue == 0);
        assert(par[3].intValue == serial[3].intValue && par[3].intValue == 2499);

        /* Parallel workers honor data skipping too. */
        vm::VecPredicate pred;
        pred.terms.push_back({0, parser::ComparisonOp::Geq, vm::Value::makeInt(2100)});
        vm::SkipStats st;
        auto parp = vm::parallelColumnarAggregate(
            btc, {{vm::VecAggregate::Kind::CountStar, -1}}, pred, 4, &st);
        assert(parp[0].intValue == 400);
        assert(st.blocksSkipped == 2);
    }

    semantic::Catalog::instance().reset();
    std::remove("relite_test_colstore.db");
    std::cout << "column_store_test passed\n";
}

}  // namespace

int main() {
    run();
    return 0;
}
