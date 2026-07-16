#include "index/disk_bplus_tree.hpp"

#include <cstdint>
#include <cstring>

#include "backend/page_guard.hpp"

namespace db::index {

using vm::RecordID;
using vm::Value;
using vm::ValueType;

namespace {

constexpr int kSplitLimit = backend::PAGE_SIZE;
const RecordID kMinRid{-1, -1};

void putU16(char* d, int off, std::uint16_t v) {
    d[off] = static_cast<char>(v & 0xFF);
    d[off + 1] = static_cast<char>((v >> 8) & 0xFF);
}
std::uint16_t getU16(const char* d, int off) {
    return static_cast<std::uint16_t>(
        (static_cast<unsigned char>(d[off])) |
        (static_cast<unsigned char>(d[off + 1]) << 8));
}
void putI32(char* d, int off, std::int32_t v) {
    std::uint32_t u = static_cast<std::uint32_t>(v);
    for (int i = 0; i < 4; ++i) d[off + i] = static_cast<char>((u >> (8 * i)) & 0xFF);
}
std::int32_t getI32(const char* d, int off) {
    std::uint32_t u = 0;
    for (int i = 0; i < 4; ++i)
        u |= static_cast<std::uint32_t>(static_cast<unsigned char>(d[off + i])) << (8 * i);
    return static_cast<std::int32_t>(u);
}

std::string encodeValue(const Value& v) {
    std::string out;
    out.push_back(static_cast<char>(v.type));
    switch (v.type) {
        case ValueType::Int: {
            std::uint64_t u = static_cast<std::uint64_t>(v.intValue);
            for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
            break;
        }
        case ValueType::Double: {
            std::uint64_t u;
            std::memcpy(&u, &v.doubleValue, 8);
            for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
            break;
        }
        case ValueType::Bool:
            out.push_back(v.boolValue ? 1 : 0);
            break;
        case ValueType::Text:
            out.append(v.textValue);
            break;
        case ValueType::Null:
            break;
    }
    return out;
}

Value decodeValue(const char* p, int len) {
    Value v;
    if (len <= 0) return v;
    v.type = static_cast<ValueType>(static_cast<unsigned char>(p[0]));
    const char* body = p + 1;
    int blen = len - 1;
    switch (v.type) {
        case ValueType::Int: {
            std::uint64_t u = 0;
            for (int i = 0; i < 8 && i < blen; ++i)
                u |= static_cast<std::uint64_t>(static_cast<unsigned char>(body[i])) << (8 * i);
            v.intValue = static_cast<std::int64_t>(u);
            break;
        }
        case ValueType::Double: {
            std::uint64_t u = 0;
            for (int i = 0; i < 8 && i < blen; ++i)
                u |= static_cast<std::uint64_t>(static_cast<unsigned char>(body[i])) << (8 * i);
            std::memcpy(&v.doubleValue, &u, 8);
            break;
        }
        case ValueType::Bool:
            v.boolValue = blen > 0 && body[0] != 0;
            break;
        case ValueType::Text:
            v.textValue.assign(body, static_cast<std::size_t>(blen));
            break;
        case ValueType::Null:
            break;
    }
    return v;
}

bool ridLess(const RecordID& a, const RecordID& b) {
    if (a.pageId != b.pageId) return a.pageId < b.pageId;
    return a.slotId < b.slotId;
}

bool keyEqual(const Value& a, const Value& b) {
    return !vm::valueLess(a, b) && !vm::valueLess(b, a);
}

bool entryLess(const Value& ak, const RecordID& ar, const Value& bk,
               const RecordID& br) {
    if (vm::valueLess(ak, bk)) return true;
    if (vm::valueLess(bk, ak)) return false;
    return ridLess(ar, br);
}

}  // namespace

DiskBPlusTree::DiskBPlusTree(backend::BufferPool* pool) : pool_(pool) {}

DiskBPlusTree::NodeView DiskBPlusTree::readNode(int pageId) const {
    NodeView view;
    backend::Page* page = pool_->fetchPage(pageId);
    backend::PageGuard guard(pool_, pageId, page);
    const char* d = page->data();
    view.leaf = d[0] != 0;
    int n = getU16(d, 2);
    view.nextLeaf = getI32(d, 4);
    int pos = 8;
    view.keys.reserve(n);
    view.rids.reserve(n);
    for (int i = 0; i < n; ++i) {
        int klen = getU16(d, pos);
        pos += 2;
        view.keys.push_back(decodeValue(d + pos, klen));
        pos += klen;
        RecordID rid;
        rid.pageId = getI32(d, pos);
        rid.slotId = getI32(d, pos + 4);
        pos += 8;
        view.rids.push_back(rid);
    }
    if (!view.leaf) {
        view.children.reserve(n + 1);
        for (int i = 0; i < n + 1; ++i) {
            view.children.push_back(getI32(d, pos));
            pos += 4;
        }
    }
    return view;
}

void DiskBPlusTree::writeNode(int pageId, const NodeView& view) {
    backend::Page* page = pool_->fetchPage(pageId);
    backend::PageGuard guard(pool_, pageId, page);
    guard.markDirty();
    char* d = page->data();
    std::memset(d, 0, backend::PAGE_SIZE);
    d[0] = view.leaf ? 1 : 0;
    putU16(d, 2, static_cast<std::uint16_t>(view.keys.size()));
    putI32(d, 4, view.nextLeaf);
    int pos = 8;
    for (std::size_t i = 0; i < view.keys.size(); ++i) {
        std::string kb = encodeValue(view.keys[i]);
        putU16(d, pos, static_cast<std::uint16_t>(kb.size()));
        pos += 2;
        std::memcpy(d + pos, kb.data(), kb.size());
        pos += static_cast<int>(kb.size());
        putI32(d, pos, view.rids[i].pageId);
        putI32(d, pos + 4, view.rids[i].slotId);
        pos += 8;
    }
    if (!view.leaf) {
        for (int child : view.children) {
            putI32(d, pos, child);
            pos += 4;
        }
    }
}

int DiskBPlusTree::allocNode(const NodeView& view) {
    backend::PageId pid = -1;
    backend::Page* page = pool_->newPage(pid);
    (void)page;
    pool_->unpin(pid, false);
    writeNode(pid, view);
    return pid;
}

namespace {

std::size_t measure(const std::vector<Value>& keys, const std::vector<RecordID>& rids,
                    bool leaf) {
    std::size_t bytes = 8;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        bytes += 2 + encodeValue(keys[i]).size() + 8;
        (void)rids;
    }
    if (!leaf) bytes += (keys.size() + 1) * 4;
    return bytes;
}

}  // namespace

