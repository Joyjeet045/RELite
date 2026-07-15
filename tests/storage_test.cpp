#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "backend/buffer_pool.hpp"
#include "backend/disk_manager.hpp"
#include "backend/page.hpp"
#include "backend/page_guard.hpp"
#include "vm/tuple.hpp"
#include "vm/value.hpp"

using namespace db;

namespace {

std::string tempDbPath(const char* tag) {
    return std::string("prqlite_test_") + tag + ".db";
}

void testTupleRoundTrip() {
    vm::Schema schema = {parser::DataType::Int, parser::DataType::Text,
                         parser::DataType::Bool, parser::DataType::Int};
    vm::Tuple t({vm::Value::makeInt(42), vm::Value::makeText("O'Brien"),
                 vm::Value::makeBool(true), vm::Value::null()});
    std::string bytes = t.serialize(schema);
    vm::Tuple back = vm::Tuple::deserialize(bytes, schema);
    assert(back.size() == 4);
    assert(back.at(0).intValue == 42);
    assert(back.at(1).textValue == "O'Brien");
    assert(back.at(2).boolValue == true);
    assert(back.at(3).isNull());
}

void testValueCompare() {
    assert(*vm::compareValues(vm::Value::makeInt(1), vm::Value::makeInt(2)) < 0);
    assert(*vm::compareValues(vm::Value::makeText("b"), vm::Value::makeText("a")) > 0);
    assert(!vm::compareValues(vm::Value::null(), vm::Value::makeInt(1)).has_value());
    assert(vm::valueLess(vm::Value::null(), vm::Value::makeInt(0)));
}

void testPageInsertGetErase() {
    backend::Page page;
    int s0, s1;
    assert(page.insert("hello", s0));
    assert(page.insert("world!!", s1));
    std::string out;
    assert(page.get(s0, out) && out == "hello");
    assert(page.get(s1, out) && out == "world!!");
    assert(page.erase(s0));
    assert(!page.get(s0, out));
    assert(page.isLive(s1));
    assert(page.update(s1, "worl"));
    assert(page.get(s1, out) && out == "worl");
    assert(!page.update(s1, "way too long to fit in four bytes"));
}

void testPageFull() {
    backend::Page page;
    std::string big(2000, 'x');
    int slot;
    assert(page.insert(big, slot));
    assert(page.insert(big, slot));
    assert(!page.insert(big, slot));
}

void testDiskRoundTrip() {
    std::string path = tempDbPath("disk");
    {
        backend::DiskManager disk(path, /*truncate=*/true);
        backend::PageId id = disk.allocatePage();
        assert(id == 0);
        std::vector<char> buf(backend::PAGE_SIZE, 0);
        std::string msg = "persisted-page";
        std::copy(msg.begin(), msg.end(), buf.begin());
        disk.writePage(id, buf.data());
    }
    {
        backend::DiskManager disk(path);
        assert(disk.numPages() == 1);
        std::vector<char> buf(backend::PAGE_SIZE, 0);
        disk.readPage(0, buf.data());
        assert(std::string(buf.data(), 14) == "persisted-page");
    }
    std::remove(path.c_str());
}

void testBufferPoolEviction() {
    std::string path = tempDbPath("bp");
    backend::DiskManager disk(path, /*truncate=*/true);
    backend::BufferPool pool(&disk, /*numFrames=*/2);

    std::vector<backend::PageId> ids;
    for (int i = 0; i < 3; ++i) {
        backend::PageId id;
        backend::Page* p = pool.newPage(id);
        assert(p != nullptr);
        int slot;
        p->insert(std::string("row-") + std::to_string(i), slot);
        pool.unpin(id, /*dirty=*/true);
        ids.push_back(id);
    }
    pool.flushAll();

    for (int i = 0; i < 3; ++i) {
        backend::Page* p = pool.fetchPage(ids[i]);
        assert(p != nullptr);
        std::string out;
        assert(p->get(0, out));
        assert(out == std::string("row-") + std::to_string(i));
        pool.unpin(ids[i], false);
    }
    std::remove(path.c_str());
}

void testPageGuard() {
    std::string path = tempDbPath("guard");
    backend::DiskManager disk(path, /*truncate=*/true);
    backend::BufferPool pool(&disk, 4);
    backend::PageId id;
    {
        backend::Page* p = pool.newPage(id);
        backend::PageGuard guard(&pool, id, p);
        int slot;
        guard.page()->insert("guarded", slot);
        guard.markDirty();
    }
    backend::Page* p = pool.fetchPage(id);
    std::string out;
    assert(p->get(0, out) && out == "guarded");
    pool.unpin(id, false);
    std::remove(path.c_str());
}

}

int main() {
    testTupleRoundTrip();
    testValueCompare();
    testPageInsertGetErase();
    testPageFull();
    testDiskRoundTrip();
    testBufferPoolEviction();
    testPageGuard();
    std::cout << "All storage tests passed.\n";
    return 0;
}
