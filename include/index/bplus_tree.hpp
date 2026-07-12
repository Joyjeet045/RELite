#pragma once

#include <cstddef>
#include <shared_mutex>
#include <vector>

#include "vm/record_id.hpp"
#include "vm/value.hpp"

namespace db::index {

// A B+ tree mapping ordered keys to the RecordIDs of rows carrying that key.
// Keys are vm::Values ordered by vm::valueLess; duplicate keys are supported
// (each maps to a list of rids). Leaves are chained for range scans.
//
// Insertion is fully balanced (nodes split and the split propagates to the
// root). Deletion removes entries in place; a leaf that becomes empty is
// unlinked from its parent (cascading up), so the tree does not accumulate dead
// nodes. Underfull-but-nonempty nodes are not merged (a common simplification),
// which keeps lookups/scans correct. All public operations are thread-safe.
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

    // Inclusive [lo, hi] range scan in key order.
    std::vector<vm::RecordID> range(const Key& lo, const Key& hi) const;

    // Bounded range scan with optional (nullptr = unbounded) endpoints. Both
    // bounds are inclusive when present. Used by the executor's index scans.
    std::vector<vm::RecordID> rangeScan(const Key* lo, const Key* hi) const;

    std::size_t distinctKeys() const { return distinctKeys_; }

private:
    struct Node {
        bool leaf = true;
        std::vector<Key> keys;
        std::vector<Node*> children;                    // internal nodes
        std::vector<std::vector<vm::RecordID>> values;  // leaf nodes
        Node* next = nullptr;                           // leaf chain (forward)
        Node* prev = nullptr;                           // leaf chain (backward)
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

}  // namespace db::index
