#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "backend/disk_manager.hpp"
#include "backend/page.hpp"

namespace db::backend {

class BufferPool {
public:
    BufferPool(DiskManager* disk, std::size_t numFrames);

    Page* fetchPage(PageId id);

    Page* newPage(PageId& outId);

    bool unpin(PageId id, bool dirty);

    bool flush(PageId id);

    void flushAll();

    void setPreEvictHook(std::function<void()> hook) { preEvictHook_ = std::move(hook); }

    std::size_t frameCount() const { return frames_.size(); }

private:
    struct Frame {
        Page page;
        PageId pageId = -1;
        int pinCount = 0;
        bool dirty = false;
        bool valid = false;
    };

    int findFrame(PageId id) const;
    int acquireFrame();
    void touchUnpinned(int frame);
    void removeFromLru(int frame);

    DiskManager* disk_;
    std::vector<Frame> frames_;
    std::unordered_map<PageId, int> table_;
    std::list<int> lru_;
    std::unordered_map<int, std::list<int>::iterator> lruPos_;
    std::vector<int> freeList_;
    std::function<void()> preEvictHook_;
    mutable std::mutex mtx_;
};

}
