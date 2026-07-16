#include "vm/version_store.hpp"

namespace db::vm {

void VersionStore::stageInsert(int tableId, const RecordID& rid, std::string bytes) {
    Change c;
    c.tableId = tableId;
    c.isDelete = false;
    c.rid = rid;
    c.bytes = std::move(bytes);
    pending_.push_back(std::move(c));
    knownTables_.insert(tableId);
}

void VersionStore::stageDelete(int tableId, const RecordID& rid) {
    Change c;
    c.tableId = tableId;
    c.isDelete = true;
    c.rid = rid;
    pending_.push_back(std::move(c));
    knownTables_.insert(tableId);
}

std::uint64_t VersionStore::commitPending() {
    if (pending_.empty()) return version_;
    ++version_;
    for (auto& c : pending_) {
        c.version = version_;
        log_.push_back(std::move(c));
    }
    pending_.clear();
    maybeGc();
    return version_;
}

void VersionStore::discardPending() { pending_.clear(); }

void VersionStore::seedBaseline(int tableId,
                                std::vector<std::pair<RecordID, std::string>> rows) {
    std::vector<BaseRow>& base = baseline_[tableId];
    base.clear();
    base.reserve(rows.size());
    for (auto& r : rows) {
        base.push_back({r.first, std::move(r.second)});
    }
    knownTables_.insert(tableId);
}

void VersionStore::setRetention(std::size_t maxChanges, std::uint64_t retainVersions) {
    maxChanges_ = maxChanges;
    retainVersions_ = retainVersions;
}

std::vector<VersionStore::BaseRow> VersionStore::reconstruct(
    int tableId, std::uint64_t version) const {
    std::unordered_map<RecordID, std::size_t> pos;
    std::vector<BaseRow> rows;
    std::vector<bool> live;

    auto it = baseline_.find(tableId);
    if (it != baseline_.end()) {
        for (const BaseRow& br : it->second) {
            pos[br.rid] = rows.size();
            rows.push_back(br);
            live.push_back(true);
        }
    }
    for (const Change& c : log_) {
        if (c.version <= baselineVersion_) continue;
        if (c.version > version) break;
        if (c.tableId != tableId) continue;
        auto found = pos.find(c.rid);
        if (c.isDelete) {
            if (found != pos.end()) live[found->second] = false;
        } else if (found != pos.end()) {
            rows[found->second].bytes = c.bytes;
            live[found->second] = true;
        } else {
            pos[c.rid] = rows.size();
            rows.push_back({c.rid, c.bytes});
            live.push_back(true);
        }
    }

    std::vector<BaseRow> out;
    out.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (live[i]) out.push_back(std::move(rows[i]));
    }
    return out;
}

std::vector<std::string> VersionStore::snapshotAsOf(int tableId,
                                                    std::uint64_t version) const {
    if (version < baselineVersion_) version = baselineVersion_;
    std::vector<BaseRow> rows = reconstruct(tableId, version);
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (BaseRow& br : rows) out.push_back(std::move(br.bytes));
    return out;
}

void VersionStore::beginSnapshot(int txnId) { txnSnapshots_[txnId] = version_; }

void VersionStore::endSnapshot(int txnId) { txnSnapshots_.erase(txnId); }

bool VersionStore::snapshotVersionOf(int txnId, std::uint64_t& out) const {
    auto it = txnSnapshots_.find(txnId);
    if (it == txnSnapshots_.end()) return false;
    out = it->second;
    return true;
}

