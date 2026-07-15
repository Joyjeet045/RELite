#include <cassert>
#include <cstdio>
#include <exception>
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

void expectThrow(Harness& h, const std::string& sql) {
    bool threw = false;
    try {
        h.run(sql);
    } catch (const std::exception&) {
        threw = true;
    }
    if (!threw) {
        std::cerr << "Expected error for: " << sql << "\n";
        assert(false);
    }
}

void testForeignKeys(Harness& h) {
    h.run("BUILD RELATION dept (id INT, dname TEXT);");
    h.run("PUT INTO dept VALUES (1,'eng'),(2,'sales');");
    h.run("BUILD RELATION emp (id INT, name TEXT, dept_id INT REFERENCES dept(id));");
    h.run("PUT INTO emp VALUES (1,'Alice',1);");
    h.run("PUT INTO emp VALUES (2,'Bob',2);");

    expectThrow(h, "PUT INTO emp VALUES (3,'Carol',99);");
    h.run("PUT INTO emp VALUES (4,'Dave',NULL);");
    assert(h.run("FETCH id FROM emp;").rows.size() == 3);

    expectThrow(h, "MODIFY emp SET dept_id = 99 WHEN id = 1;");
    expectThrow(h, "REMOVE FROM dept WHEN id = 1;");
}

void testSubqueries(Harness& h) {
    auto s1 = h.run(
        "FETCH name FROM emp WHEN dept_id = (FETCH id FROM dept WHEN dname = 'sales');");
    assert(s1.rows.size() == 1 && s1.rows[0][0].textValue == "Bob");

    auto s2 = h.run("FETCH name FROM emp WHEN dept_id IN (FETCH id FROM dept);");
    assert(s2.rows.size() == 2);

    auto s3 = h.run(
        "FETCH name FROM emp WHEN dept_id NOT IN (FETCH id FROM dept WHEN dname='eng');");
    assert(s3.rows.size() == 1 && s3.rows[0][0].textValue == "Bob");

    auto s4 = h.run("FETCH name FROM emp WHEN EXISTS (FETCH id FROM dept WHEN id = 2);");
    assert(s4.rows.size() == 3);

    auto s5 =
        h.run("FETCH name FROM emp WHEN NOT EXISTS (FETCH id FROM dept WHEN id = 2);");
    assert(s5.rows.empty());
}

void testAlterTable(Harness& h) {
    h.run("BUILD RELATION t (id INT, name TEXT);");
    h.run("PUT INTO t VALUES (1,'a'),(2,'b');");

    h.run("RESHAPE RELATION t ADD COLUMN age INT;");
    auto a1 = h.run("FETCH age FROM t WHEN id = 1;");
    assert(a1.rows.size() == 1 && a1.rows[0][0].isNull());

    h.run("PUT INTO t VALUES (3,'c',30);");
    auto a2 = h.run("FETCH age FROM t WHEN id = 3;");
    assert(a2.rows[0][0].intValue == 30);

    h.run("RESHAPE RELATION t DISCARD COLUMN name;");
    auto a3 = h.run("FETCH * FROM t WHEN id = 3;");
    assert(a3.columns.size() == 2);
    assert(a3.columns[0] == "id" && a3.columns[1] == "age");
    assert(a3.rows[0][1].intValue == 30);
}

void run() {
    semantic::Catalog::instance().reset();
    Harness h("prqlite_test_p6.db", "prqlite_test_p6.wal");

    testForeignKeys(h);
    testSubqueries(h);
    testAlterTable(h);

    semantic::Catalog::instance().reset();
    std::remove("prqlite_test_p6.db");
    std::remove("prqlite_test_p6.wal");
    std::cout << "All Phase V+ tests passed (subqueries, foreign keys, ALTER).\n";
}

}

int main() {
    run();
    return 0;
}
