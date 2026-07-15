#include "txn/wal.hpp"

#include <cstring>
#include <ios>
#include <unordered_set>

#include "backend/durability.hpp"

namespace db::txn {

namespace {

void putU32(std::string& out, std::uint32_t v) {
    char b[4];
    std::memcpy(b, &v, 4);
    out.append(b, 4);
}
void putU64(std::string& out, std::uint64_t v) {
    char b[8];
    std::memcpy(b, &v, 8);
    out.append(b, 8);
}
void putI32(std::string& out, std::int32_t v) {
    putU32(out, static_cast<std::uint32_t>(v));
}
void putStr(std::string& out, const std::string& s) {
    putU32(out, static_cast<std::uint32_t>(s.size()));
    out.append(s);
}

std::string encode(const LogRecord& r) {
    std::string out;
    putU64(out, r.lsn);
    putI32(out, r.txnId);
    out.push_back(static_cast<char>(r.type));
    putI32(out, r.tableId);
    putI32(out, r.rid.pageId);
    putI32(out, r.rid.slotId);
    putStr(out, r.beforeImage);
    putStr(out, r.afterImage);
    std::string framed;
    putU32(framed, static_cast<std::uint32_t>(out.size()));
    framed.append(out);
    return framed;
}

}

WriteAheadLog::WriteAheadLog(const std::string& path, bool truncate) : path_(path) {
    auto mode = std::ios::in | std::ios::out | std::ios::binary;
    if (truncate) {
        std::fstream clear(path_, std::ios::out | std::ios::binary | std::ios::trunc);
    }
    file_.open(path_, mode);
    if (!file_.is_open()) {
        std::fstream create(path_, std::ios::out | std::ios::binary);
        create.close();
        file_.open(path_, mode);
    }
    auto existing = readAll();
    for (const auto& rec : existing) {
        if (rec.lsn >= nextLsn_) nextLsn_ = rec.lsn + 1;
    }
}

WriteAheadLog::~WriteAheadLog() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

lsn_t WriteAheadLog::append(LogRecord record) {
    std::lock_guard<std::mutex> lock(mtx_);
    record.lsn = nextLsn_++;
    std::string bytes = encode(record);
    file_.clear();
    file_.seekp(0, std::ios::end);
    file_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file_.flush();
    ++appendsSinceReset_;
    return record.lsn;
}

std::vector<LogRecord> WriteAheadLog::readAll() {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<LogRecord> records;
    file_.clear();
    file_.seekg(0, std::ios::beg);

    auto readN = [&](std::string& buf, std::size_t n) -> bool {
        buf.resize(n);
        file_.read(buf.data(), static_cast<std::streamsize>(n));
        return file_.gcount() == static_cast<std::streamsize>(n);
    };

    for (;;) {
        std::string lenBuf;
        if (!readN(lenBuf, 4)) break;
        std::uint32_t len;
        std::memcpy(&len, lenBuf.data(), 4);
        std::string body;
        if (!readN(body, len)) break;

        std::size_t pos = 0;
        auto u64 = [&]() { std::uint64_t v; std::memcpy(&v, body.data() + pos, 8); pos += 8; return v; };
        auto i32 = [&]() { std::int32_t v; std::memcpy(&v, body.data() + pos, 4); pos += 4; return v; };
        auto str = [&]() {
            std::uint32_t n;
            std::memcpy(&n, body.data() + pos, 4);
            pos += 4;
            std::string s = body.substr(pos, n);
            pos += n;
            return s;
        };

        LogRecord r;
        r.lsn = u64();
        r.txnId = i32();
        r.type = static_cast<LogType>(static_cast<unsigned char>(body[pos++]));
        r.tableId = i32();
        r.rid.pageId = i32();
        r.rid.slotId = i32();
        r.beforeImage = str();
        r.afterImage = str();
        records.push_back(std::move(r));
    }
    file_.clear();
    return records;
}

void WriteAheadLog::flush() {
    std::lock_guard<std::mutex> lock(mtx_);
    file_.flush();
    backend::syncFileToDisk(path_);
}

void WriteAheadLog::reset() {
    std::lock_guard<std::mutex> lock(mtx_);
    file_.close();
    { std::fstream clear(path_, std::ios::out | std::ios::binary | std::ios::trunc); }
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    backend::syncFileToDisk(path_);
    nextLsn_ = 1;
    appendsSinceReset_ = 0;
}

std::size_t WriteAheadLog::pendingRecords() const {
    return appendsSinceReset_;
}

std::vector<int> WriteAheadLog::committedTxns(const std::vector<LogRecord>& records) {
    std::unordered_set<int> committed;
    for (const auto& r : records) {
        if (r.type == LogType::Commit) {
            committed.insert(r.txnId);
        }
    }
    return std::vector<int>(committed.begin(), committed.end());
}

}
