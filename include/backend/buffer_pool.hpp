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

// Fixed pool of in-memory frames caching disk pages. Pages are pinned while in
// use; unpinned pages are eviction candidates managed by an LRU replacer
// (chosen over the article's random eviction for better hit rates). Dirty
// pages are written back on eviction. Thread-safe.
class BufferPool {
public:
    BufferPool(DiskManager* disk, std::size_t numFrames);

    // Pins and returns the page with id `id`, loading it from disk if needed.
    // Returns nullptr only when every frame is pinned (pool exhausted).
    Page* fetchPage(PageId id);

    // Allocates a fresh page on disk, pins it, and returns it via the pool.
    Page* newPage(PageId& outId);

    // Releases one pin on `id`, OR-ing `dirty` into its dirty flag.
    bool unpin(PageId id, bool dirty);

    // Writes a single page back to disk (if resident).
    bool flush(PageId id);

    // Writes every dirty resident page back to disk.
    void flushAll();

    // Installs a hook run just before a dirty page is written back during
    // eviction. Used to flush the write-ahead log first (WAL protocol), so a
    // stolen uncommitted page can always be undone after a crash.
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

    int findFrame(PageId id) const;   // frame index or -1
    int acquireFrame();               // free frame or LRU victim; -1 if all pinned
    void touchUnpinned(int frame);    // add to LRU tail
    void removeFromLru(int frame);

    DiskManager* disk_;
    std::vector<Frame> frames_;
    std::unordered_map<PageId, int> table_;
    std::list<int> lru_;                                       // front = least recent
    std::unordered_map<int, std::list<int>::iterator> lruPos_;
    std::vector<int> freeList_;
    std::function<void()> preEvictHook_;
    mutable std::mutex mtx_;
};

}  // namespace db::backend
