#include "index/bplus_tree.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace db::index {

BPlusTree::BPlusTree(int order) : order_(order < 3 ? 3 : order) {
    root_ = new Node();
    root_->leaf = true;
}

BPlusTree::~BPlusTree() { freeNode(root_); }

void BPlusTree::freeNode(Node* node) {
    if (node == nullptr) return;
    if (!node->leaf) {
        for (Node* child : node->children) {
            freeNode(child);
        }
    }
    delete node;
}

BPlusTree::Node* BPlusTree::findLeaf(const Key& key) const {
    Node* n = root_;
    while (!n->leaf) {
        std::size_t idx = 0;
        while (idx < n->keys.size() && !less(key, n->keys[idx])) {
            ++idx;
        }
        n = n->children[idx];
    }
    return n;
}

BPlusTree::Node* BPlusTree::firstLeaf() const {
    Node* n = root_;
    while (!n->leaf) {
        n = n->children[0];
    }
    return n;
}

BPlusTree::Split BPlusTree::splitLeaf(Node* node) {
    int mid = static_cast<int>(node->keys.size()) / 2;
    Node* right = new Node();
    right->leaf = true;
    right->keys.assign(node->keys.begin() + mid, node->keys.end());
    right->values.assign(node->values.begin() + mid, node->values.end());
    node->keys.erase(node->keys.begin() + mid, node->keys.end());
    node->values.erase(node->values.begin() + mid, node->values.end());
    right->next = node->next;
    right->prev = node;
    if (node->next != nullptr) {
        node->next->prev = right;
    }
    node->next = right;
    return Split{true, right->keys.front(), right};
}

BPlusTree::Split BPlusTree::splitInternal(Node* node) {
    int mid = static_cast<int>(node->keys.size()) / 2;
    Key up = node->keys[mid];
    Node* right = new Node();
    right->leaf = false;
    right->keys.assign(node->keys.begin() + mid + 1, node->keys.end());
    right->children.assign(node->children.begin() + mid + 1, node->children.end());
    node->keys.erase(node->keys.begin() + mid, node->keys.end());
    node->children.erase(node->children.begin() + mid + 1, node->children.end());
    return Split{true, up, right};
}

BPlusTree::Split BPlusTree::insertRec(Node* node, const Key& key,
                                      const vm::RecordID& rid) {
    if (node->leaf) {
        std::size_t pos = 0;
        while (pos < node->keys.size() && less(node->keys[pos], key)) {
            ++pos;
        }
        if (pos < node->keys.size() && equal(node->keys[pos], key)) {
            node->values[pos].push_back(rid);
            return Split{};
        }
        node->keys.insert(node->keys.begin() + pos, key);
        node->values.insert(node->values.begin() + pos,
                            std::vector<vm::RecordID>{rid});
        ++distinctKeys_;
        if (static_cast<int>(node->keys.size()) > order_) {
            return splitLeaf(node);
        }
        return Split{};
    }

    std::size_t idx = 0;
    while (idx < node->keys.size() && !less(key, node->keys[idx])) {
        ++idx;
    }
    Split s = insertRec(node->children[idx], key, rid);
    if (!s.did) {
        return Split{};
    }
    node->keys.insert(node->keys.begin() + idx, s.key);
    node->children.insert(node->children.begin() + idx + 1, s.right);
    if (static_cast<int>(node->keys.size()) > order_) {
        return splitInternal(node);
    }
    return Split{};
}

void BPlusTree::insert(const Key& key, const vm::RecordID& rid) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    Split s = insertRec(root_, key, rid);
    if (s.did) {
        Node* newRoot = new Node();
        newRoot->leaf = false;
        newRoot->keys.push_back(s.key);
        newRoot->children.push_back(root_);
        newRoot->children.push_back(s.right);
        root_ = newRoot;
    }
}

