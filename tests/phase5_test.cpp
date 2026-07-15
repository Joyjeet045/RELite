#include <cassert>
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

int scalarInt(const vm::ResultSet& rs) {
    assert(rs.rows.size() == 1 && rs.rows[0].size() == 1);
    return static_cast<int>(rs.rows[0][0].intValue);
}

void run() {
    semantic::Catalog::instance().reset();
    Harness h("prqlite_test_p5.db", "prqlite_test_p5.wal");

    h.run("BUILD RELATION emp (id INT, name TEXT, dept TEXT, salary INT);");
    h.run("PUT INTO emp VALUES (1,'Alice','eng',100),(2,'Bob','eng',200),"
          "(3,'Carol','sales',150),(4,'Dave','sales',NULL);");

    assert(scalarInt(h.run("FETCH COUNT(*) FROM emp;")) == 4);
    assert(scalarInt(h.run("FETCH SUM(salary) FROM emp;")) == 450);
    assert(scalarInt(h.run("FETCH MIN(salary) FROM emp;")) == 100);
    assert(scalarInt(h.run("FETCH MAX(salary) FROM emp;")) == 200);
    assert(scalarInt(h.run("FETCH COUNT(salary) FROM emp;")) == 3);

    auto grouped = h.run("FETCH dept, COUNT(*) FROM emp GROUP BY dept;");
    assert(grouped.columns.size() == 2 && grouped.rows.size() == 2);
    assert(grouped.rows[0][0].textValue == "eng" && grouped.rows[0][1].intValue == 2);

    auto having = h.run("FETCH dept FROM emp GROUP BY dept HAVING dept = 'sales';");
    assert(having.rows.size() == 1 && having.rows[0][0].textValue == "sales");

    assert(h.run("FETCH id FROM emp WHEN salary IS NULL;").rows.size() == 1);
    assert(h.run("FETCH id FROM emp WHEN salary IS NOT NULL;").rows.size() == 3);

    assert(h.run("FETCH id FROM emp WHEN id IN (1, 2);").rows.size() == 2);
    assert(h.run("FETCH id FROM emp WHEN id NOT IN (1, 2);").rows.size() == 2);

    assert(h.run("FETCH id FROM emp WHEN id BETWEEN 2 AND 3;").rows.size() == 2);

    auto like = h.run("FETCH name FROM emp WHEN name LIKE 'A%';");
    assert(like.rows.size() == 1 && like.rows[0][0].textValue == "Alice");
    assert(h.run("FETCH name FROM emp WHEN name LIKE '_o%';").rows.size() == 1);

    auto ordered = h.run("FETCH id FROM emp SORT BY id DESC;");
    assert(ordered.rows.size() == 4 && ordered.rows[0][0].intValue == 4);
    auto limited = h.run("FETCH id FROM emp SORT BY id ASC TAKE 2;");
    assert(limited.rows.size() == 2 && limited.rows[0][0].intValue == 1 &&
           limited.rows[1][0].intValue == 2);

    h.run("MODIFY emp SET salary = 175 WHEN id = 4;");
    assert(scalarInt(h.run("FETCH salary FROM emp WHEN id = 4;")) == 175);
    assert(h.run("FETCH id FROM emp WHEN salary IS NULL;").rows.empty());

    int before = scalarInt(h.run("FETCH COUNT(*) FROM emp;"));
    h.run("START;");
    h.run("PUT INTO emp VALUES (5,'Eve','eng',300);");
    assert(scalarInt(h.run("FETCH COUNT(*) FROM emp;")) == before + 1);
    h.run("UNDO;");
    assert(scalarInt(h.run("FETCH COUNT(*) FROM emp;")) == before);

    h.run("START;");
    h.run("REMOVE FROM emp WHEN id = 1;");
    assert(h.run("FETCH id FROM emp WHEN id = 1;").rows.empty());
    h.run("UNDO;");
    assert(h.run("FETCH id FROM emp WHEN id = 1;").rows.size() == 1);

    h.run("START;");
    h.run("MODIFY emp SET dept = 'mgmt' WHEN id = 1;");
    h.run("SAVE;");
    auto committed = h.run("FETCH dept FROM emp WHEN id = 1;");
    assert(committed.rows[0][0].textValue == "mgmt");

    h.run("BUILD RELATION dept (dname TEXT, floor INT);");
    h.run("PUT INTO dept VALUES ('eng', 3), ('sales', 1), ('mgmt', 5);");
    auto joined =
        h.run("FETCH emp.name, dept.floor FROM emp LINK dept ON emp.dept = dept.dname;");
    assert(joined.columns.size() == 2 && joined.rows.size() == 4);
    auto joinFiltered = h.run(
        "FETCH emp.name FROM emp LINK dept ON emp.dept = dept.dname "
        "WHEN dept.floor = 1;");
    assert(joinFiltered.rows.size() == 2);
    h.run("DISCARD RELATION dept;");

    h.run("BUILD INDEX emp_id ON emp (id);");
    h.run("DISCARD INDEX emp_id;");
    h.run("DISCARD RELATION emp;");
    assert(!semantic::Catalog::instance().hasTable("emp"));

    semantic::Catalog::instance().reset();
    std::remove("prqlite_test_p5.db");
    std::remove("prqlite_test_p5.wal");
    std::cout << "All Phase V tests passed.\n";
}

}

int main() {
    run();
    return 0;
}
