#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

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

namespace {

/*
 * Regression coverage for the arithmetic, FLOAT, and outer/cross join features.
 * Each statement runs through the full lexer -> parser -> analyzer -> executor
 * pipeline against a real storage engine, WAL, and lock manager.
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

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

void run() {
    semantic::Catalog::instance().reset();
    Harness h("relite_test_feat.db", "relite_test_feat.wal");

    h.run("BUILD RELATION items (id INT, name TEXT, price FLOAT, qty INT);");
    h.run("PUT INTO items VALUES (1,'a',9.99,3),(2,'b',19.5,2),(3,'c',-5.25,10);");

    /* Arithmetic in the projection list, with float promotion. */
    auto proj = h.run("FETCH price * qty FROM items SORT BY id;");
    assert(proj.rows.size() == 3);
    assert(near(proj.rows[0][0].doubleValue, 29.97));
    assert(near(proj.rows[1][0].doubleValue, 39.0));
    assert(near(proj.rows[2][0].doubleValue, -52.5));

    /* Integer arithmetic: division truncates toward zero. */
    auto idiv = h.run("FETCH qty / 2 FROM items SORT BY id;");
    assert(idiv.rows[0][0].intValue == 1 && idiv.rows[1][0].intValue == 1 &&
           idiv.rows[2][0].intValue == 5);

    /* Division by zero yields NULL rather than trapping. */
    auto dz = h.run("FETCH qty / 0 FROM items SORT BY id;");
    assert(dz.rows.size() == 3 && dz.rows[0][0].isNull());

    /* Float aggregates. */
    assert(near(h.run("FETCH SUM(price) FROM items;").rows[0][0].doubleValue, 24.24));
    assert(near(h.run("FETCH AVG(price) FROM items;").rows[0][0].doubleValue, 8.08));
    assert(h.run("FETCH SUM(qty) FROM items;").rows[0][0].intValue == 15);

    /* Arithmetic inside a predicate. */
    assert(h.run("FETCH name FROM items WHEN qty * 2 > 15;").rows.size() == 1);

    /* Negative literals in filters and projections. */
    assert(h.run("FETCH name FROM items WHEN price < 0;").rows.size() == 1);
    auto neg = h.run("FETCH id FROM items WHEN id = 3 - 1;");
    assert(neg.rows.size() == 1 && neg.rows[0][0].intValue == 2);

    h.run("BUILD RELATION t (x INT);");
    h.run("PUT INTO t VALUES (-5),(3);");
    assert(h.run("FETCH x FROM t WHEN x = -5;").rows.size() == 1);
    assert(h.run("FETCH x FROM t WHEN x < 0;").rows[0][0].intValue == -5);

    /* LEFT LINK null-pads the unmatched left row (item 3 has no category). */
    h.run("BUILD RELATION cat (cid INT, label TEXT);");
    h.run("PUT INTO cat VALUES (1,'A'),(2,'B');");
    auto lj = h.run(
        "FETCH items.name, cat.label FROM items LEFT LINK cat "
        "ON items.id = cat.cid SORT BY items.id;");
    assert(lj.rows.size() == 3);
    assert(lj.rows[0][1].textValue == "A");
    assert(lj.rows[2][1].isNull());

    /* CROSS LINK forms the full cartesian product. */
    h.run("BUILD RELATION colors (c TEXT);");
    h.run("PUT INTO colors VALUES ('red'),('blue');");
    h.run("BUILD RELATION sizes (s TEXT);");
    h.run("PUT INTO sizes VALUES ('S'),('M'),('L');");
    auto cj = h.run("FETCH colors.c, sizes.s FROM colors CROSS LINK sizes;");
    assert(cj.rows.size() == 6);

    /* A FLOAT default fills in for omitted columns. */
    h.run("BUILD RELATION acct (id INT, bal FLOAT DEFAULT 2.5);");
    h.run("PUT INTO acct (id) VALUES (7);");
    assert(near(h.run("FETCH bal FROM acct WHEN id = 7;").rows[0][0].doubleValue, 2.5));

    /* Column aliases (AS) rename output headers. */
    auto al = h.run("FETCH cid AS ident, label AS tag FROM cat SORT BY cid;");
    assert(al.columns.size() == 2 && al.columns[0] == "ident" && al.columns[1] == "tag");

