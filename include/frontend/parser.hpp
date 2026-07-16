#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "frontend/ast.hpp"
#include "frontend/token.hpp"

namespace db::parser {

class ParseError : public std::runtime_error {
public:
    ParseError(std::string message, int line, int column);

    int line() const { return line_; }
    int column() const { return column_; }

private:
    int line_;
    int column_;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    ASTNodePtr parseStatement();

    ExpressionPtr parseWholeExpression();

private:
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;

    const Token& peek() const;
    const Token& peekAt(std::size_t offset) const;
    const Token& previous() const;
    bool isAtEnd() const;
    const Token& advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    const Token& consume(TokenType type, const std::string& what);
    [[noreturn]] void error(const Token& tok, const std::string& message) const;

    std::int64_t toInt64(const Token& tok) const;
    std::unique_ptr<LiteralExpr> makeLiteral(const Token& tok);

    ASTNodePtr parseCreate();
    ASTNodePtr parseInsert();
    ASTNodePtr parseSelect();
    ASTNodePtr parseSelectStatement();
    ASTNodePtr parseDelete();
    ASTNodePtr parseUpdate();
    ASTNodePtr parseDrop();
    ASTNodePtr parseAlter();
    ASTNodePtr parseTransaction();

    ColumnDefinition parseColumnDefinition();
    std::unique_ptr<ColumnRef> parseColumnRef();
    std::string parseOptionalAlias();
    ExpressionPtr parseCase();
    ExpressionPtr parseCall();
    DataType parseCastType();
    std::unique_ptr<SelectStatement> parseSubquery();
    ExpressionPtr parseLiteral();

    ExpressionPtr parseExpression();
    ExpressionPtr parseOr();
    ExpressionPtr parseAnd();
    ExpressionPtr parseNot();
    ExpressionPtr parseComparison();
    ExpressionPtr parseAdditive();
    ExpressionPtr parseMultiplicative();
    ExpressionPtr parseUnary();
    ExpressionPtr parsePrimary();
};

}
