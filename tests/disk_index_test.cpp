#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "backend/buffer_pool.hpp"
#include "backend/disk_manager.hpp"
#include "index/disk_bplus_tree.hpp"
#include "vm/record_id.hpp"
#include "vm/value.hpp"

using namespace db;

namespace {

vm::RecordID rid(int p, int s) { return vm::RecordID{p, s}; }

/*
 * Exercises the page-resident B+ tree over a real disk manager + buffer pool:
 * many inserts forcing node splits, point lookups, duplicate keys, range scans
 * spanning leaves, lazy deletes, and text keys.
 */
void run() {
    backend::DiskManager disk("relite_test_diskidx.db", /*truncate=*/true);
    backend::BufferPool pool(&disk, /*numFrames=*/16);

    index::DiskBPlusTree tree(&pool);

    const int n = 5000;
    for (int i = 0; i < n; ++i) {
        tree.insert(vm::Value::makeInt(i), rid(i / 100, i % 100));
    }

    /* Point lookups after many splits. */
    for (int i = 0; i < n; ++i) {
        auto hits = tree.lookup(vm::Value::makeInt(i));
        assert(hits.size() == 1);
        assert(hits[0] == rid(i / 100, i % 100));
    }

    /* Missing key. */
    assert(tree.lookup(vm::Value::makeInt(n + 10)).empty());

    /* Duplicate values: one indexed value mapping to several rows. */
    tree.insert(vm::Value::makeInt(42), rid(9, 9));
    tree.insert(vm::Value::makeInt(42), rid(9, 10));
    assert(tree.lookup(vm::Value::makeInt(42)).size() == 3);

    /* Inclusive range spanning many leaves. */
    auto r = tree.range(vm::Value::makeInt(100), vm::Value::makeInt(200));
    assert(r.size() == 101);

    /* Open-ended scans. */
    vm::Value lo = vm::Value::makeInt(4990);
    assert(tree.rangeScan(&lo, nullptr).size() == 10);
    vm::Value hi = vm::Value::makeInt(9);
    assert(tree.rangeScan(nullptr, &hi).size() == 10);

    /* Lazy delete: remove specific (key, rid) entries. */
    assert(tree.erase(vm::Value::makeInt(42), rid(9, 9)));
    assert(tree.lookup(vm::Value::makeInt(42)).size() == 2);
    assert(!tree.erase(vm::Value::makeInt(42), rid(123, 45)));
    assert(tree.erase(vm::Value::makeInt(7), rid(0, 7)));
    assert(tree.lookup(vm::Value::makeInt(7)).empty());

    /* Text keys. */
    index::DiskBPlusTree strTree(&pool);
    strTree.insert(vm::Value::makeText("banana"), rid(0, 0));
    strTree.insert(vm::Value::makeText("apple"), rid(0, 1));
    strTree.insert(vm::Value::makeText("cherry"), rid(0, 2));
    strTree.insert(vm::Value::makeText("date"), rid(0, 3));
    auto sr = strTree.range(vm::Value::makeText("apple"), vm::Value::makeText("cherry"));
    assert(sr.size() == 3);
    assert(strTree.lookup(vm::Value::makeText("banana")).size() == 1);
    assert(strTree.lookup(vm::Value::makeText("fig")).empty());

    /* Full scan returns every remaining entry (5000 inserts + 2 dup values
     * - 2 deletes). */
    auto all = tree.rangeScan(nullptr, nullptr);
    assert(all.size() == 5000);

    std::remove("relite_test_diskidx.db");
    std::cout << "disk_index_test passed\n";
}

}  // namespace

int main() {
    run();
    return 0;
}
