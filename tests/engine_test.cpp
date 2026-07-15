#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

#include "frontend/catalog.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semantic_analyzer.hpp"
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

void run() {
    semantic::Catalog::instance().reset();
    std::string path = "prqlite_test_engine.db";
    vm::StorageEngine se(path, /*truncate=*/true);

    exec(se, "BUILD RELATION friend (id INT, name TEXT, active BOOL);");
    exec(se, "PUT INTO friend VALUES (1, 'alice', TRUE), (2, 'bob', FALSE), (3, 'carol', TRUE);");

    auto all = exec(se, "FETCH * FROM friend;");
    assert(all.isQuery);
    assert(all.columns.size() == 3 && all.columns[0] == "id");
    assert(all.rows.size() == 3);

    auto one = exec(se, "FETCH name FROM friend WHEN id = 2;");
    assert(one.rows.size() == 1);
    assert(one.columns.size() == 1 && one.columns[0] == "name");
    assert(one.rows[0][0].textValue == "bob");

    auto actives = exec(se, "FETCH id FROM friend WHEN active OR id = 2;");
    assert(actives.rows.size() == 3);

    auto gt = exec(se, "FETCH id FROM friend WHEN id >= 2;");
    assert(gt.rows.size() == 2);

    exec(se, "PUT INTO friend (id, name) VALUES (4, 'dave');");
    auto nullCheck = exec(se, "FETCH active FROM friend WHEN id = 4;");
    assert(nullCheck.rows.size() == 1);
    assert(nullCheck.rows[0][0].isNull());

    auto nullCmp = exec(se, "FETCH id FROM friend WHEN active = TRUE;");
    assert(nullCmp.rows.size() == 2);

    auto del = exec(se, "REMOVE FROM friend WHEN id = 2;");
    assert(del.message == "REMOVE 1");
    auto afterDel = exec(se, "FETCH * FROM friend;");
    assert(afterDel.rows.size() == 3);

    exec(se, "REMOVE FROM friend;");
    auto empty = exec(se, "FETCH * FROM friend;");
    assert(empty.rows.empty());

    semantic::Catalog::instance().reset();
    std::remove(path.c_str());
    std::cout << "All engine tests passed.\n";
}

}

int main() {
    run();
    return 0;
}
