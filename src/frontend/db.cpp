#include "frontend/db.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "backend/durability.hpp"
#include "backend/page.hpp"
#include "frontend/ast.hpp"
#include "frontend/catalog.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semantic_analyzer.hpp"
#include "txn/lock_manager.hpp"
#include "txn/transaction_manager.hpp"
#include "txn/wal.hpp"
#include "vm/executor_engine.hpp"
#include "vm/result_set.hpp"
#include "vm/storage_engine.hpp"
#include "vm/table_manager.hpp"
#include "vm/tuple.hpp"

namespace db {

namespace {

constexpr const char* kDefaultDbFile = "relite.db";
constexpr const char* kDefaultWalFile = "relite.wal";
constexpr const char* kMetaFile = "relite.meta";

constexpr std::size_t kCheckpointRecords = 256;

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto start = s.find_first_not_of(ws);
    if (start == std::string::npos) {
        return "";
    }
    auto end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

std::string cellText(const vm::Value& v) {
    return v.isNull() ? "NULL" : v.toString();
}

std::string formatResult(const vm::ResultSet& rs) {
    if (!rs.isQuery) {
        return rs.message + "\n";
    }

    const std::size_t ncols = rs.columns.size();
    std::vector<std::size_t> widths(ncols);
    for (std::size_t c = 0; c < ncols; ++c) {
        widths[c] = rs.columns[c].size();
    }
    for (const auto& row : rs.rows) {
        for (std::size_t c = 0; c < ncols && c < row.size(); ++c) {
            widths[c] = std::max(widths[c], cellText(row[c]).size());
        }
    }

    std::ostringstream os;
    for (std::size_t c = 0; c < ncols; ++c) {
        if (c) os << " | ";
        os << std::string(widths[c] - rs.columns[c].size(), ' ') << rs.columns[c];
    }
    os << "\n";
    for (std::size_t c = 0; c < ncols; ++c) {
        if (c) os << "-+-";
        os << std::string(widths[c], '-');
    }
    os << "\n";
    for (const auto& row : rs.rows) {
        for (std::size_t c = 0; c < ncols; ++c) {
            if (c) os << " | ";
            std::string text = c < row.size() ? cellText(row[c]) : "";
            os << std::string(widths[c] - text.size(), ' ') << text;
        }
        os << "\n";
    }
    os << "(" << rs.rows.size() << (rs.rows.size() == 1 ? " row)\n" : " rows)\n");
    return os.str();
}

}

DB::DB()
    : storage_(std::make_unique<vm::StorageEngine>(kDefaultDbFile, /*truncate=*/false)),
      wal_(std::make_unique<txn::WriteAheadLog>(kDefaultWalFile, /*truncate=*/false)),
      locks_(std::make_unique<txn::LockManager>()),
      txnMgr_(std::make_unique<txn::TransactionManager>(wal_.get(), locks_.get())) {
    storage_->bufferPool().setPreEvictHook([this] {
        if (wal_) wal_->flush();
    });
    loadCatalog();
    recover();
    rebuildIndexes();
}

DB::~DB() {
    if (txnMgr_ && currentTxn_ != 0) {
        txnMgr_->rollback(currentTxn_);
        currentTxn_ = 0;
    }
    if (storage_) {
        storage_->flush();
    }
    saveCatalog();
}

std::string DB::connect(const std::string& query) {
    parser::Lexer lexer(query);
    parser::Parser parser(lexer.tokenize());

    parser::ASTNodePtr stmt;
    try {
        stmt = parser.parseStatement();
    } catch (const parser::ParseError& e) {
        return std::string("ERROR  ") + e.what() + "\n";
    }

    auto& catalog = catalog_;
    try {
        semantic::SemanticAnalyzer analyzer(catalog);
        analyzer.analyze(*stmt);
    } catch (const semantic::SemanticError& e) {
        return std::string("ERROR  semantic error: ") + e.what() + "\n";
    }

    try {
        vm::ExecutorEngine engine(*storage_, catalog, txnMgr_.get(), &currentTxn_);
        vm::ResultSet rs = engine.run(*stmt);
        if (!rs.isQuery && currentTxn_ == 0) {
            storage_->flush();
            saveCatalog();
            if (wal_ && wal_->pendingRecords() >= kCheckpointRecords) {
                wal_->reset();
            }
        }
        return formatResult(rs);
    } catch (const std::exception& e) {
        return std::string("ERROR  runtime error: ") + e.what() + "\n";
    }
}

void DB::saveCatalog() {
    const std::string tmpPath = std::string(kMetaFile) + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::trunc | std::ios::binary);
        if (!out) return;
        out.precision(17);