    /* TAKE ... SKIP applies an offset before the limit. */
    h.run("BUILD RELATION nums (n INT);");
    h.run("PUT INTO nums VALUES (1),(2),(3),(4),(5);");
    auto off = h.run("FETCH n FROM nums SORT BY n TAKE 2 SKIP 1;");
    assert(off.rows.size() == 2 && off.rows[0][0].intValue == 2 &&
           off.rows[1][0].intValue == 3);

    /* Table aliases enable an aliased self-join. */
    auto sj = h.run(
        "FETCH e.label, d.label AS other FROM cat e LINK cat d "
        "ON e.cid = d.cid SORT BY e.cid;");
    assert(sj.columns[1] == "other" && sj.rows.size() == 2 &&
           sj.rows[0][0].textValue == sj.rows[0][1].textValue);

    /* Scalar string and math functions. */
    assert(h.run("FETCH UPPER('abc') AS u FROM nums TAKE 1;").rows[0][0].textValue == "ABC");
    assert(h.run("FETCH LENGTH('hello') AS n FROM nums TAKE 1;").rows[0][0].intValue == 5);
    assert(h.run("FETCH SUBSTR('hello',2,3) AS s FROM nums TAKE 1;").rows[0][0].textValue ==
           "ell");
    assert(h.run("FETCH ABS(-7) AS a FROM nums TAKE 1;").rows[0][0].intValue == 7);
    assert(h.run("FETCH MOD(10,3) AS m FROM nums TAKE 1;").rows[0][0].intValue == 1);
    assert(near(h.run("FETCH ROUND(3.14159,2) AS r FROM nums TAKE 1;").rows[0][0].doubleValue,
                3.14));

    /* COALESCE returns the first non-null; NULLIF nulls equal values. */
    assert(h.run("FETCH COALESCE(NULL,5) AS c FROM nums TAKE 1;").rows[0][0].intValue == 5);
    assert(h.run("FETCH NULLIF(4,4) AS n FROM nums TAKE 1;").rows[0][0].isNull());

    /* CASE chooses the first matching branch. */
    auto ce = h.run(
        "FETCH n, CASE WHEN n > 3 THEN 'hi' ELSE 'lo' END AS b FROM nums SORT BY n;");
    assert(ce.rows[0][1].textValue == "lo" && ce.rows[4][1].textValue == "hi");

    /* CAST converts between types. */
    assert(h.run("FETCH CAST(2.9 AS INT) AS ci FROM nums TAKE 1;").rows[0][0].intValue == 2);

    /* RIGHT and FULL outer joins. */
    h.run("BUILD RELATION ra (id INT, x TEXT);");
    h.run("BUILD RELATION rb (id INT, y TEXT);");
    h.run("PUT INTO ra VALUES (1,'a1'),(2,'a2');");
    h.run("PUT INTO rb VALUES (2,'b2'),(3,'b3');");
    auto rj = h.run(
        "FETCH ra.x, rb.y FROM ra RIGHT LINK rb ON ra.id = rb.id SORT BY rb.id;");
    assert(rj.rows.size() == 2 && rj.rows[1][0].isNull() &&
           rj.rows[1][1].textValue == "b3");
    auto fj = h.run(
        "FETCH ra.x, rb.y FROM ra FULL LINK rb ON ra.id = rb.id SORT BY ra.id;");
    assert(fj.rows.size() == 3);

    /* EXPLAIN emits a single-column plan. */
    auto ex = h.run("EXPLAIN FETCH x FROM ra WHEN id = 1;");
    assert(ex.columns.size() == 1 && ex.columns[0] == "plan" && !ex.rows.empty());

    /* Set operations combine two queries. */
    h.run("BUILD RELATION s1 (n INT);");
    h.run("BUILD RELATION s2 (n INT);");
    h.run("PUT INTO s1 VALUES (1),(2),(3);");
    h.run("PUT INTO s2 VALUES (2),(3),(4);");
    assert(h.run("FETCH n FROM s1 UNION FETCH n FROM s2;").rows.size() == 4);
    assert(h.run("FETCH n FROM s1 UNION ALL FETCH n FROM s2;").rows.size() == 6);
    auto inter = h.run("FETCH n FROM s1 INTERSECT FETCH n FROM s2 SORT BY n;");
    assert(inter.rows.size() == 2 && inter.rows[0][0].intValue == 2);
    auto exc = h.run("FETCH n FROM s1 EXCEPT FETCH n FROM s2;");
    assert(exc.rows.size() == 1 && exc.rows[0][0].intValue == 1);

