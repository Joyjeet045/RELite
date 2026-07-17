#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
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
#include "vm/vectorized.hpp"

using namespace db;

namespace {

/*
 * Exercises the vectorized (batch/push) aggregate path both through the SQL
 * executor and directly, comparing every result against expectations computed
 * independently in this test over the known input data.
 */
struct Harness {
    vm::StorageEngine se;
    txn::WriteAheadLog wal;
    txn::LockManager locks;
    txn::TransactionManager tm;
    int cur = 0;

    Harness(const std::string& db, const std::string& wl)
        : se(db, true), wal(wl, true), locks(), tm(&wal, &locks) {}

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

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

struct Row {
    long long x;
    bool yNull;
    double y;
};

void run() {
    semantic::Catalog::instance().reset();
    Harness h("relite_test_vec.db", "relite_test_vec.wal");

    h.run("BUILD RELATION nums (id INT, x INT, y FLOAT);");

    /* Deterministic pseudo-random data with some NULL y values. */
    std::vector<Row> data;
    std::uint64_t seed = 88172645463325252ULL;
    auto nextRand = [&]() {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };

    std::string insert = "PUT INTO nums VALUES ";
    const int n = 400;
    for (int i = 0; i < n; ++i) {
        long long x = static_cast<long long>(nextRand() % 1000);
        bool yNull = (nextRand() % 5 == 0);
        double y = static_cast<double>(nextRand() % 10000) / 100.0;
        data.push_back({x, yNull, y});
        if (i) insert += ',';
        insert += "(" + std::to_string(i) + "," + std::to_string(x) + "," +
                  (yNull ? std::string("NULL") : std::to_string(y)) + ")";
    }
    insert += ';';
    h.run(insert);

    auto expectInt = [&](long long threshold) {
        long long count = 0, csum = 0;
        long long cmin = 0, cmax = 0;
        bool any = false;
        for (const Row& r : data) {
            if (!(r.x < threshold)) continue;
            ++count;
            csum += r.x;
            if (!any) {
                cmin = cmax = r.x;
                any = true;
            } else {
                cmin = std::min(cmin, r.x);
                cmax = std::max(cmax, r.x);
            }
        }
        return std::make_tuple(count, csum, cmin, cmax, any);
    };

    for (long long threshold : {-5LL, 100LL, 500LL, 1000LL, 2000LL}) {
        auto [count, csum, cmin, cmax, any] = expectInt(threshold);
        std::string w = " WHEN x < " + std::to_string(threshold);

        auto rc = h.run("FETCH COUNT(*) FROM nums" + w + ";");
        auto rs = h.run("FETCH SUM(x) FROM nums" + w + ";");
        auto rmm = h.run("FETCH MIN(x), MAX(x) FROM nums" + w + ";");
        auto ra = h.run("FETCH AVG(x) FROM nums" + w + ";");

        assert(rc.rows[0][0].intValue == count);
        if (any) {
            assert(rs.rows[0][0].intValue == csum);
            assert(rmm.rows[0][0].intValue == cmin);
            assert(rmm.rows[0][1].intValue == cmax);
            assert(near(ra.rows[0][0].doubleValue,
                        static_cast<double>(csum) / static_cast<double>(count)));
        } else {
            assert(rs.rows[0][0].isNull());
            assert(rmm.rows[0][0].isNull() && rmm.rows[0][1].isNull());
            assert(ra.rows[0][0].isNull());
        }
    }

    /* FLOAT column with NULLs: SUM/AVG/COUNT must skip nulls. */
    {
        double ysum = 0.0;
        long long ycount = 0;
        for (const Row& r : data) {
            if (r.yNull) continue;
            ysum += r.y;
            ++ycount;
        }
        auto rs = h.run("FETCH SUM(y), COUNT(y), AVG(y) FROM nums;");
        assert(near(rs.rows[0][0].doubleValue, ysum));
        assert(rs.rows[0][1].intValue == ycount);
        assert(near(rs.rows[0][2].doubleValue, ysum / static_cast<double>(ycount)));
    }

    /* Direct call into the vectorized module (bypassing SQL). */
    {
        const semantic::TableSchema* ts = semantic::Catalog::instance().getTable("nums");
        assert(ts != nullptr);
        vm::Schema schema;
        for (const auto& c : ts->columns) schema.push_back(c.type);

        std::vector<vm::VecAggregate> aggs = {
            {vm::VecAggregate::Kind::CountStar, -1},
            {vm::VecAggregate::Kind::Sum, 1},
        };
        vm::VecPredicate pred;
        pred.terms.push_back({1, parser::ComparisonOp::Lt, vm::Value::makeInt(500)});

        auto out = vm::runVectorizedAggregate(h.se, ts->tableId, schema, pred, aggs);
        auto [count, csum, cmin, cmax, any] = expectInt(500);
        (void)cmin;
        (void)cmax;
        assert(out[0].intValue == count);
        assert(out[1].intValue == csum);
        (void)any;
    }

    /* Vectorized hash GROUP BY: per-group aggregates folded over batches. */
    {
        h.run("BUILD RELATION grp (k INT, v INT);");
        h.run("PUT INTO grp VALUES (1,10),(1,20),(2,5),(2,7),(2,3),(3,100);");
        const semantic::TableSchema* gt = semantic::Catalog::instance().getTable("grp");
        vm::Schema gs;
        for (const auto& c : gt->columns) gs.push_back(c.type);

        std::vector<vm::VecAggregate> aggs = {
            {vm::VecAggregate::Kind::CountStar, -1},
            {vm::VecAggregate::Kind::Sum, 1},
        };
        auto groups = vm::runVectorizedGroupAggregate(h.se, gt->tableId, gs,
                                                      std::nullopt, {0}, aggs);
        /* First-seen group order: k = 1, 2, 3; row = [k, count, sum]. */
        assert(groups.size() == 3);
        assert(groups[0][0].intValue == 1 && groups[0][1].intValue == 2 &&
               groups[0][2].intValue == 30);
        assert(groups[1][0].intValue == 2 && groups[1][1].intValue == 3 &&
               groups[1][2].intValue == 15);
        assert(groups[2][0].intValue == 3 && groups[2][1].intValue == 1 &&
               groups[2][2].intValue == 100);
    }

    /* Vectorized hash join: build over one table, probe the other in batches. */
    {
        h.run("BUILD RELATION jl (id INT, name TEXT);");
        h.run("BUILD RELATION jr (id INT, score INT);");
        h.run("PUT INTO jl VALUES (1,'a'),(2,'b'),(3,'c');");
        h.run("PUT INTO jr VALUES (2,20),(3,30),(3,35),(9,99);");
        const semantic::TableSchema* lt = semantic::Catalog::instance().getTable("jl");
        const semantic::TableSchema* rt = semantic::Catalog::instance().getTable("jr");
        vm::Schema ls, rs2;
        for (const auto& c : lt->columns) ls.push_back(c.type);
        for (const auto& c : rt->columns) rs2.push_back(c.type);

        /* Build on jl(id), probe with jr(id); row = jr cols ++ jl cols. */
        auto joined = vm::runVectorizedHashJoin(h.se, lt->tableId, ls, 0,
                                                rt->tableId, rs2, 0);
        assert(joined.size() == 3);
        assert(joined[0][0].intValue == 2 && joined[0][1].intValue == 20 &&
               joined[0][2].intValue == 2 && joined[0][3].textValue == "b");
        assert(joined[1][0].intValue == 3 && joined[1][1].intValue == 30 &&
               joined[1][3].textValue == "c");
        assert(joined[2][0].intValue == 3 && joined[2][1].intValue == 35 &&
               joined[2][3].textValue == "c");
    }

    semantic::Catalog::instance().reset();
    std::cout << "vectorized_test passed\n";
}

}  // namespace

int main() {
    run();
    return 0;
}
