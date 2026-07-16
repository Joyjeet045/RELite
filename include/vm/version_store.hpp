#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "vm/record_id.hpp"

namespace db::vm {

/*
 * Append-only multiversion store that powers snapshot reads and AS OF time
 * travel. Every committed write statement (autocommit) or transaction advances
 * a global logical version. Row images are retained as serialized bytes keyed
 * by RecordID.
 *
 * History older than a garbage-collection horizon is compacted into a per-table
 * baseline snapshot, which bounds memory for long-running sessions; AS OF a
 * version at or after the horizon is exact, while a version before it is clamped
 * to the baseline. The whole store can be (de)serialized so time travel and
 * snapshot isolation survive a clean restart.
 */
class VersionStore {
public:
    void stageInsert(int tableId, const RecordID& rid, std::string bytes);
    void stageDelete(int tableId, const RecordID& rid);

    std::uint64_t commitPending();
    void discardPending();

    std::uint64_t currentVersion() const { return version_; }
    std::uint64_t baselineVersion() const { return baselineVersion_; }
    std::size_t changeCount() const { return log_.size(); }

    std::vector<std::string> snapshotAsOf(int tableId, std::uint64_t version) const;

    /* Seed a table's baseline from its current rows (used at load time when a
     * table has no persisted version history). */
    void seedBaseline(int tableId,
                      std::vector<std::pair<RecordID, std::string>> rows);
    bool hasHistory(int tableId) const { return knownTables_.count(tableId) != 0; }

    /* Compact all history at or below `horizon` into the per-table baseline. */
    void gc(std::uint64_t horizon);
    void setRetention(std::size_t maxChanges, std::uint64_t retainVersions);

    void serialize(std::ostream& out) const;
    void deserialize(std::istream& in);

private:
    struct Change {
        std::uint64_t version = 0;
        int tableId = 0;
        bool isDelete = false;
        RecordID rid;
        std::string bytes;
    };
    struct BaseRow {
        RecordID rid;
        std::string bytes;
    };

    std::vector<BaseRow> reconstruct(int tableId, std::uint64_t version) const;
    void maybeGc();

    std::vector<Change> log_;
    std::vector<Change> pending_;
    std::unordered_map<int, std::vector<BaseRow>> baseline_;
    std::unordered_set<int> knownTables_;
    std::uint64_t baselineVersion_ = 0;
    std::uint64_t version_ = 0;
    std::size_t maxChanges_ = 2000000;
    std::uint64_t retainVersions_ = 200000;
};

}
