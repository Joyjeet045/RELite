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
