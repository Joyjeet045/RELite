#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

#include "frontend/catalog.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semantic_analyzer.hpp"
#include "index/bloom_filter.hpp"
#include "index/bplus_tree.hpp"
#include "vm/executor_engine.hpp"
#include "vm/storage_engine.hpp"

using namespace db;

namespace {

void testBloom() {
    index::BloomFilter bloom(1000, 4);
    for (int i = 0; i < 500; ++i) bloom.addInt(i);
    for (int i = 0; i < 500; ++i) assert(bloom.mightContainInt(i));  // no false negatives
    int falsePositives = 0;
    for (int i = 1000; i < 2000; ++i) {
        if (bloom.mightContainInt(i)) ++falsePositives;
    }
    assert(falsePositives < 100);  // comfortably low rate
}

void testBPlusTree() {
    index::BPlusTree tree(/*order=*/4);  // small order to force many splits
    for (int i = 0; i < 1000; ++i) {
        tree.insert(vm::Value::makeInt(i), vm::RecordID{i / 100, i % 100});
    }
    assert(tree.distinctKeys() == 1000);

    for (int i = 0; i < 1000; ++i) {
        auto hits = tree.lookup(vm::Value::makeInt(i));
        assert(hits.size() == 1);
        assert(hits[0] == (vm::RecordID{i / 100, i % 100}));
    }

    auto r = tree.range(vm::Value::makeInt(100), vm::Value::makeInt(200));
    assert(r.size() == 101);

    // Duplicate keys map to multiple rids.
    tree.insert(vm::Value::makeInt(42), vm::RecordID{9, 9});
    assert(tree.lookup(vm::Value::makeInt(42)).size() == 2);

    // Erase one rid for the duplicate, then the whole key.
    assert(tree.erase(vm::Value::makeInt(42), vm::RecordID{9, 9}));
    assert(tree.lookup(vm::Value::makeInt(42)).size() == 1);
    assert(tree.erase(vm::Value::makeInt(7), vm::RecordID{0, 7}));
    assert(tree.lookup(vm::Value::makeInt(7)).empty());

    // Text keys ordered lexicographically.
    index::BPlusTree strTree;
    strTree.insert(vm::Value::makeText("banana"), vm::RecordID{0, 0});
    strTree.insert(vm::Value::makeText("apple"), vm::RecordID{0, 1});
    strTree.insert(vm::Value::makeText("cherry"), vm::RecordID{0, 2});
    auto sr = strTree.range(vm::Value::makeText("apple"), vm::Value::makeText("banana"));
    assert(sr.size() == 2);
}

vm::ResultSet exec(vm::StorageEngine& se, const std::string& sql) {
    parser::Lexer lexer(sql);
    parser::Parser parser(lexer.tokenize());
    auto stmt = parser.parseStatement();
    semantic::SemanticAnalyzer analyzer(semantic::Catalog::instance());
    analyzer.analyze(*stmt);
    vm::ExecutorEngine engine(se, semantic::Catalog::instance());
    return engine.run(*stmt);
}

void testIndexedQueries() {
    semantic::Catalog::instance().reset();
    std::string path = "prqlite_test_index.db";
    vm::StorageEngine se(path, /*truncate=*/true);

    exec(se, "BUILD RELATION t (id INT, name TEXT);");
    for (int i = 0; i < 50; ++i) {
        exec(se, "PUT INTO t VALUES (" + std::to_string(i) + ", 'n" +
                     std::to_string(i) + "');");
    }
    // Build index after data exists (backfill path).
    exec(se, "BUILD INDEX t_id ON t (id);");

    // Point lookup via index.
    auto q = exec(se, "FETCH name FROM t WHEN id = 25;");
    assert(q.rows.size() == 1 && q.rows[0][0].textValue == "n25");

    // Insert after index exists (maintenance path).
    exec(se, "PUT INTO t VALUES (100, 'hundred');");
    auto q2 = exec(se, "FETCH name FROM t WHEN id = 100;");
    assert(q2.rows.size() == 1 && q2.rows[0][0].textValue == "hundred");

    // Delete maintains index.
    exec(se, "REMOVE FROM t WHEN id = 25;");
    auto q3 = exec(se, "FETCH name FROM t WHEN id = 25;");
    assert(q3.rows.empty());

    // Absent key.
    auto q4 = exec(se, "FETCH name FROM t WHEN id = 9999;");
    assert(q4.rows.empty());

    semantic::Catalog::instance().reset();
    std::remove(path.c_str());
}

}  // namespace

int main() {
    testBloom();
    testBPlusTree();
    testIndexedQueries();
    std::cout << "All index tests passed.\n";
    return 0;
}
