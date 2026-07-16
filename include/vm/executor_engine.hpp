#pragma once

#include <string>
#include <utility>
#include <vector>

#include "frontend/ast.hpp"
#include "frontend/catalog.hpp"
#include "txn/transaction_manager.hpp"
#include "vm/record_id.hpp"
#include "vm/result_set.hpp"
#include "vm/storage_engine.hpp"
#include "vm/tuple.hpp"

namespace db::vm {

class ExecutorEngine : public parser::ASTVisitor {
public:
    ExecutorEngine(StorageEngine& storage, semantic::Catalog& catalog,
                   txn::TransactionManager* txnManager = nullptr,
                   int* currentTxn = nullptr);

    ResultSet run(parser::ASTNode& statement);

    void visit(parser::CreateStatement& node) override;
    void visit(parser::CreateIdxStatement& node) override;
    void visit(parser::InsertStatement& node) override;
    void visit(parser::SelectStatement& node) override;
    void visit(parser::DeleteStatement& node) override;
    void visit(parser::UpdateStatement& node) override;
    void visit(parser::DropStatement& node) override;
    void visit(parser::AlterStatement& node) override;
    void visit(parser::TransactionStatement& node) override;
    void visit(parser::SetOpStatement& node) override;
    void visit(parser::CreateViewStatement& node) override;

    void visit(parser::LiteralExpr&) override {}
    void visit(parser::ColumnRef&) override {}
    void visit(parser::BinaryExpr&) override {}
    void visit(parser::ArithmeticExpr&) override {}
    void visit(parser::LogicalExpr&) override {}
    void visit(parser::UnaryExpr&) override {}
    void visit(parser::IsNullExpr&) override {}
    void visit(parser::InExpr&) override {}
    void visit(parser::BetweenExpr&) override {}
    void visit(parser::LikeExpr&) override {}
    void visit(parser::FunctionExpr&) override {}
    void visit(parser::CallExpr&) override {}
    void visit(parser::CaseExpr&) override {}
    void visit(parser::WindowExpr&) override {}
    void visit(parser::SubqueryExpr&) override {}

private:
    void loadSchema(int tableId, Schema& schema, std::vector<std::string>& names) const;
    bool txnActive() const { return currentTxn_ != nullptr && *currentTxn_ != 0; }

    std::vector<std::pair<RecordID, std::vector<Value>>> gatherRows(
        int tableId, const Schema& schema, parser::Expression* where);

    std::vector<std::pair<RecordID, std::vector<Value>>> gatherBaseRows(
        int tableId, const Schema& schema, parser::Expression* where);

    std::vector<std::pair<RecordID, std::vector<Value>>> joinTwo(
        const std::vector<std::pair<RecordID, std::vector<Value>>>& leftRows,
        int leftWidth,
        const std::vector<std::pair<RecordID, std::vector<Value>>>& rightRows,
        int rightWidth, parser::SelectStatement::JoinKind kind,
        parser::Expression* on);

    bool indexCandidates(parser::Expression* where, int tableId,
                         std::vector<RecordID>& rids);

    void explainSelect(parser::SelectStatement& node);
    void runWindowQuery(parser::SelectStatement& node);

    void materializeSubqueries(parser::Expression* expr);
    void materializeSubquery(parser::SubqueryExpr* sub);

    bool hasCorrelatedSubquery(parser::Expression* expr) const;
    void bindCorrelated(parser::Expression* expr, const std::vector<Value>& outerRow);
    void bindSubquery(parser::SubqueryExpr* sub, const std::vector<Value>& outerRow);

    bool parentHasValue(const std::string& refTable, const std::string& refColumn,
                        const Value& value);
    void checkForeignKeys(const semantic::TableSchema& schema,
                          const std::vector<Value>& row);
    void checkDeleteRestrict(const semantic::TableSchema& schema,
                             const std::vector<Value>& row);
    void applyReferentialActions(const semantic::TableSchema& parent,
                                 const std::vector<Value>& parentRow);

    void enforceConstraints(const semantic::TableSchema& schema, int tableId,
                            const std::vector<Value>& row, const RecordID* excludeRid);
    bool valueExists(int tableId, int columnIndex, const Value& value,
                     const semantic::TableSchema& schema, const RecordID* excludeRid);

    void lockOrThrow(const RecordID& rid, bool exclusive);

    StorageEngine& storage_;
    semantic::Catalog& catalog_;
    txn::TransactionManager* txnMgr_;
    int* currentTxn_;
    ResultSet result_;
};

}
