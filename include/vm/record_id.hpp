#pragma once

#include <cstddef>
#include <functional>

namespace db::vm {

struct RecordID {
    int pageId = -1;
    int slotId = -1;

    bool operator==(const RecordID& o) const {
        return pageId == o.pageId && slotId == o.slotId;
    }
    bool operator!=(const RecordID& o) const { return !(*this == o); }
    bool valid() const { return pageId >= 0 && slotId >= 0; }
};

}

namespace std {
template <>
struct hash<db::vm::RecordID> {
    std::size_t operator()(const db::vm::RecordID& r) const noexcept {
        return (static_cast<std::size_t>(r.pageId) << 20) ^
               static_cast<std::size_t>(r.slotId);
    }
};
}
