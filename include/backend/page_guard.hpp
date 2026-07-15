#pragma once

#include "backend/buffer_pool.hpp"
#include "backend/page.hpp"

namespace db::backend {

class PageGuard {
public:
    PageGuard() = default;
    PageGuard(BufferPool* pool, PageId id, Page* page)
        : pool_(pool), pageId_(id), page_(page) {}

    ~PageGuard() { drop(); }

    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;

    PageGuard(PageGuard&& other) noexcept { moveFrom(other); }
    PageGuard& operator=(PageGuard&& other) noexcept {
        if (this != &other) {
            drop();
            moveFrom(other);
        }
        return *this;
    }

    Page* page() const { return page_; }
    PageId pageId() const { return pageId_; }
    bool valid() const { return page_ != nullptr; }
    void markDirty() { dirty_ = true; }

    void drop() {
        if (pool_ != nullptr && page_ != nullptr) {
            pool_->unpin(pageId_, dirty_);
        }
        pool_ = nullptr;
        page_ = nullptr;
        dirty_ = false;
    }

private:
    void moveFrom(PageGuard& other) {
        pool_ = other.pool_;
        pageId_ = other.pageId_;
        page_ = other.page_;
        dirty_ = other.dirty_;
        other.pool_ = nullptr;
        other.page_ = nullptr;
        other.dirty_ = false;
    }

    BufferPool* pool_ = nullptr;
    PageId pageId_ = -1;
    Page* page_ = nullptr;
    bool dirty_ = false;
};

}
