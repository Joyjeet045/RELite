#pragma once

#include <stdexcept>
#include <string>

#include "frontend/ast.hpp"
#include "frontend/catalog.hpp"

namespace db::semantic {

class SemanticError : public std::runtime_error {
public:
    explicit SemanticError(const std::string& message);
};

class SemanticAnalyzer : public parser::ASTVisitor {
public:
    explicit SemanticAnalyzer(Catalog& catalog);

    void analyze(parser::ASTNode& node);

    void bindExpression(parser::Expression& expr, const std::string& tableName);

    void visit(parser::LiteralExpr& node) override;
    void visit(parser::ColumnRef& node) override;
    void visit(parser::BinaryExpr& node) override;
    void visit(parser::ArithmeticExpr& node) override;
    void visit(parser::LogicalExpr& node) override;
    void visit(parser::UnaryExpr& node) override;
    void visit(parser::IsNullExpr& node) override;
    void visit(parser::InExpr& node) override;
    void visit(parser::BetweenExpr& node) override;
    void visit(parser::LikeExpr& node) override;
    void visit(parser::FunctionExpr& node) override;
    void visit(parser::SubqueryExpr& node) override;
    void visit(parser::CreateStatement& node) override;
    void visit(parser::CreateIdxStatement& node) override;
    void visit(parser::InsertStatement& node) override;
    void visit(parser::SelectStatement& node) override;
    void visit(parser::DeleteStatement& node) override;
    void visit(parser::UpdateStatement& node) override;
    void visit(parser::DropStatement& node) override;
    void visit(parser::AlterStatement& node) override;
    void visit(parser::TransactionStatement& node) override;

private:
    Catalog& catalog_;
    const TableSchema* currentTable_ = nullptr;
    std::string currentAlias_;

    const TableSchema* leftTable_ = nullptr;
    const TableSchema* rightTable_ = nullptr;
    std::string leftAlias_;
    std::string rightAlias_;
    int leftColumnCount_ = 0;
    bool joinMode_ = false;

    void checkPredicate(parser::Expression& expr);
};

}