int DiskBPlusTree::findLeaf(const Value& key, const RecordID& rid,
                            std::vector<int>& path) const {
    int cur = rootId_;
    while (true) {
        NodeView view = readNode(cur);
        if (view.leaf) return cur;
        path.push_back(cur);
        int idx = 0;
        int n = static_cast<int>(view.keys.size());
        while (idx < n && !entryLess(key, rid, view.keys[idx], view.rids[idx])) ++idx;
        cur = view.children[idx];
    }
}

int DiskBPlusTree::firstLeaf() const {
    int cur = rootId_;
    while (true) {
        NodeView view = readNode(cur);
        if (view.leaf) return cur;
        cur = view.children.front();
    }
}

void DiskBPlusTree::insert(const Key& key, const RecordID& rid) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rootId_ == -1) {
        NodeView root;
        root.leaf = true;
        root.keys.push_back(key);
        root.rids.push_back(rid);
        rootId_ = allocNode(root);
        ++distinctKeys_;
        return;
    }

    std::vector<int> path;
    int leafId = findLeaf(key, rid, path);
    NodeView leaf = readNode(leafId);

    int pos = 0;
    int n = static_cast<int>(leaf.keys.size());
    while (pos < n && entryLess(leaf.keys[pos], leaf.rids[pos], key, rid)) ++pos;
    if (pos < n && keyEqual(leaf.keys[pos], key) && leaf.rids[pos] == rid) {
        return;
    }
    bool newKey = !(pos < n && keyEqual(leaf.keys[pos], key)) &&
                  !(pos > 0 && keyEqual(leaf.keys[pos - 1], key));
    leaf.keys.insert(leaf.keys.begin() + pos, key);
    leaf.rids.insert(leaf.rids.begin() + pos, rid);
    if (newKey) ++distinctKeys_;

    if (measure(leaf.keys, leaf.rids, true) <= kSplitLimit) {
        writeNode(leafId, leaf);
        return;
    }

    std::size_t mid = leaf.keys.size() / 2;
    NodeView right;
    right.leaf = true;
    right.keys.assign(leaf.keys.begin() + mid, leaf.keys.end());
    right.rids.assign(leaf.rids.begin() + mid, leaf.rids.end());
    right.nextLeaf = leaf.nextLeaf;
    leaf.keys.resize(mid);
    leaf.rids.resize(mid);
    int rightId = allocNode(right);
    leaf.nextLeaf = rightId;
    writeNode(leafId, leaf);

    Value upKey = right.keys.front();
    RecordID upRid = right.rids.front();
    int childId = rightId;

    while (!path.empty()) {
        int parentId = path.back();
        path.pop_back();
        NodeView parent = readNode(parentId);
        int ip = 0;
        int pn = static_cast<int>(parent.keys.size());
        while (ip < pn && entryLess(parent.keys[ip], parent.rids[ip], upKey, upRid)) ++ip;
        parent.keys.insert(parent.keys.begin() + ip, upKey);
        parent.rids.insert(parent.rids.begin() + ip, upRid);
        parent.children.insert(parent.children.begin() + ip + 1, childId);

        if (measure(parent.keys, parent.rids, false) <= kSplitLimit) {
            writeNode(parentId, parent);
            return;
        }

        std::size_t pmid = parent.keys.size() / 2;
        Value sepKey = parent.keys[pmid];
        RecordID sepRid = parent.rids[pmid];
        NodeView pright;
        pright.leaf = false;
        pright.keys.assign(parent.keys.begin() + pmid + 1, parent.keys.end());
        pright.rids.assign(parent.rids.begin() + pmid + 1, parent.rids.end());
        pright.children.assign(parent.children.begin() + pmid + 1, parent.children.end());
        parent.keys.resize(pmid);
        parent.rids.resize(pmid);
        parent.children.resize(pmid + 1);
        int prightId = allocNode(pright);
        writeNode(parentId, parent);

        upKey = sepKey;
        upRid = sepRid;
        childId = prightId;
    }

    NodeView newRoot;
    newRoot.leaf = false;
    newRoot.keys.push_back(upKey);
    newRoot.rids.push_back(upRid);
    newRoot.children.push_back(rootId_);
    newRoot.children.push_back(childId);
    rootId_ = allocNode(newRoot);
}

