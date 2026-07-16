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

    /* Composite index: equality on all columns is served by the index and
       stays consistent across updates. */
    h.run("BUILD RELATION cx (a INT, b INT, c TEXT);");
    h.run("PUT INTO cx VALUES (1,10,'x'),(1,20,'y'),(2,10,'z');");
    h.run("BUILD INDEX cxab ON cx (a, b);");
    assert(h.run("FETCH c FROM cx WHEN a = 1 AND b = 20;").rows[0][0].textValue == "y");
    h.run("MODIFY cx SET b = 99 WHEN a = 1 AND b = 20;");
    assert(h.run("FETCH c FROM cx WHEN a = 1 AND b = 99;").rows.size() == 1);
    assert(h.run("FETCH c FROM cx WHEN a = 1 AND b = 20;").rows.empty());
    bool idxScan = false;
    for (auto& r : h.run("EXPLAIN FETCH c FROM cx WHEN a = 1 AND b = 99;").rows) {
        if (r[0].textValue.find("Index Scan") != std::string::npos) idxScan = true;
    }
    assert(idxScan);

    /* Views: a stored query is materialized and outer clauses apply on top. */
    h.run("BUILD RELATION emp2 (id INT, dept TEXT, sal INT);");
    h.run("PUT INTO emp2 VALUES (1,'eng',100),(2,'eng',200),(3,'sa',150);");
    h.run("BUILD VIEW eng2 AS FETCH id, sal FROM emp2 WHEN dept = 'eng';");
    assert(h.run("FETCH id FROM eng2 SORT BY id;").rows.size() == 2);
    assert(h.run("FETCH id FROM eng2 WHEN sal > 150;").rows[0][0].intValue == 2);
    assert(h.run("FETCH COUNT(*) AS n FROM eng2;").rows[0][0].intValue == 2);

    /* Multi-way (3+ table) joins chain left-associatively. */
    h.run("BUILD RELATION ja (id INT, name TEXT);");
    h.run("BUILD RELATION jb (id INT, aid INT, val INT);");
    h.run("BUILD RELATION jc (id INT, bid INT, tag TEXT);");
    h.run("PUT INTO ja VALUES (1,'x'),(2,'y');");
    h.run("PUT INTO jb VALUES (10,1,100),(20,2,200);");
    h.run("PUT INTO jc VALUES (100,10,'p'),(200,20,'q');");
    auto mj = h.run(
        "FETCH ja.name, jc.tag FROM ja LINK jb ON ja.id = jb.aid "
        "LINK jc ON jb.id = jc.bid SORT BY ja.id;");
    assert(mj.rows.size() == 2 && mj.rows[0][0].textValue == "x" &&
           mj.rows[0][1].textValue == "p" && mj.rows[1][1].textValue == "q");
    auto mjw = h.run(
        "FETCH ja.name FROM ja LINK jb ON ja.id = jb.aid "
        "LINK jc ON jb.id = jc.bid WHEN jb.val > 150;");
    assert(mjw.rows.size() == 1 && mjw.rows[0][0].textValue == "y");

    /* Window functions: ROW_NUMBER per partition and SUM over a partition. */
    h.run("BUILD RELATION we (id INT, dept TEXT, sal INT);");
    h.run("PUT INTO we VALUES (1,'e',100),(2,'e',200),(3,'e',150),(4,'s',300);");
    auto rn = h.run(
        "FETCH dept, ROW_NUMBER() OVER (PARTITION BY dept SORT BY sal DESC) AS rn "
        "FROM we SORT BY dept, sal DESC;");
    assert(rn.rows.size() == 4 && rn.rows[0][1].intValue == 1 &&
           rn.rows[1][1].intValue == 2 && rn.rows[2][1].intValue == 3 &&
           rn.rows[3][1].intValue == 1);
    auto wsum = h.run(
        "FETCH dept, SUM(sal) OVER (PARTITION BY dept) AS tot FROM we WHEN dept = 'e';");
    assert(wsum.rows.size() == 3 && wsum.rows[0][1].intValue == 450);

    /* Correlated subqueries: EXISTS, scalar-in-WHERE, and scalar-in-projection. */
    h.run("BUILD RELATION cdept (id INT, dname TEXT);");
    h.run("PUT INTO cdept VALUES (1,'a'),(2,'b'),(3,'c');");
    h.run("BUILD RELATION cemp (id INT, did INT, sal INT);");
    h.run("PUT INTO cemp VALUES (1,1,100),(2,1,200),(3,2,300),(4,2,150);");
    auto cex = h.run(
        "FETCH dname FROM cdept d WHEN EXISTS "
        "(FETCH id FROM cemp e WHEN e.did = d.id) SORT BY dname;");
    assert(cex.rows.size() == 2 && cex.rows[0][0].textValue == "a" &&
           cex.rows[1][0].textValue == "b");
    auto cwh = h.run(
        "FETCH id FROM cemp e WHEN sal > "
        "(FETCH AVG(sal) FROM cemp e2 WHEN e2.did = e.did) SORT BY id;");
    assert(cwh.rows.size() == 2 && cwh.rows[0][0].intValue == 2 &&
           cwh.rows[1][0].intValue == 3);
    auto cpr = h.run(
        "FETCH dname, (FETCH COUNT(*) FROM cemp e WHEN e.did = d.id) AS n "
        "FROM cdept d SORT BY dname;");
    assert(cpr.rows.size() == 3 && cpr.rows[0][1].intValue == 2 &&
           cpr.rows[1][1].intValue == 2 && cpr.rows[2][1].intValue == 0);

    /* MVCC time travel: AS OF reconstructs a table at a past logical version. */
    h.run("BUILD RELATION tt (id INT, bal INT);");
    h.run("PUT INTO tt VALUES (1,100),(2,200);");
    unsigned long long verA = h.se.versions().currentVersion();
    h.run("MODIFY tt SET bal = 999 WHEN id = 1;");
    unsigned long long verB = h.se.versions().currentVersion();
    h.run("REMOVE FROM tt WHEN id = 2;");
    auto tlive = h.run("FETCH id, bal FROM tt SORT BY id;");
    assert(tlive.rows.size() == 1 && tlive.rows[0][1].intValue == 999);
    auto tvA = h.run("FETCH id, bal FROM tt AS OF " + std::to_string(verA) +
                     " SORT BY id;");
    assert(tvA.rows.size() == 2 && tvA.rows[0][1].intValue == 100 &&
           tvA.rows[1][1].intValue == 200);
    auto tvB = h.run("FETCH id, bal FROM tt AS OF " + std::to_string(verB) +
                     " SORT BY id;");
    assert(tvB.rows.size() == 2 && tvB.rows[0][1].intValue == 999 &&
           tvB.rows[1][1].intValue == 200);
    auto tv0 = h.run("FETCH id, bal FROM tt AS OF 0;");
    assert(tv0.rows.empty());

    /* MVCC garbage collection: compacting history keeps AS OF exact at/after the
     * horizon and clamps older versions to the baseline. */
    {
        auto& vs = h.se.versions();
        h.run("BUILD RELATION gct (id INT, v INT);");
        h.run("PUT INTO gct VALUES (1,10);");
        unsigned long long gA = vs.currentVersion();
        h.run("PUT INTO gct VALUES (2,20);");
        unsigned long long gB = vs.currentVersion();
        h.run("MODIFY gct SET v = 99 WHEN id = 1;");
        unsigned long long gC = vs.currentVersion();

        std::size_t before = vs.changeCount();
        vs.gc(gB);
        assert(vs.baselineVersion() == gB);
        assert(vs.changeCount() < before);

        auto gc1 = h.run("FETCH id, v FROM gct AS OF " + std::to_string(gC) +
                         " SORT BY id;");
        assert(gc1.rows.size() == 2 && gc1.rows[0][1].intValue == 99 &&
               gc1.rows[1][1].intValue == 20);
        auto gb1 = h.run("FETCH id, v FROM gct AS OF " + std::to_string(gB) +
                         " SORT BY id;");
        assert(gb1.rows.size() == 2 && gb1.rows[0][1].intValue == 10 &&
               gb1.rows[1][1].intValue == 20);
        auto ga1 = h.run("FETCH id, v FROM gct AS OF " + std::to_string(gA) +
                         " SORT BY id;");
        assert(ga1.rows.size() == 2 && ga1.rows[0][1].intValue == 10);
    }

    /* Snapshot isolation: a reader transaction sees a stable snapshot even after
     * another transaction commits a change; its own writes remain visible. */
    {
        int txnCur = 0;
        auto runTxn = [&](const std::string& sql) {
            parser::Lexer lexer(sql);
            parser::Parser parser(lexer.tokenize());
            auto stmt = parser.parseStatement();
            semantic::SemanticAnalyzer analyzer(semantic::Catalog::instance());
            analyzer.analyze(*stmt);
            vm::ExecutorEngine engine(h.se, semantic::Catalog::instance(), &h.tm,
                                      &txnCur);
            return engine.run(*stmt);
        };

        h.run("BUILD RELATION si (id INT, bal INT);");
        h.run("PUT INTO si VALUES (1,100);");

        runTxn("START;");
        auto r1 = runTxn("FETCH bal FROM si WHEN id = 1;");
        assert(r1.rows.size() == 1 && r1.rows[0][0].intValue == 100);

        h.run("MODIFY si SET bal = 500 WHEN id = 1;");

        auto r2 = runTxn("FETCH bal FROM si WHEN id = 1;");
        assert(r2.rows.size() == 1 && r2.rows[0][0].intValue == 100);

        runTxn("SAVE;");
        auto r3 = h.run("FETCH bal FROM si WHEN id = 1;");
        assert(r3.rows.size() == 1 && r3.rows[0][0].intValue == 500);
    }

    /* Join time travel: AS OF applies to every table in a multi-table query. */
    {
        h.run("BUILD RELATION jdept (id INT, dname TEXT);");
        h.run("BUILD RELATION jemp (id INT, did INT, nm TEXT);");
        h.run("PUT INTO jdept VALUES (1,'eng'),(2,'sales');");
        h.run("PUT INTO jemp VALUES (1,1,'a'),(2,2,'b');");
        unsigned long long jv = h.se.versions().currentVersion();
        h.run("MODIFY jdept SET dname = 'ENG' WHEN id = 1;");

        auto live = h.run(
            "FETCH jemp.nm, jdept.dname FROM jemp LINK jdept ON jemp.did = jdept.id "
            "SORT BY jemp.nm;");
        assert(live.rows.size() == 2 && live.rows[0][1].textValue == "ENG" &&
               live.rows[1][1].textValue == "sales");

        auto past = h.run(
            "FETCH jemp.nm, jdept.dname FROM jemp AS OF " + std::to_string(jv) +
            " LINK jdept ON jemp.did = jdept.id SORT BY jemp.nm;");
        assert(past.rows.size() == 2 && past.rows[0][1].textValue == "eng" &&
               past.rows[1][1].textValue == "sales");
    }

    /* Cost-based join: large inputs pick a sort-merge join (EXPLAIN shows it)
     * and produce the same result a hash join would. */
    {
        h.run("BUILD RELATION ml (k INT, lv INT);");
        h.run("BUILD RELATION mr (k INT, rv INT);");
        std::string li = "PUT INTO ml VALUES ";
        std::string ri = "PUT INTO mr VALUES ";
        const int m = 10001;
        for (int i = 0; i < m; ++i) {
            if (i) {
                li += ',';
                ri += ',';
            }
            li += "(" + std::to_string(i) + "," + std::to_string(i * 2) + ")";
            ri += "(" + std::to_string(i) + "," + std::to_string(i * 3) + ")";
        }
        li += ';';
        ri += ';';
        h.run(li);
        h.run(ri);

        auto ex = h.run("EXPLAIN FETCH ml.k FROM ml LINK mr ON ml.k = mr.k;");
        bool sawMerge = false;
        for (auto& r : ex.rows) {
            if (r[0].textValue.find("Merge") != std::string::npos) sawMerge = true;
        }
        assert(sawMerge);

        auto joined = h.run(
            "FETCH ml.k, ml.lv, mr.rv FROM ml LINK mr ON ml.k = mr.k SORT BY ml.k;");
        assert(joined.rows.size() == static_cast<std::size_t>(m));
        assert(joined.rows[5][0].intValue == 5 && joined.rows[5][1].intValue == 10 &&
               joined.rows[5][2].intValue == 15);
    }

    /* PUT INTO ... DEFAULT VALUES inserts one row of column defaults. */
    {
        h.run("BUILD RELATION dv (id INT AUTO_INCREMENT, n INT DEFAULT 7, "
              "s TEXT DEFAULT 'x');");
        h.run("PUT INTO dv DEFAULT VALUES;");
        auto d = h.run("FETCH id, n, s FROM dv;");
        assert(d.rows.size() == 1 && d.rows[0][0].intValue == 1 &&
               d.rows[0][1].intValue == 7 && d.rows[0][2].textValue == "x");
    }

    /* DISCARD VIEW drops a view (and only a view). */
    {
        h.run("BUILD RELATION vsrc (id INT, v INT);");
        h.run("PUT INTO vsrc VALUES (1,10),(2,20);");
        h.run("BUILD VIEW vv AS FETCH id FROM vsrc WHEN v > 15;");
        assert(semantic::Catalog::instance().hasTable("vv"));
        h.run("DISCARD VIEW vv;");
        assert(!semantic::Catalog::instance().hasTable("vv"));
    }

    /* CTAS: BUILD RELATION t AS FETCH ... creates and populates from a query. */
    {
        h.run("BUILD RELATION ctas_src (id INT, v INT);");
        h.run("PUT INTO ctas_src VALUES (1,10),(2,20),(3,30);");
        h.run("BUILD RELATION ctas_dst AS FETCH id, v FROM ctas_src WHEN v > 10;");
        assert(semantic::Catalog::instance().hasTable("ctas_dst"));
        auto c = h.run("FETCH id, v FROM ctas_dst SORT BY id;");
        assert(c.rows.size() == 2 && c.rows[0][0].intValue == 2 &&
               c.rows[1][0].intValue == 3 && c.rows[1][1].intValue == 30);
    }

    /* TRUNCATE clears all rows (and resets scan-based AUTO_INCREMENT). */
    {
        h.run("BUILD RELATION tr (id INT AUTO_INCREMENT, v INT);");
        h.run("PUT INTO tr (v) VALUES (10),(20),(30);");
        assert(h.run("FETCH v FROM tr;").rows.size() == 3);
        h.run("TRUNCATE RELATION tr;");
        assert(h.run("FETCH v FROM tr;").rows.empty());
        h.run("PUT INTO tr (v) VALUES (99);");
        auto after = h.run("FETCH id, v FROM tr;");
        assert(after.rows.size() == 1 && after.rows[0][0].intValue == 1 &&
               after.rows[0][1].intValue == 99);
    }

    /* RETURNING projects affected rows from PUT / MODIFY / REMOVE. */
    {
        h.run("BUILD RELATION ret (id INT AUTO_INCREMENT, v INT);");
        auto ins = h.run("PUT INTO ret (v) VALUES (10),(20) RETURNING id, v;");
        assert(ins.isQuery && ins.rows.size() == 2);
        assert(ins.columns.size() == 2 && ins.columns[0] == "id" &&
               ins.columns[1] == "v");
        assert(ins.rows[0][0].intValue == 1 && ins.rows[0][1].intValue == 10 &&
               ins.rows[1][0].intValue == 2 && ins.rows[1][1].intValue == 20);

        auto insStar = h.run("PUT INTO ret (v) VALUES (30) RETURNING *;");
        assert(insStar.isQuery && insStar.rows.size() == 1 &&
               insStar.rows[0][0].intValue == 3 &&
               insStar.rows[0][1].intValue == 30);

        auto upd = h.run("MODIFY ret SET v = v + 100 WHEN id = 1 RETURNING id, v;");
        assert(upd.isQuery && upd.rows.size() == 1 &&
               upd.rows[0][0].intValue == 1 && upd.rows[0][1].intValue == 110);

        auto del = h.run("REMOVE FROM ret WHEN id = 2 RETURNING *;");
        assert(del.isQuery && del.rows.size() == 1 &&
               del.rows[0][0].intValue == 2 && del.rows[0][1].intValue == 20);

        assert(h.run("FETCH id FROM ret;").rows.size() == 2);
    }

    /* UPSERT: ON CONFLICT DO MODIFY updates, DO NOTHING skips. */
    {
        h.run("BUILD RELATION up (id INT PRIMARY KEY, v INT, hits INT);");
        h.run("PUT INTO up VALUES (1,10,0),(2,20,0);");
        h.run("PUT INTO up VALUES (1,99,0) ON CONFLICT (id) DO MODIFY "
              "SET v = 99, hits = hits + 1;");
        auto r1 = h.run("FETCH v, hits FROM up WHEN id = 1;");
        assert(r1.rows.size() == 1 && r1.rows[0][0].intValue == 99 &&
               r1.rows[0][1].intValue == 1);

        /* No conflict -> plain insert. */
        h.run("PUT INTO up VALUES (3,30,0) ON CONFLICT (id) DO NOTHING;");
        assert(h.run("FETCH id FROM up;").rows.size() == 3);

        /* Conflict -> DO NOTHING leaves the existing row untouched. */
        h.run("PUT INTO up VALUES (3,999,0) ON CONFLICT (id) DO NOTHING;");
        auto r3 = h.run("FETCH v FROM up WHEN id = 3;");
        assert(r3.rows.size() == 1 && r3.rows[0][0].intValue == 30);

        /* UPSERT with RETURNING projects the modified row. */
        auto rr = h.run("PUT INTO up VALUES (2,20,0) ON CONFLICT (id) DO MODIFY "
                        "SET v = v + 5 RETURNING id, v;");
        assert(rr.isQuery && rr.rows.size() == 1 &&
               rr.rows[0][0].intValue == 2 && rr.rows[0][1].intValue == 25);
    }

    /* ON MODIFY CASCADE propagates a parent key change; SET NULL nulls it. */
    {
        h.run("BUILD RELATION mpar (id INT PRIMARY KEY);");
        h.run("BUILD RELATION mcc (id INT, "
              "p INT REFERENCES mpar(id) ON MODIFY CASCADE);");
        h.run("BUILD RELATION mnn (id INT, "
              "p INT REFERENCES mpar(id) ON MODIFY SET NULL);");
        h.run("PUT INTO mpar VALUES (1),(2);");
        h.run("PUT INTO mcc VALUES (10,1),(11,1),(12,2);");
        h.run("PUT INTO mnn VALUES (20,1),(21,2);");
        h.run("MODIFY mpar SET id = 99 WHEN id = 1;");
        auto cc = h.run("FETCH id FROM mcc WHEN p = 99 SORT BY id;");
        assert(cc.rows.size() == 2 && cc.rows[0][0].intValue == 10 &&
               cc.rows[1][0].intValue == 11);
        assert(h.run("FETCH id FROM mcc WHEN p = 2;").rows.size() == 1);
        auto nn2 = h.run("FETCH p FROM mnn WHEN id = 20;");
        assert(nn2.rows.size() == 1 && nn2.rows[0][0].isNull());
    }

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
