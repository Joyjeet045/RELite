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

    semantic::Catalog::instance().reset();
    std::cout << "vectorized_test passed\n";
}

}  // namespace

int main() {
    run();
    return 0;
}