std::vector<RecordID> DiskBPlusTree::rangeScan(const Key* lo, const Key* hi) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RecordID> out;
    if (rootId_ == -1) return out;

    int leafId;
    if (lo != nullptr) {
        std::vector<int> path;
        leafId = findLeaf(*lo, kMinRid, path);
    } else {
        leafId = firstLeaf();
    }

    while (leafId != -1) {
        NodeView leaf = readNode(leafId);
        for (std::size_t i = 0; i < leaf.keys.size(); ++i) {
            if (lo != nullptr && vm::valueLess(leaf.keys[i], *lo)) continue;
            if (hi != nullptr && vm::valueLess(*hi, leaf.keys[i])) return out;
            out.push_back(leaf.rids[i]);
        }
        leafId = leaf.nextLeaf;
    }
    return out;
}

std::vector<RecordID> DiskBPlusTree::range(const Key& lo, const Key& hi) const {
    return rangeScan(&lo, &hi);
}

std::vector<RecordID> DiskBPlusTree::lookup(const Key& key) const {
    return rangeScan(&key, &key);
}

bool DiskBPlusTree::erase(const Key& key, const RecordID& rid) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rootId_ == -1) return false;
    std::vector<int> path;
    int leafId = findLeaf(key, rid, path);
    NodeView leaf = readNode(leafId);
    for (std::size_t i = 0; i < leaf.keys.size(); ++i) {
        if (keyEqual(leaf.keys[i], key) && leaf.rids[i] == rid) {
            bool hadPrev = i > 0 && keyEqual(leaf.keys[i - 1], key);
            bool hadNext = i + 1 < leaf.keys.size() && keyEqual(leaf.keys[i + 1], key);
            leaf.keys.erase(leaf.keys.begin() + i);
            leaf.rids.erase(leaf.rids.begin() + i);
            if (!hadPrev && !hadNext && distinctKeys_ > 0) --distinctKeys_;
            writeNode(leafId, leaf);
            return true;
        }
    }
    return false;
}

}  // namespace db::index