        auto& cat = catalog_;
        out << "RELITE6\n";
        out << cat.nextTableId() << "\n";

        auto tables = cat.allTables();
        out << tables.size() << "\n";
        for (const semantic::TableSchema* ts : tables) {
            out << ts->tableId << " " << ts->name << " " << ts->columns.size() << "\n";
            for (const auto& c : ts->columns) {
                out << c.name << " " << static_cast<int>(c.type) << " "
                    << c.varcharLength << " " << (c.notNull ? 1 : 0) << " "
                    << (c.primaryKey ? 1 : 0) << " " << (c.unique ? 1 : 0) << " "
                    << (c.autoIncrement ? 1 : 0) << " "
                    << (c.hasDefault ? 1 : 0) << " "
                    << static_cast<int>(c.defaultValue.kind) << " "
                    << c.defaultValue.intValue << " "
                    << (c.defaultValue.boolValue ? 1 : 0) << " "
                    << c.defaultValue.doubleValue << " "
                    << c.defaultValue.stringValue.size() << " ";
                out.write(c.defaultValue.stringValue.data(),
                          static_cast<std::streamsize>(c.defaultValue.stringValue.size()));
                out << " " << c.checkSource.size() << " ";
                out.write(c.checkSource.data(),
                          static_cast<std::streamsize>(c.checkSource.size()));
                out << "\n";
            }
            out << ts->foreignKeys.size() << "\n";
            for (const auto& fk : ts->foreignKeys) {
                out << fk.columnIndex << " " << fk.refTable << " " << fk.refColumn
                    << " " << static_cast<int>(fk.onDelete) << "\n";
            }
            const auto& pages = storage_->tables().pageList(ts->tableId);
            out << pages.size();
            for (backend::PageId p : pages) out << " " << p;
            out << "\n";
        }

        auto idxs = cat.allIndexes();
        out << idxs.size() << "\n";
        for (const auto& ix : idxs) {
            out << ix.name << " " << ix.table << " " << ix.column << "\n";
        }
        out.flush();
    }
    backend::syncFileToDisk(tmpPath);

    std::error_code ec;
    std::filesystem::rename(tmpPath, kMetaFile, ec);
    if (ec) {
        std::filesystem::copy_file(
            tmpPath, kMetaFile, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmpPath, ec);
    }
}