std::vector<std::string> VersionStore::snapshotForTxn(
    int tableId, std::uint64_t snapshotVersion) const {
    if (snapshotVersion < baselineVersion_) snapshotVersion = baselineVersion_;
    std::vector<BaseRow> rows = reconstruct(tableId, snapshotVersion);

    std::unordered_map<RecordID, std::size_t> pos;
    std::vector<bool> live(rows.size(), true);
    for (std::size_t i = 0; i < rows.size(); ++i) pos[rows[i].rid] = i;
    for (const Change& c : pending_) {
        if (c.tableId != tableId) continue;
        auto found = pos.find(c.rid);
        if (c.isDelete) {
            if (found != pos.end()) live[found->second] = false;
        } else if (found != pos.end()) {
            rows[found->second].bytes = c.bytes;
            live[found->second] = true;
        } else {
            pos[c.rid] = rows.size();
            rows.push_back({c.rid, c.bytes});
            live.push_back(true);
        }
    }

    std::vector<std::string> out;
    out.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (live[i]) out.push_back(std::move(rows[i].bytes));
    }
    return out;
}

void VersionStore::gc(std::uint64_t horizon) {
    if (horizon > version_) horizon = version_;
    if (horizon <= baselineVersion_) return;

    std::unordered_map<int, std::vector<BaseRow>> newBaseline;
    for (int tableId : knownTables_) {
        newBaseline[tableId] = reconstruct(tableId, horizon);
    }

    std::vector<Change> kept;
    for (Change& c : log_) {
        if (c.version > horizon) kept.push_back(std::move(c));
    }
    log_.swap(kept);
    baseline_.swap(newBaseline);
    baselineVersion_ = horizon;
}

void VersionStore::maybeGc() {
    if (log_.size() <= maxChanges_) return;
    if (version_ <= retainVersions_) return;
    gc(version_ - retainVersions_);
}

namespace {

void writeBytes(std::ostream& out, const std::string& s) {
    out << s.size() << " ";
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
    out << "\n";
}

std::string readBytes(std::istream& in) {
    long long len = 0;
    in >> len;
    in.get();
    std::string s(static_cast<std::size_t>(len < 0 ? 0 : len), '\0');
    if (len > 0) in.read(&s[0], static_cast<std::streamsize>(len));
    return s;
}

}  // namespace

void VersionStore::serialize(std::ostream& out) const {
    out << "RVS1\n";
    out << version_ << " " << baselineVersion_ << "\n";

    out << baseline_.size() << "\n";
    for (const auto& [tableId, rows] : baseline_) {
        out << tableId << " " << rows.size() << "\n";
        for (const BaseRow& br : rows) {
            out << br.rid.pageId << " " << br.rid.slotId << " ";
            writeBytes(out, br.bytes);
        }
    }

    out << log_.size() << "\n";
    for (const Change& c : log_) {
        out << c.version << " " << c.tableId << " " << (c.isDelete ? 1 : 0) << " "
            << c.rid.pageId << " " << c.rid.slotId << " ";
        writeBytes(out, c.bytes);
    }
}

void VersionStore::deserialize(std::istream& in) {
    std::string magic;
    in >> magic;
    if (magic != "RVS1") return;

    log_.clear();
    pending_.clear();
    baseline_.clear();
    knownTables_.clear();

    in >> version_ >> baselineVersion_;

    std::size_t nbase = 0;
    in >> nbase;
    for (std::size_t t = 0; t < nbase; ++t) {
        int tableId = 0;
        std::size_t nrows = 0;
        in >> tableId >> nrows;
        std::vector<BaseRow> rows;
        rows.reserve(nrows);
        for (std::size_t r = 0; r < nrows; ++r) {
            BaseRow br;
            in >> br.rid.pageId >> br.rid.slotId;
            in.get();
            br.bytes = readBytes(in);
            rows.push_back(std::move(br));
        }
        baseline_[tableId] = std::move(rows);
        knownTables_.insert(tableId);
    }

    std::size_t nlog = 0;
    in >> nlog;
    log_.reserve(nlog);
    for (std::size_t i = 0; i < nlog; ++i) {
        Change c;
        int del = 0;
        in >> c.version >> c.tableId >> del >> c.rid.pageId >> c.rid.slotId;
        c.isDelete = del != 0;
        in.get();
        c.bytes = readBytes(in);
        log_.push_back(std::move(c));
        knownTables_.insert(c.tableId);
    }
}

}