    /* INSERT ... FETCH copies query results into a table. */
    h.run("BUILD RELATION s3 (n INT);");
    h.run("PUT INTO s3 FETCH n FROM s1 WHEN n > 1;");
    assert(h.run("FETCH n FROM s3;").rows.size() == 2);

    /* AUTO_INCREMENT assigns sequential ids; BIGINT is an INT alias. */
    h.run("BUILD RELATION seq (id INT PRIMARY KEY AUTO_INCREMENT, v BIGINT);");
    h.run("PUT INTO seq (v) VALUES (10);");
    h.run("PUT INTO seq (v) VALUES (20);");
    auto sq = h.run("FETCH id, v FROM seq SORT BY id;");
    assert(sq.rows.size() == 2 && sq.rows[0][0].intValue == 1 &&
           sq.rows[1][0].intValue == 2);

    /* UPDATE accepts a subquery in its WHEN clause. */
    h.run("MODIFY seq SET v = 99 WHEN id IN (FETCH id FROM seq WHEN v = 10);");
    assert(h.run("FETCH v FROM seq WHEN id = 1;").rows[0][0].intValue == 99);

    /* Aggregates are allowed in HAVING. */
    h.run("BUILD RELATION emp (id INT, dept TEXT, sal INT);");
    h.run("PUT INTO emp VALUES (1,'eng',100),(2,'eng',200),(3,'sa',150),(4,'sa',300),"
          "(5,'sa',50);");
    auto hv = h.run(
        "FETCH dept FROM emp GROUP BY dept HAVING COUNT(*) > 2 SORT BY dept;");
    assert(hv.rows.size() == 1 && hv.rows[0][0].textValue == "sa");
    auto hv2 = h.run(
        "FETCH dept FROM emp GROUP BY dept HAVING SUM(sal) > 300 SORT BY dept;");
    assert(hv2.rows.size() == 1 && hv2.rows[0][0].textValue == "sa");

    /* ON REMOVE CASCADE deletes children; SET NULL nulls the child FK. */
    h.run("BUILD RELATION par (id INT PRIMARY KEY);");
    h.run("BUILD RELATION ch (id INT, p INT REFERENCES par(id) ON REMOVE CASCADE);");
    h.run("BUILD RELATION nt (id INT, p INT REFERENCES par(id) ON REMOVE SET NULL);");
    h.run("PUT INTO par VALUES (1),(2);");
    h.run("PUT INTO ch VALUES (10,1),(11,1),(12,2);");
    h.run("PUT INTO nt VALUES (20,1),(21,2);");
    h.run("REMOVE FROM par WHEN id = 1;");
    assert(h.run("FETCH id FROM ch SORT BY id;").rows.size() == 1);
    auto nn = h.run("FETCH p FROM nt WHEN id = 20;");
    assert(nn.rows.size() == 1 && nn.rows[0][0].isNull());

    /* DATE/TIMESTAMP compare as ISO text; DECIMAL is numeric. */
    h.run("BUILD RELATION ev (id INT, d DATE, amt DECIMAL(10,2));");
    h.run("PUT INTO ev VALUES (1,'2024-03-01',19.99),(2,'2023-12-25',5.5);");
    auto dt = h.run("FETCH id FROM ev WHEN d > '2024-01-01' SORT BY d;");
    assert(dt.rows.size() == 1 && dt.rows[0][0].intValue == 1);
    assert(near(h.run("FETCH amt * 2 AS x FROM ev WHEN id = 1;").rows[0][0].doubleValue,
                39.98));

    semantic::Catalog::instance().reset();
    std::remove("relite_test_feat.db");
    std::remove("relite_test_feat.wal");
    std::cout << "All feature tests passed.\n";
}

}

int main() {
    run();
    return 0;
}