void DB::loadCatalog() {
    std::ifstream in(kMetaFile);
    if (!in) return;
    std::string magic;
    in >> magic;
    int ver = 0;
    if (magic == "RELITE1") ver = 1;
    else if (magic == "RELITE2") ver = 2;
    else if (magic == "RELITE3") ver = 3;
    else if (magic == "RELITE4") ver = 4;
    else if (magic == "RELITE5") ver = 5;
    else if (magic == "RELITE6") ver = 6;
    else return;

    auto& cat = catalog_;
    int nextId = 0;
    in >> nextId;

    int ntables = 0;
    in >> ntables;
    for (int t = 0; t < ntables; ++t) {
        semantic::TableSchema ts;
        int ncols = 0;
        in >> ts.tableId >> ts.name >> ncols;
        for (int c = 0; c < ncols; ++c) {
            semantic::ColumnSchema col;
            int typeInt = 0;
            in >> col.name >> typeInt >> col.varcharLength;
            col.type = static_cast<parser::DataType>(typeInt);
            if (ver >= 2) {
                int nn = 0, pk = 0, uq = 0, hd = 0, dk = 0, db = 0;
                long long dint = 0, tlen = 0;
                double ddbl = 0.0;
                int ai = 0;
                in >> nn >> pk >> uq;
                if (ver >= 5) in >> ai;
                in >> hd >> dk >> dint >> db;
                if (ver >= 4) in >> ddbl;
                in >> tlen;
                col.notNull = nn != 0;
                col.primaryKey = pk != 0;
                col.unique = uq != 0;
                col.autoIncrement = ai != 0;
                col.hasDefault = hd != 0;
                col.defaultValue.kind = static_cast<parser::CachedValue::Kind>(dk);
                col.defaultValue.intValue = dint;
                col.defaultValue.boolValue = db != 0;
                col.defaultValue.doubleValue = ddbl;
                in.get();
                std::string txt(static_cast<std::size_t>(tlen), '\0');
                if (tlen > 0) in.read(&txt[0], static_cast<std::streamsize>(tlen));
                col.defaultValue.stringValue = std::move(txt);
            }
            if (ver >= 3) {
                long long clen = 0;
                in >> clen;
                in.get();
                std::string csrc(static_cast<std::size_t>(clen), '\0');
                if (clen > 0) in.read(&csrc[0], static_cast<std::streamsize>(clen));
                col.checkSource = std::move(csrc);
            }
            ts.columns.push_back(col);
        }
        int nfk = 0;
        in >> nfk;
        for (int f = 0; f < nfk; ++f) {
            semantic::ForeignKey fk;
            in >> fk.columnIndex >> fk.refTable >> fk.refColumn;
            if (ver >= 6) {
                int act = 0;
                in >> act;
                fk.onDelete = static_cast<semantic::ForeignKey::Action>(act);
            }
            ts.foreignKeys.push_back(fk);
        }
        int npages = 0;
        in >> npages;
        std::vector<backend::PageId> pages;
        for (int p = 0; p < npages; ++p) {
            int pid = 0;
            in >> pid;
            pages.push_back(pid);
        }
        cat.restoreTable(ts);
        storage_->tables().registerTable(ts.tableId);
        storage_->tables().restorePages(ts.tableId, std::move(pages));
    }
    cat.setNextTableId(nextId);

    for (const semantic::TableSchema* ts : cat.allTables()) {
        for (int c = 0; c < static_cast<int>(ts->columns.size()); ++c) {
            const std::string& src = ts->columns[c].checkSource;
            if (src.empty()) continue;
            try {
                parser::Lexer lex(src);
                parser::Parser p(lex.tokenize());
                auto expr = std::shared_ptr<parser::Expression>(p.parseWholeExpression());
                semantic::SemanticAnalyzer analyzer(cat);
                analyzer.bindExpression(*expr, ts->name);
                cat.setColumnCheckExpr(ts->name, c, std::move(expr));
            } catch (const std::exception&) {
            }
        }
    }

    int nidx = 0;
    in >> nidx;
    for (int i = 0; i < nidx; ++i) {
        std::string name, table, column;
        in >> name >> table >> column;
        cat.createIndex(name, table, column);
    }
}

void DB::rebuildIndexes() {
    auto& cat = catalog_;
    for (const auto& ix : cat.allIndexes()) {
        const semantic::TableSchema* ts = cat.getTable(ix.table);
        if (ts == nullptr) continue;
        int colIdx = ts->columnIndex(ix.column);
        index::Index* idx = storage_->indexes().create(ix.name, ts->tableId, colIdx);
        if (idx == nullptr) continue;
        vm::Schema schema;
        for (const auto& c : ts->columns) schema.push_back(c.type);
        for (vm::TableIterator it(&storage_->tables(), ts->tableId); it.valid();
             it.next()) {
            vm::Tuple tup = vm::Tuple::deserialize(it.bytes(), schema);
            if (colIdx >= 0 && colIdx < static_cast<int>(tup.size())) {
                idx->add(tup.at(colIdx), it.rid());
            }
        }
    }
}