std::vector<vm::RecordID> BPlusTree::lookup(const Key& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    Node* leaf = findLeaf(key);
    for (std::size_t i = 0; i < leaf->keys.size(); ++i) {
        if (equal(leaf->keys[i], key)) {
            return leaf->values[i];
        }
    }
    return {};
}

std::vector<vm::RecordID> BPlusTree::range(const Key& lo, const Key& hi) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<vm::RecordID> out;
    Node* leaf = findLeaf(lo);
    while (leaf != nullptr) {
        for (std::size_t i = 0; i < leaf->keys.size(); ++i) {
            if (less(leaf->keys[i], lo)) continue;
            if (less(hi, leaf->keys[i])) return out;  // past the upper bound
            out.insert(out.end(), leaf->values[i].begin(), leaf->values[i].end());
        }
        leaf = leaf->next;
    }
    return out;
}

std::vector<vm::RecordID> BPlusTree::rangeScan(const Key* lo, const Key* hi) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<vm::RecordID> out;
    Node* leaf = (lo != nullptr) ? findLeaf(*lo) : firstLeaf();
    while (leaf != nullptr) {
        for (std::size_t i = 0; i < leaf->keys.size(); ++i) {
            if (lo != nullptr && less(leaf->keys[i], *lo)) continue;
            if (hi != nullptr && less(*hi, leaf->keys[i])) return out;
            out.insert(out.end(), leaf->values[i].begin(), leaf->values[i].end());
        }
        leaf = leaf->next;
    }
    return out;
}

bool BPlusTree::erase(const Key& key, const vm::RecordID& rid) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Descend to the target leaf, recording the (parent, childIndex) path so an
    // emptied node can be unlinked from its parent afterwards.
    std::vector<std::pair<Node*, std::size_t>> path;
    Node* node = root_;
    while (!node->leaf) {
        std::size_t idx = 0;
        while (idx < node->keys.size() && !less(key, node->keys[idx])) {
            ++idx;
        }
        path.emplace_back(node, idx);
        node = node->children[idx];
    }
    Node* leaf = node;

    int pos = -1;
    for (std::size_t i = 0; i < leaf->keys.size(); ++i) {
        if (equal(leaf->keys[i], key)) {
            pos = static_cast<int>(i);
            break;
        }
    }
    if (pos < 0) {
        return false;
    }
    auto& list = leaf->values[pos];
    auto it = std::find(list.begin(), list.end(), rid);
    if (it == list.end()) {
        return false;
    }
    list.erase(it);
    if (!list.empty()) {
        return true;  // other rids still carry this key
    }
    leaf->keys.erase(leaf->keys.begin() + pos);
    leaf->values.erase(leaf->values.begin() + pos);
    --distinctKeys_;

    // Unlink now-empty nodes, cascading toward the root. A node is removed only
    // when it is completely empty (leaf: no keys; internal: no children); this
    // is always a structurally valid B+ tree edit.
    Node* child = node;
    while (child != root_ && !path.empty()) {
        bool empty = child->leaf ? child->keys.empty() : child->children.empty();
        if (!empty) break;

        auto [parent, ci] = path.back();
        path.pop_back();

        if (child->leaf) {
            if (child->prev != nullptr) child->prev->next = child->next;
            if (child->next != nullptr) child->next->prev = child->prev;
        }
        parent->children.erase(parent->children.begin() + ci);
        if (!parent->keys.empty()) {
            std::size_t ki = (ci > 0) ? ci - 1 : 0;
            parent->keys.erase(parent->keys.begin() + ki);
        }
        delete child;
        child = parent;
    }

    // Collapse redundant / emptied roots.
    while (!root_->leaf && root_->children.size() == 1) {
        Node* only = root_->children[0];
        delete root_;
        root_ = only;
    }
    if (!root_->leaf && root_->children.empty()) {
        root_->leaf = true;
        root_->keys.clear();
    }
    return true;
}

}  // namespace db::index
