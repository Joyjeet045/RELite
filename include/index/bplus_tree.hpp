#pragma once

#include <cstddef>
#include <shared_mutex>
#include <vector>

#include "vm/record_id.hpp"
#include "vm/value.hpp"

namespace db::index {

class BPlusTree {
public:
    using Key = vm::Value;

    explicit BPlusTree(int order = 32);
    ~BPlusTree();

    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    void insert(const Key& key, const vm::RecordID& rid);
    std::vector<vm::RecordID> lookup(const Key& key) const;
    bool erase(const Key& key, const vm::RecordID& rid);

    std::vector<vm::RecordID> range(const Key& lo, const Key& hi) const;

    std::vector<vm::RecordID> rangeScan(const Key* lo, const Key* hi) const;

    std::size_t distinctKeys() const { return distinctKeys_; }

private:
    struct Node {
        bool leaf = true;
        std::vector<Key> keys;
        std::vector<Node*> children;
        std::vector<std::vector<vm::RecordID>> values;
        Node* next = nullptr;
        Node* prev = nullptr;
    };

    struct Split {
        bool did = false;
        Key key;
        Node* right = nullptr;
    };

    static bool less(const Key& a, const Key& b) { return vm::valueLess(a, b); }
    static bool equal(const Key& a, const Key& b) {
        return !less(a, b) && !less(b, a);
    }

    Node* findLeaf(const Key& key) const;
    Node* firstLeaf() const;
    Split insertRec(Node* node, const Key& key, const vm::RecordID& rid);
    Split splitLeaf(Node* node);
    Split splitInternal(Node* node);
    static void freeNode(Node* node);

    mutable std::shared_mutex mutex_;
    Node* root_;
    int order_;
    std::size_t distinctKeys_ = 0;
};

}
