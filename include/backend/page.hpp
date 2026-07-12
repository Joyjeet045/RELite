#pragma once

#include <cstdint>
#include <string>

namespace db::backend {

constexpr int PAGE_SIZE = 4096;

using PageId = int;

// A 4KB slotted page.
//
//   offset 0 : uint16 slotCount
//   offset 2 : uint16 freeSpaceOffset  (lowest byte used by tuple data)
//   offset 4 : slot array, growing up   (each slot = uint16 offset, uint16 len)
//   ...      : free space
//   tail     : tuple data, growing down from PAGE_SIZE
//
// A slot with length 0 is a tombstone (deleted row); its id is never reused so
// that RecordIDs stay stable.
class Page {
public:
    static constexpr int HEADER_SIZE = 4;
    static constexpr int SLOT_SIZE = 4;

    Page() { init(); }

    // Resets the page to an empty state.
    void init();

    char* data() { return bytes_; }
    const char* data() const { return bytes_; }

    int slotCount() const;
    int freeSpace() const;

    // Bytes available for a new record (after reclaiming tombstoned space),
    // excluding the slot-header cost. Used as a free-space-map hint.
    int freeBytes() const;

    // Appends a record. Returns false (unchanged) when it does not fit.
    bool insert(const std::string& record, int& outSlot);

    // Reads a live record. Returns false for out-of-range or deleted slots.
    bool get(int slot, std::string& out) const;

    // Marks a slot as deleted. Returns false if already deleted / out of range.
    bool erase(int slot);

    // Overwrites a slot in place when the new record is not larger than the old
    // one. Returns false when it does not fit (caller must relocate) or the slot
    // is invalid.
    bool update(int slot, const std::string& record);

    bool isLive(int slot) const;

private:
    std::uint16_t readU16(int offset) const;
    void writeU16(int offset, std::uint16_t value);
    void slotAt(int slot, std::uint16_t& offset, std::uint16_t& length) const;
    void setSlot(int slot, std::uint16_t offset, std::uint16_t length);

    // Bytes available for a new record after reclaiming tombstoned tuple space
    // (i.e. the space a compact() would free). Excludes the new slot header.
    int usableSpace() const;

    // Rewrites live tuples contiguously toward the tail, reclaiming the space
    // held by tombstoned tuples. Slot indices are preserved so RecordIDs stay
    // valid; tombstones remain length-0 slots.
    void compact();

    char bytes_[PAGE_SIZE];
};

}  // namespace db::backend
