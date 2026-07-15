#include "backend/buffer_pool.hpp"

namespace db::backend {

BufferPool::BufferPool(DiskManager* disk, std::size_t numFrames) : disk_(disk) {
    if (numFrames == 0) numFrames = 1;
    frames_.resize(numFrames);
    freeList_.reserve(numFrames);
    for (int i = static_cast<int>(numFrames) - 1; i >= 0; --i) {
        freeList_.push_back(i);
    }
}

int BufferPool::findFrame(PageId id) const {
    auto it = table_.find(id);
    return it == table_.end() ? -1 : it->second;
}

void BufferPool::touchUnpinned(int frame) {
    lru_.push_back(frame);
    lruPos_[frame] = std::prev(lru_.end());
}

void BufferPool::removeFromLru(int frame) {
    auto it = lruPos_.find(frame);
    if (it != lruPos_.end()) {
        lru_.erase(it->second);
        lruPos_.erase(it);
    }
}

int BufferPool::acquireFrame() {
    if (!freeList_.empty()) {
        int idx = freeList_.back();
        freeList_.pop_back();
        return idx;
    }
    if (lru_.empty()) {
        return -1;
    }
    int victim = lru_.front();
    lru_.pop_front();
    lruPos_.erase(victim);

    Frame& f = frames_[victim];
    if (f.valid && f.dirty) {
        if (preEvictHook_) preEvictHook_();
        disk_->writePage(f.pageId, f.page.data());
    }
    if (f.valid) {
        table_.erase(f.pageId);
    }
    f.valid = false;
    f.dirty = false;
    f.pinCount = 0;
    return victim;
}

Page* BufferPool::fetchPage(PageId id) {
    std::lock_guard<std::mutex> lock(mtx_);
    int idx = findFrame(id);
    if (idx >= 0) {
        Frame& f = frames_[idx];
        if (f.pinCount == 0) {
            removeFromLru(idx);
        }
        ++f.pinCount;
        return &f.page;
    }

    idx = acquireFrame();
    if (idx < 0) {
        return nullptr;
    }
    Frame& f = frames_[idx];
    disk_->readPage(id, f.page.data());
    f.pageId = id;
    f.pinCount = 1;
    f.dirty = false;
    f.valid = true;
    table_[id] = idx;
    return &f.page;
}

Page* BufferPool::newPage(PageId& outId) {
    std::lock_guard<std::mutex> lock(mtx_);
    PageId id = disk_->allocatePage();
    int idx = acquireFrame();
    if (idx < 0) {
        return nullptr;
    }
    Frame& f = frames_[idx];
    f.page.init();
    f.pageId = id;
    f.pinCount = 1;
    f.dirty = true;
    f.valid = true;
    table_[id] = idx;
    outId = id;
    return &f.page;
}

bool BufferPool::unpin(PageId id, bool dirty) {
    std::lock_guard<std::mutex> lock(mtx_);
    int idx = findFrame(id);
    if (idx < 0) {
        return false;
    }
    Frame& f = frames_[idx];
    if (dirty) {
        f.dirty = true;
    }
    if (f.pinCount > 0) {
        --f.pinCount;
    }
    if (f.pinCount == 0) {
        touchUnpinned(idx);
    }
    return true;
}

bool BufferPool::flush(PageId id) {
    std::lock_guard<std::mutex> lock(mtx_);
    int idx = findFrame(id);
    if (idx < 0) {
        return false;
    }
    Frame& f = frames_[idx];
    disk_->writePage(f.pageId, f.page.data());
    f.dirty = false;
    return true;
}

void BufferPool::flushAll() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (Frame& f : frames_) {
        if (f.valid && f.dirty) {
            disk_->writePage(f.pageId, f.page.data());
            f.dirty = false;
        }
    }
}

}
