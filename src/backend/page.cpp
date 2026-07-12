#include "backend/page.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace db::backend {

void Page::init() {
    std::memset(bytes_, 0, PAGE_SIZE);
    writeU16(0, 0);                                        // slotCount
    writeU16(2, static_cast<std::uint16_t>(PAGE_SIZE));    // freeSpaceOffset
}

std::uint16_t Page::readU16(int offset) const {
    std::uint16_t v;
    std::memcpy(&v, bytes_ + offset, sizeof(v));
    return v;
}

void Page::writeU16(int offset, std::uint16_t value) {
    std::memcpy(bytes_ + offset, &value, sizeof(value));
}

void Page::slotAt(int slot, std::uint16_t& offset, std::uint16_t& length) const {
    int base = HEADER_SIZE + slot * SLOT_SIZE;
    offset = readU16(base);
    length = readU16(base + 2);
}

void Page::setSlot(int slot, std::uint16_t offset, std::uint16_t length) {
    int base = HEADER_SIZE + slot * SLOT_SIZE;
    writeU16(base, offset);
    writeU16(base + 2, length);
}

int Page::slotCount() const {
    return readU16(0);
}

int Page::freeSpace() const {
    int freeStart = HEADER_SIZE + slotCount() * SLOT_SIZE;
    int freeEnd = readU16(2);
    return freeEnd - freeStart;
}

bool Page::isLive(int slot) const {
    if (slot < 0 || slot >= slotCount()) {
        return false;
    }
    std::uint16_t offset, length;
    slotAt(slot, offset, length);
    return length != 0;
}

bool Page::insert(const std::string& record, int& outSlot) {
    const int need = static_cast<int>(record.size());
    if (need == 0 || need > 0xFFFF) {
        return false;
    }
    if (freeSpace() < need + SLOT_SIZE) {
        // Not enough contiguous space. Reclaim tombstoned tuple bytes and retry
        // if the total reclaimable space would be enough.
        if (usableSpace() < need + SLOT_SIZE) {
            return false;
        }
        compact();
    }
    int count = slotCount();
    std::uint16_t freeEnd = readU16(2);
    std::uint16_t dataOffset = static_cast<std::uint16_t>(freeEnd - need);
    std::memcpy(bytes_ + dataOffset, record.data(), need);

    setSlot(count, dataOffset, static_cast<std::uint16_t>(need));
    writeU16(0, static_cast<std::uint16_t>(count + 1));
    writeU16(2, dataOffset);
    outSlot = count;
    return true;
}

int Page::usableSpace() const {
    int count = slotCount();
    int liveBytes = 0;
    for (int s = 0; s < count; ++s) {
        std::uint16_t offset, length;
        slotAt(s, offset, length);
        liveBytes += length;
    }
    int slotArrayEnd = HEADER_SIZE + count * SLOT_SIZE;
    return PAGE_SIZE - slotArrayEnd - liveBytes;
}

int Page::freeBytes() const { return usableSpace(); }

void Page::compact() {
    int count = slotCount();
    // Snapshot live tuples first (the rewrite overwrites the data region).
    std::vector<std::pair<int, std::string>> live;
    for (int s = 0; s < count; ++s) {
        std::uint16_t offset, length;
        slotAt(s, offset, length);
        if (length == 0) continue;  // tombstone
        live.emplace_back(s, std::string(bytes_ + offset, length));
    }
    std::uint16_t freeEnd = static_cast<std::uint16_t>(PAGE_SIZE);
    for (auto& [slot, data] : live) {
        freeEnd = static_cast<std::uint16_t>(freeEnd - data.size());
        std::memcpy(bytes_ + freeEnd, data.data(), data.size());
        setSlot(slot, freeEnd, static_cast<std::uint16_t>(data.size()));
    }
    writeU16(2, freeEnd);
}

bool Page::get(int slot, std::string& out) const {
    if (!isLive(slot)) {
        return false;
    }
    std::uint16_t offset, length;
    slotAt(slot, offset, length);
    out.assign(bytes_ + offset, length);
    return true;
}

bool Page::erase(int slot) {
    if (!isLive(slot)) {
        return false;
    }
    std::uint16_t offset, length;
    slotAt(slot, offset, length);
    setSlot(slot, offset, 0);  // tombstone: keep offset, zero the length
    return true;
}

bool Page::update(int slot, const std::string& record) {
    if (!isLive(slot)) {
        return false;
    }
    std::uint16_t offset, length;
    slotAt(slot, offset, length);
    const int need = static_cast<int>(record.size());
    if (need == 0 || need > length) {
        return false;  // caller must relocate
    }
    std::memcpy(bytes_ + offset, record.data(), need);
    setSlot(slot, offset, static_cast<std::uint16_t>(need));
    return true;
}

}  // namespace db::backend