void DB::recover() {
    std::vector<txn::LogRecord> records = wal_->readAll();
    if (records.empty()) return;

    std::unordered_set<int> committed;
    for (const auto& r : records) {
        if (r.type == txn::LogType::Commit) committed.insert(r.txnId);
    }

    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const txn::LogRecord& r = *it;
        if (committed.count(r.txnId) != 0) continue;
        if (r.type == txn::LogType::Insert) {
            storage_->tables().eraseTuple(r.tableId, r.rid);
        } else if (r.type == txn::LogType::Delete) {
            storage_->tables().insertTuple(r.tableId, r.beforeImage);
        }
    }

    storage_->flush();
    saveCatalog();
    wal_->reset();
}

void DB::run() {
    const std::string primaryPrompt = "relite=# ";
    const std::string continuationPrompt = "relite-# ";

    std::cout << "Relite 0.1.0 - a relational database from scratch.\n";
    std::cout << "Speaks the Relite query language (not SQL). Terminate statements\n";
    std::cout << "with ';'. Use \\q to quit, \\h for help.\n\n";

    std::string buffer;
    std::string line;
    bool inStatement = false;

    std::cout << primaryPrompt << std::flush;
    while (std::getline(std::cin, line)) {
        std::string trimmed = trim(line);

        if (!inStatement && !trimmed.empty() && trimmed[0] == '\\') {
            if (trimmed == "\\q") {
                break;
            }
            if (trimmed == "\\h") {
                std::cout << "Relite language (not SQL):\n"
                             "  BUILD RELATION / BUILD INDEX, DISCARD RELATION / DISCARD INDEX\n"
                             "  PUT INTO (VALUES or FETCH), MODIFY, REMOVE, RESHAPE RELATION\n"
                             "  FETCH: projections with AS aliases and arithmetic (+ - * /),\n"
                             "    WHEN (=,!=,<,<=,>,>=, AND/OR/NOT, IS [NOT] NULL,\n"
                             "    [NOT] IN, BETWEEN, LIKE), inner/LEFT/RIGHT/FULL/CROSS LINK,\n"
                             "    GROUP BY/HAVING, aggregates (COUNT/SUM/AVG/MIN/MAX),\n"
                             "    UNIQUEONLY, SORT BY, TAKE ... SKIP, subqueries.\n"
                             "  Functions: UPPER/LOWER/LENGTH/SUBSTR/TRIM, ABS/ROUND/MOD/CEIL/FLOOR,\n"
                             "    COALESCE/NULLIF, CAST(x AS type), CASE WHEN..THEN..ELSE..END.\n"
                             "  Set ops: UNION [ALL], INTERSECT, EXCEPT.  EXPLAIN FETCH ...\n"
                             "  Types: INT/BIGINT, FLOAT, BOOL, TEXT, VARCHAR(n); AUTO_INCREMENT.\n"
                             "  Transactions: START, SAVE, UNDO.\n"
                             "  \\q  quit\n"
                             "  \\h  help\n";
            } else {
                std::cout << "Unknown command: " << trimmed << "\n";
            }
            std::cout << primaryPrompt << std::flush;
            continue;
        }

        if (!buffer.empty()) {
            buffer.push_back('\n');
        }
        buffer += line;

        std::size_t semi;
        while ((semi = buffer.find(';')) != std::string::npos) {
            std::string statement = buffer.substr(0, semi);
            buffer.erase(0, semi + 1);
            std::string stmtTrimmed = trim(statement);
            if (!stmtTrimmed.empty()) {
                std::cout << connect(stmtTrimmed);
            }
        }

        inStatement = !trim(buffer).empty();
        std::cout << (inStatement ? continuationPrompt : primaryPrompt) << std::flush;
    }

    std::cout << "\nBye.\n";
}

}
