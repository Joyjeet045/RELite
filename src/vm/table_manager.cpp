#include "vm/table_manager.hpp"

#include "backend/page.hpp"
#include "backend/page_guard.hpp"

namespace db::vm {

const std::vector<backend::PageId> TableManager::kEmpty;

void TableManager::registerTable(int tableId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    pages_.try_emplace(tableId);
}

bool TableManager::hasTable(int tableId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return pages_.count(tableId) != 0;
}

void TableManager::dropTable(int tableId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    pages_.erase(tableId);
}

const std::vector<backend::PageId>& TableManager::pageList(int tableId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = pages_.find(tableId);
    return it == pages_.end() ? kEmpty : it->second;
}

RecordID TableManager::insertTuple(int tableId, const std::string& bytes) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto& list = pages_[tableId];
    const int needed = static_cast<int>(bytes.size()) + backend::Page::SLOT_SIZE;

    for (auto rit = list.rbegin(); rit != list.rend(); ++rit) {
        backend::PageId pid = *rit;
        auto hint = pageFree_.find(pid);
        if (hint != pageFree_.end() && hint->second < needed) continue;
        backend::Page* page = pool_->fetchPage(pid);
        if (page == nullptr) continue;
        backend::PageGuard guard(pool_, pid, page);
        int slot;
        if (page->insert(bytes, slot)) {
            guard.markDirty();
            pageFree_[pid] = page->freeBytes();
            return RecordID{pid, slot};
        }
        pageFree_[pid] = page->freeBytes();
    }

    backend::PageId pid;
    backend::Page* page = pool_->newPage(pid);
    if (page == nullptr) {
        return RecordID{};
    }
    backend::PageGuard guard(pool_, pid, page);
    list.push_back(pid);
    int slot;
    if (page->insert(bytes, slot)) {
        guard.markDirty();
        pageFree_[pid] = page->freeBytes();
        return RecordID{pid, slot};
    }
    return RecordID{};
}

bool TableManager::getTuple(int tableId, const RecordID& rid, std::string& out) {
    (void)tableId;
    backend::Page* page = pool_->fetchPage(rid.pageId);
    if (page == nullptr) return false;
    backend::PageGuard guard(pool_, rid.pageId, page);
    return page->get(rid.slotId, out);
}

bool TableManager::eraseTuple(int tableId, const RecordID& rid) {
    (void)tableId;
    backend::Page* page = pool_->fetchPage(rid.pageId);
    if (page == nullptr) return false;
    backend::PageGuard guard(pool_, rid.pageId, page);
    if (page->erase(rid.slotId)) {
        guard.markDirty();
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pageFree_[rid.pageId] = page->freeBytes();
        return true;
    }
    return false;
}

RecordID TableManager::updateTuple(int tableId, const RecordID& rid,
                                   const std::string& bytes) {
    backend::Page* page = pool_->fetchPage(rid.pageId);
    if (page == nullptr) return RecordID{};
    {
        backend::PageGuard guard(pool_, rid.pageId, page);
        if (page->update(rid.slotId, bytes)) {
            guard.markDirty();
            return rid;
        }
        if (page->erase(rid.slotId)) {
            guard.markDirty();
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            pageFree_[rid.pageId] = page->freeBytes();
        }
    }
    return insertTuple(tableId, bytes);
}

TableIterator::TableIterator(TableManager* manager, int tableId)
    : manager_(manager), tableId_(tableId) {
    advanceToLive();
}

void TableIterator::next() {
    if (!valid_) return;
    ++slot_;
    advanceToLive();
}

void TableIterator::advanceToLive() {
    const std::vector<backend::PageId>& list = manager_->pageList(tableId_);
    backend::BufferPool* pool = manager_->pool();

    for (; pageIdx_ < list.size(); ++pageIdx_) {
        backend::PageId pid = list[pageIdx_];
        backend::Page* page = pool->fetchPage(pid);
        if (page == nullptr) {
            slot_ = -1;
            continue;
        }
        backend::PageGuard guard(pool, pid, page);
        int count = page->slotCount();
        if (slot_ < 0) slot_ = 0;
        for (; slot_ < count; ++slot_) {
            if (page->get(slot_, bytes_)) {
                rid_ = RecordID{pid, slot_};
                valid_ = true;
                return;
            }
        }
        slot_ = -1;
    }
    valid_ = false;
}

}
