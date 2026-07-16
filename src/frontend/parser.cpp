#include "frontend/parser.hpp"

#include <cctype>
#include <string>
#include <utility>

namespace db::parser {

namespace {

bool isComparisonToken(TokenType type) {
    switch (type) {
        case TokenType::EQ:
        case TokenType::NEQ:
        case TokenType::LT:
        case TokenType::LEQ:
        case TokenType::GT:
        case TokenType::GEQ:
            return true;
        default:
            return false;
    }
}

ComparisonOp toComparisonOp(TokenType type) {
    switch (type) {
        case TokenType::EQ: return ComparisonOp::Eq;
        case TokenType::NEQ: return ComparisonOp::Neq;
        case TokenType::LT: return ComparisonOp::Lt;
        case TokenType::LEQ: return ComparisonOp::Leq;
        case TokenType::GT: return ComparisonOp::Gt;
        case TokenType::GEQ: return ComparisonOp::Geq;
        default: return ComparisonOp::Eq;
    }
}

bool isLiteralToken(TokenType type) {
    switch (type) {
        case TokenType::INTEGER_LITERAL:
        case TokenType::FLOAT_LITERAL:
        case TokenType::STRING_LITERAL:
        case TokenType::TRUE:
        case TokenType::FALSE:
        case TokenType::NULL_LITERAL:
            return true;
        default:
            return false;
    }
}

std::string upperName(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool isAggregateName(const std::string& name) {
    std::string u = upperName(name);
    return u == "COUNT" || u == "SUM" || u == "AVG" || u == "MIN" || u == "MAX";
}

}

ParseError::ParseError(std::string message, int line, int column)
    : std::runtime_error(std::move(message)), line_(line), column_(column) {}

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

const Token& Parser::peek() const { return tokens_[pos_]; }

const Token& Parser::peekAt(std::size_t offset) const {
    std::size_t idx = pos_ + offset;
    if (idx >= tokens_.size()) {
        return tokens_.back();
    }
    return tokens_[idx];
}

const Token& Parser::previous() const { return tokens_[pos_ - 1]; }

bool Parser::isAtEnd() const { return peek().type == TokenType::END_OF_FILE; }

const Token& Parser::advance() {
    if (!isAtEnd()) {
        ++pos_;
    }
    return previous();
}

bool Parser::check(TokenType type) const { return peek().type == type; }

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

const Token& Parser::consume(TokenType type, const std::string& what) {
    if (check(type)) {
        return advance();
    }
    error(peek(), "expected " + what);
}

void Parser::error(const Token& tok, const std::string& message) const {
    std::string full = "syntax error at line " + std::to_string(tok.line) +
                       ", column " + std::to_string(tok.column) + ": " + message;
    if (tok.type == TokenType::END_OF_FILE) {
        full += " (reached end of input)";
    } else if (!tok.lexeme.empty()) {
        full += " (near '" + tok.lexeme + "')";
    }
    throw ParseError(std::move(full), tok.line, tok.column);
}

std::int64_t Parser::toInt64(const Token& tok) const {
    try {
        return static_cast<std::int64_t>(std::stoll(tok.lexeme));
    } catch (const std::exception&) {
        error(tok, "integer literal out of range");
    }
}

std::unique_ptr<LiteralExpr> Parser::makeLiteral(const Token& tok) {
    auto lit = std::make_unique<LiteralExpr>();
    switch (tok.type) {
        case TokenType::INTEGER_LITERAL:
            lit->kind = LiteralExpr::Kind::Integer;
            lit->intValue = toInt64(tok);
            break;
        case TokenType::FLOAT_LITERAL:
            lit->kind = LiteralExpr::Kind::Float;
            lit->doubleValue = std::stod(tok.lexeme);
            break;
        case TokenType::STRING_LITERAL:
            lit->kind = LiteralExpr::Kind::String;
            lit->stringValue = tok.lexeme;
            break;
        case TokenType::TRUE:
            lit->kind = LiteralExpr::Kind::Boolean;
            lit->boolValue = true;
            break;
        case TokenType::FALSE:
            lit->kind = LiteralExpr::Kind::Boolean;
            lit->boolValue = false;
            break;
        case TokenType::NULL_LITERAL:
            lit->kind = LiteralExpr::Kind::Null;
            break;
        default:
            error(tok, "expected a literal value");
    }
    return lit;
}

ASTNodePtr Parser::parseStatement() {
    const Token& t = peek();
    ASTNodePtr stmt;
    switch (t.type) {
        case TokenType::CREATE: stmt = parseCreate(); break;
        case TokenType::INSERT: stmt = parseInsert(); break;
        case TokenType::SELECT: stmt = parseSelectStatement(); break;
        case TokenType::EXPLAIN: {
            advance();
            stmt = parseSelect();
            if (auto* s = dynamic_cast<SelectStatement*>(stmt.get())) {
                s->explain = true;
            }
            break;
        }
        case TokenType::DELETE: stmt = parseDelete(); break;
        case TokenType::UPDATE: stmt = parseUpdate(); break;
        case TokenType::DROP: stmt = parseDrop(); break;
        case TokenType::ALTER: stmt = parseAlter(); break;
        case TokenType::BEGIN:
        case TokenType::COMMIT:
        case TokenType::ROLLBACK: stmt = parseTransaction(); break;
        default:
            error(t, "expected a statement (BUILD, PUT, FETCH, REMOVE, MODIFY, DISCARD, START, SAVE, or UNDO)");
    }

    match(TokenType::SEMICOLON);
    if (!isAtEnd()) {
        error(peek(), "unexpected token after statement");
    }
    return stmt;
}

ExpressionPtr Parser::parseWholeExpression() {
    ExpressionPtr e = parseExpression();
    match(TokenType::SEMICOLON);
    if (!isAtEnd()) {
        error(peek(), "unexpected token after expression");
    }
    return e;
}

ASTNodePtr Parser::parseCreate() {
    consume(TokenType::CREATE, "CREATE");

    if (match(TokenType::TABLE)) {
        auto stmt = std::make_unique<CreateStatement>();
        stmt->table = consume(TokenType::IDENTIFIER, "table name").lexeme;
        consume(TokenType::LPAREN, "'('");
        do {
            stmt->columns.push_back(parseColumnDefinition());
        } while (match(TokenType::COMMA));
        consume(TokenType::RPAREN, "')'");
        return stmt;
    }

    if (match(TokenType::INDEX)) {
        auto stmt = std::make_unique<CreateIdxStatement>();
        stmt->indexName = consume(TokenType::IDENTIFIER, "index name").lexeme;
        consume(TokenType::ON, "ON");
        stmt->table = consume(TokenType::IDENTIFIER, "table name").lexeme;
        consume(TokenType::LPAREN, "'('");
        stmt->column = consume(TokenType::IDENTIFIER, "column name").lexeme;
        consume(TokenType::RPAREN, "')'");
        return stmt;
    }

    error(peek(), "expected RELATION or INDEX after BUILD");
}

ColumnDefinition Parser::parseColumnDefinition() {
    ColumnDefinition def;
    def.name = consume(TokenType::IDENTIFIER, "column name").lexeme;

    const Token& typeTok = advance();
    switch (typeTok.type) {
        case TokenType::INT_TYPE: def.type = DataType::Int; break;
        case TokenType::BOOL_TYPE: def.type = DataType::Bool; break;
        case TokenType::TEXT_TYPE: def.type = DataType::Text; break;
        case TokenType::FLOAT_TYPE: def.type = DataType::Float; break;
        case TokenType::VARCHAR:
            def.type = DataType::Varchar;
            if (match(TokenType::LPAREN)) {
                const Token& lenTok = consume(TokenType::INTEGER_LITERAL, "VARCHAR length");
                def.varcharLength = static_cast<int>(toInt64(lenTok));
                consume(TokenType::RPAREN, "')'");
            }
            break;
        default:
            error(typeTok, "expected a column type (INT, BOOL, TEXT, VARCHAR)");
    }

    for (;;) {
        if (match(TokenType::PRIMARY)) {
            consume(TokenType::KEY, "KEY");
            def.primaryKey = true;
        } else if (match(TokenType::UNIQUE)) {
            def.unique = true;
        } else if (match(TokenType::AUTOINCR)) {
            def.autoIncrement = true;
        } else if (match(TokenType::NOT)) {
            consume(TokenType::NULL_LITERAL, "NULL");
            def.notNull = true;
        } else if (match(TokenType::DEFAULT)) {
            const Token& lit = peek();
            def.hasDefault = true;
            switch (lit.type) {
                case TokenType::INTEGER_LITERAL:
                    def.defaultValue.kind = CachedValue::Kind::Int;
                    def.defaultValue.intValue = toInt64(lit);
                    break;
                case TokenType::FLOAT_LITERAL:
                    def.defaultValue.kind = CachedValue::Kind::Float;
                    def.defaultValue.doubleValue = std::stod(lit.lexeme);
                    break;
                case TokenType::STRING_LITERAL:
                    def.defaultValue.kind = CachedValue::Kind::Text;
                    def.defaultValue.stringValue = lit.lexeme;
                    break;
                case TokenType::TRUE:
                    def.defaultValue.kind = CachedValue::Kind::Bool;
                    def.defaultValue.boolValue = true;
                    break;
                case TokenType::FALSE:
                    def.defaultValue.kind = CachedValue::Kind::Bool;
                    def.defaultValue.boolValue = false;
                    break;
                case TokenType::NULL_LITERAL:
                    def.defaultValue.kind = CachedValue::Kind::Null;
                    break;
                default:
                    error(lit, "expected a literal after DEFAULT");
            }
            advance();
        } else if (match(TokenType::REFERENCES)) {
            def.refTable = consume(TokenType::IDENTIFIER, "referenced table").lexeme;
            consume(TokenType::LPAREN, "'('");
            def.refColumn = consume(TokenType::IDENTIFIER, "referenced column").lexeme;
            consume(TokenType::RPAREN, "')'");
            if (match(TokenType::ON)) {
                consume(TokenType::DELETE, "REMOVE");
                if (match(TokenType::CASCADE)) {
                    def.refOnDelete = 1;
                } else if (match(TokenType::SET)) {
                    consume(TokenType::NULL_LITERAL, "NULL");
                    def.refOnDelete = 2;
                } else {
                    consume(TokenType::RESTRICT, "RESTRICT");
                    def.refOnDelete = 0;
                }
            }
        } else if (match(TokenType::CHECK)) {
            consume(TokenType::LPAREN, "'('");
            ExpressionPtr e = parseExpression();
            consume(TokenType::RPAREN, "')'");
            def.checkExpr = std::shared_ptr<Expression>(std::move(e));
        } else {
            break;
        }
    }
    return def;
}

ASTNodePtr Parser::parseInsert() {
    consume(TokenType::INSERT, "INSERT");
    consume(TokenType::INTO, "INTO");

    auto stmt = std::make_unique<InsertStatement>();
    stmt->table = consume(TokenType::IDENTIFIER, "table name").lexeme;

    if (match(TokenType::LPAREN)) {
        do {
            stmt->columns.push_back(consume(TokenType::IDENTIFIER, "column name").lexeme);
        } while (match(TokenType::COMMA));
        consume(TokenType::RPAREN, "')'");
    }

    if (check(TokenType::SELECT)) {
        auto sel = parseSelect();
        stmt->select = std::unique_ptr<SelectStatement>(
            dynamic_cast<SelectStatement*>(sel.release()));
        return stmt;
    }

    consume(TokenType::VALUES, "VALUES");
    do {
        consume(TokenType::LPAREN, "'('");
        std::vector<ExpressionPtr> row;
        do {
            row.push_back(parseLiteral());
        } while (match(TokenType::COMMA));
        consume(TokenType::RPAREN, "')'");
        stmt->rows.push_back(std::move(row));
    } while (match(TokenType::COMMA));

    return stmt;
}

ASTNodePtr Parser::parseSelectStatement() {
    ASTNodePtr node = parseSelect();
    while (check(TokenType::UNION) || check(TokenType::INTERSECT) ||
           check(TokenType::EXCEPT)) {
        auto setop = std::make_unique<SetOpStatement>();
        if (match(TokenType::UNION)) {
            setop->op = SetOpStatement::Op::Union;
            setop->all = match(TokenType::ALL);
        } else if (match(TokenType::INTERSECT)) {
            setop->op = SetOpStatement::Op::Intersect;
        } else {
            consume(TokenType::EXCEPT, "EXCEPT");
            setop->op = SetOpStatement::Op::Except;
        }
        ASTNodePtr rightNode = parseSelect();
        auto* rsel = dynamic_cast<SelectStatement*>(rightNode.get());
        if (rsel == nullptr) {
            error(peek(), "expected FETCH after a set operator");
        }
        rightNode.release();
        setop->right = std::unique_ptr<SelectStatement>(rsel);
        setop->left = std::move(node);
        node = std::move(setop);
    }
    return node;
}

ASTNodePtr Parser::parseSelect() {
    consume(TokenType::SELECT, "SELECT");
    auto stmt = std::make_unique<SelectStatement>();

    if (match(TokenType::DISTINCT)) {
        stmt->distinct = true;
    }

    if (match(TokenType::STAR)) {
        stmt->selectStar = true;
    } else {
        do {
            if (check(TokenType::IDENTIFIER) && peekAt(1).type == TokenType::LPAREN &&
                isAggregateName(peek().lexeme)) {
                auto fn = parseAggregate();
                if (match(TokenType::AS)) {
                    fn->alias = consume(TokenType::IDENTIFIER, "alias").lexeme;
                }
                stmt->aggregates.push_back(std::move(fn));
            } else {
                ExpressionPtr e = parseAdditive();
                Expression* raw = e.release();
                std::unique_ptr<ColumnRef> item;
                if (auto* cr = dynamic_cast<ColumnRef*>(raw)) {
                    item = std::unique_ptr<ColumnRef>(cr);
                } else {
                    item = std::make_unique<ColumnRef>();
                    item->computed = ExpressionPtr(raw);
                    item->column = expressionToString(*item->computed);
                }
                if (match(TokenType::AS)) {
                    item->alias = consume(TokenType::IDENTIFIER, "alias").lexeme;
                }
                stmt->columns.push_back(std::move(item));
            }
        } while (match(TokenType::COMMA));
    }

    consume(TokenType::FROM, "FROM");
    stmt->table = consume(TokenType::IDENTIFIER, "table name").lexeme;
    stmt->tableAlias = parseOptionalAlias();

    if (match(TokenType::LEFT)) {
        consume(TokenType::JOIN, "LINK");
        stmt->joinType = SelectStatement::JoinKind::Left;
        stmt->joinTable = consume(TokenType::IDENTIFIER, "table name").lexeme;
        stmt->joinTableAlias = parseOptionalAlias();
        consume(TokenType::ON, "ON");
        stmt->joinOn = parseExpression();
    } else if (match(TokenType::RIGHT)) {
        consume(TokenType::JOIN, "LINK");
        stmt->joinType = SelectStatement::JoinKind::Right;
        stmt->joinTable = consume(TokenType::IDENTIFIER, "table name").lexeme;
        stmt->joinTableAlias = parseOptionalAlias();
        consume(TokenType::ON, "ON");
        stmt->joinOn = parseExpression();
    } else if (match(TokenType::FULL)) {
        consume(TokenType::JOIN, "LINK");
        stmt->joinType = SelectStatement::JoinKind::Full;
        stmt->joinTable = consume(TokenType::IDENTIFIER, "table name").lexeme;
        stmt->joinTableAlias = parseOptionalAlias();
        consume(TokenType::ON, "ON");
        stmt->joinOn = parseExpression();
    } else if (match(TokenType::CROSS)) {
        consume(TokenType::JOIN, "LINK");
        stmt->joinType = SelectStatement::JoinKind::Cross;
        stmt->joinTable = consume(TokenType::IDENTIFIER, "table name").lexeme;
        stmt->joinTableAlias = parseOptionalAlias();
    } else if (match(TokenType::JOIN)) {
        stmt->joinType = SelectStatement::JoinKind::Inner;
        stmt->joinTable = consume(TokenType::IDENTIFIER, "table name").lexeme;
        stmt->joinTableAlias = parseOptionalAlias();
        consume(TokenType::ON, "ON");
        stmt->joinOn = parseExpression();
    }

    if (match(TokenType::WHERE)) {
        stmt->where = parseExpression();
    }
    if (match(TokenType::GROUP)) {
        consume(TokenType::BY, "BY");
        do {
            stmt->groupBy.push_back(parseColumnRef());
        } while (match(TokenType::COMMA));
    }
    if (match(TokenType::HAVING)) {
        stmt->having = parseExpression();
    }
    if (match(TokenType::ORDER)) {
        consume(TokenType::BY, "BY");
        do {
            SelectStatement::OrderKey key;
            key.column = parseColumnRef();
            if (match(TokenType::ASC)) {
                key.ascending = true;
            } else if (match(TokenType::DESC)) {
                key.ascending = false;
            }
            stmt->orderBy.push_back(std::move(key));
        } while (match(TokenType::COMMA));
    }
    if (match(TokenType::LIMIT)) {
        const Token& n = consume(TokenType::INTEGER_LITERAL, "LIMIT count");
        stmt->hasLimit = true;
        stmt->limit = toInt64(n);
    }
    if (match(TokenType::OFFSET)) {
        const Token& m = consume(TokenType::INTEGER_LITERAL, "SKIP count");
        stmt->offset = toInt64(m);
    }
    return stmt;
}

ASTNodePtr Parser::parseDelete() {
    consume(TokenType::DELETE, "DELETE");
    consume(TokenType::FROM, "FROM");

    auto stmt = std::make_unique<DeleteStatement>();
    stmt->table = consume(TokenType::IDENTIFIER, "table name").lexeme;

    if (match(TokenType::WHERE)) {
        stmt->where = parseExpression();
    }
    return stmt;
}

ASTNodePtr Parser::parseUpdate() {
    consume(TokenType::UPDATE, "UPDATE");
    auto stmt = std::make_unique<UpdateStatement>();
    stmt->table = consume(TokenType::IDENTIFIER, "table name").lexeme;
    consume(TokenType::SET, "SET");
    do {
        std::string col = consume(TokenType::IDENTIFIER, "column name").lexeme;
        consume(TokenType::EQ, "'='");
        ExpressionPtr val = parseExpression();
        stmt->targetColumns.push_back(std::move(col));
        stmt->values.push_back(std::move(val));
    } while (match(TokenType::COMMA));
    if (match(TokenType::WHERE)) {
        stmt->where = parseExpression();
    }
    return stmt;
}

ASTNodePtr Parser::parseDrop() {
    consume(TokenType::DROP, "DROP");
    auto stmt = std::make_unique<DropStatement>();
    if (match(TokenType::TABLE)) {
        stmt->isIndex = false;
        stmt->name = consume(TokenType::IDENTIFIER, "table name").lexeme;
    } else if (match(TokenType::INDEX)) {
        stmt->isIndex = true;
        stmt->name = consume(TokenType::IDENTIFIER, "index name").lexeme;
    } else {
        error(peek(), "expected RELATION or INDEX after DISCARD");
    }
    return stmt;
}

ASTNodePtr Parser::parseAlter() {
    consume(TokenType::ALTER, "ALTER");
    consume(TokenType::TABLE, "TABLE");
    auto stmt = std::make_unique<AlterStatement>();
    stmt->table = consume(TokenType::IDENTIFIER, "table name").lexeme;
    if (match(TokenType::ADD)) {
        match(TokenType::COLUMN);
        stmt->kind = AlterStatement::Kind::AddColumn;
        stmt->column = parseColumnDefinition();
    } else if (match(TokenType::DROP)) {
        match(TokenType::COLUMN);
        stmt->kind = AlterStatement::Kind::DropColumn;
        stmt->dropColumn = consume(TokenType::IDENTIFIER, "column name").lexeme;
    } else {
        error(peek(), "expected ADD or DISCARD after RESHAPE RELATION <name>");
    }
    return stmt;
}

std::unique_ptr<SelectStatement> Parser::parseSubquery() {
    ASTNodePtr node = parseSelect();
    return std::unique_ptr<SelectStatement>(
        static_cast<SelectStatement*>(node.release()));
}

ASTNodePtr Parser::parseTransaction() {
    auto stmt = std::make_unique<TransactionStatement>();
    const Token& t = advance();
    switch (t.type) {
        case TokenType::BEGIN:
            stmt->kind = TransactionStatement::Kind::Begin;
            match(TokenType::TRANSACTION);
            break;
        case TokenType::COMMIT:
            stmt->kind = TransactionStatement::Kind::Commit;
            break;
        case TokenType::ROLLBACK:
            stmt->kind = TransactionStatement::Kind::Rollback;
            break;
        default:
            error(t, "expected START, SAVE, or UNDO");
    }
    return stmt;
}

std::unique_ptr<ColumnRef> Parser::parseColumnRef() {
    auto ref = std::make_unique<ColumnRef>();
    std::string first = consume(TokenType::IDENTIFIER, "column name").lexeme;
    if (match(TokenType::DOT)) {
        ref->table = std::move(first);
        ref->column = consume(TokenType::IDENTIFIER, "column name").lexeme;
    } else {
        ref->column = std::move(first);
    }
    return ref;
}

std::unique_ptr<FunctionExpr> Parser::parseAggregate() {
    auto fn = std::make_unique<FunctionExpr>();
    fn->name = upperName(advance().lexeme);
    consume(TokenType::LPAREN, "'('");
    if (match(TokenType::DISTINCT)) {
        fn->distinct = true;
    }
    if (match(TokenType::STAR)) {
        fn->star = true;
    } else {
        fn->argument = parseColumnRef();
    }
    consume(TokenType::RPAREN, "')'");
    return fn;
}

std::string Parser::parseOptionalAlias() {
    if (match(TokenType::AS)) {
        return consume(TokenType::IDENTIFIER, "alias").lexeme;
    }
    if (check(TokenType::IDENTIFIER)) {
        return advance().lexeme;
    }
    return "";
}

DataType Parser::parseCastType() {
    const Token& t = peek();
    switch (t.type) {
        case TokenType::INT_TYPE: advance(); return DataType::Int;
        case TokenType::FLOAT_TYPE: advance(); return DataType::Float;
        case TokenType::BOOL_TYPE: advance(); return DataType::Bool;
        case TokenType::TEXT_TYPE: advance(); return DataType::Text;
        case TokenType::VARCHAR:
            advance();
            if (match(TokenType::LPAREN)) {
                consume(TokenType::INTEGER_LITERAL, "VARCHAR length");
                consume(TokenType::RPAREN, "')'");
            }
            return DataType::Varchar;
        default:
            error(t, "expected a type in CAST");
    }
}

ExpressionPtr Parser::parseCall() {
    std::string name = upperName(consume(TokenType::IDENTIFIER, "function name").lexeme);
    consume(TokenType::LPAREN, "'('");
    auto call = std::make_unique<CallExpr>();
    call->name = name;
    if (name == "CAST") {
        call->isCast = true;
        call->args.push_back(parseExpression());
        consume(TokenType::AS, "AS");
        call->castType = parseCastType();
        consume(TokenType::RPAREN, "')'");
        return call;
    }
    if (!check(TokenType::RPAREN)) {
        do {
            call->args.push_back(parseExpression());
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "')'");
    return call;
}

ExpressionPtr Parser::parseCase() {
    consume(TokenType::CASE, "CASE");
    auto node = std::make_unique<CaseExpr>();
    while (match(TokenType::WHERE)) {
        CaseExpr::Branch branch;
        branch.when = parseExpression();
        consume(TokenType::THEN, "THEN");
        branch.then = parseAdditive();
        node->branches.push_back(std::move(branch));
    }
    if (node->branches.empty()) {
        error(peek(), "CASE requires at least one WHEN branch");
    }
    if (match(TokenType::ELSE)) {
        node->elseExpr = parseAdditive();
    }
    consume(TokenType::END, "END");
    return node;
}

ExpressionPtr Parser::parseLiteral() {
    bool negate = false;
    if ((check(TokenType::MINUS) || check(TokenType::PLUS)) &&
        (peekAt(1).type == TokenType::INTEGER_LITERAL ||
         peekAt(1).type == TokenType::FLOAT_LITERAL)) {
        negate = check(TokenType::MINUS);
        advance();
    }
    const Token& t = peek();
    if (!isLiteralToken(t.type)) {
        error(t, "expected a literal value");
    }
    advance();
    auto lit = makeLiteral(t);
    if (negate) {
        if (lit->kind == LiteralExpr::Kind::Integer) lit->intValue = -lit->intValue;
        else if (lit->kind == LiteralExpr::Kind::Float) lit->doubleValue = -lit->doubleValue;
    }
    return lit;
}

ExpressionPtr Parser::parseExpression() { return parseOr(); }

ExpressionPtr Parser::parseOr() {
    ExpressionPtr expr = parseAnd();
    while (match(TokenType::OR)) {
        ExpressionPtr right = parseAnd();
        auto node = std::make_unique<LogicalExpr>();
        node->op = LogicalOp::Or;
        node->left = std::move(expr);
        node->right = std::move(right);
        expr = std::move(node);
    }
    return expr;
}

ExpressionPtr Parser::parseAnd() {
    ExpressionPtr expr = parseNot();
    while (match(TokenType::AND)) {
        ExpressionPtr right = parseNot();
        auto node = std::make_unique<LogicalExpr>();
        node->op = LogicalOp::And;
        node->left = std::move(expr);
        node->right = std::move(right);
        expr = std::move(node);
    }
    return expr;
}

ExpressionPtr Parser::parseNot() {
    if (match(TokenType::NOT)) {
        auto node = std::make_unique<UnaryExpr>();
        node->operand = parseNot();
        return node;
    }
    return parseComparison();
}

ExpressionPtr Parser::parseComparison() {
    ExpressionPtr left = parseAdditive();

    if (match(TokenType::IS)) {
        bool negated = match(TokenType::NOT);
        consume(TokenType::NULL_LITERAL, "NULL");
        auto node = std::make_unique<IsNullExpr>();
        node->operand = std::move(left);
        node->negated = negated;
        return node;
    }

    bool negated = false;
    if (check(TokenType::NOT) &&
        (peekAt(1).type == TokenType::IN || peekAt(1).type == TokenType::BETWEEN ||
         peekAt(1).type == TokenType::LIKE)) {
        advance();
        negated = true;
    }
    if (match(TokenType::IN)) {
        consume(TokenType::LPAREN, "'('");
        auto node = std::make_unique<InExpr>();
        node->value = std::move(left);
        node->negated = negated;
        if (check(TokenType::SELECT)) {
            auto sub = std::make_unique<SubqueryExpr>();
            sub->kind = SubqueryExpr::Kind::Scalar;
            sub->query = parseSubquery();
            node->subquery = std::move(sub);
        } else {
            do {
                node->items.push_back(parseLiteral());
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "')'");
        return node;
    }
    if (match(TokenType::BETWEEN)) {
        auto node = std::make_unique<BetweenExpr>();
        node->value = std::move(left);
        node->negated = negated;
        node->lo = parseAdditive();
        consume(TokenType::AND, "AND");
        node->hi = parseAdditive();
        return node;
    }
    if (match(TokenType::LIKE)) {
        auto node = std::make_unique<LikeExpr>();
        node->value = std::move(left);
        node->negated = negated;
        node->pattern = parseAdditive();
        return node;
    }

    if (isComparisonToken(peek().type)) {
        ComparisonOp op = toComparisonOp(peek().type);
        advance();
        ExpressionPtr right = parseAdditive();
        auto node = std::make_unique<BinaryExpr>();
        node->op = op;
        node->left = std::move(left);
        node->right = std::move(right);
        return node;
    }
    return left;
}

ExpressionPtr Parser::parseAdditive() {
    ExpressionPtr expr = parseMultiplicative();
    for (;;) {
        ArithmeticOp op;
        if (match(TokenType::PLUS)) op = ArithmeticOp::Add;
        else if (match(TokenType::MINUS)) op = ArithmeticOp::Sub;
        else break;
        ExpressionPtr right = parseMultiplicative();
        auto node = std::make_unique<ArithmeticExpr>();
        node->op = op;
        node->left = std::move(expr);
        node->right = std::move(right);
        expr = std::move(node);
    }
    return expr;
}

ExpressionPtr Parser::parseMultiplicative() {
    ExpressionPtr expr = parseUnary();
    for (;;) {
        ArithmeticOp op;
        if (match(TokenType::STAR)) op = ArithmeticOp::Mul;
        else if (match(TokenType::SLASH)) op = ArithmeticOp::Div;
        else break;
        ExpressionPtr right = parseUnary();
        auto node = std::make_unique<ArithmeticExpr>();
        node->op = op;
        node->left = std::move(expr);
        node->right = std::move(right);
        expr = std::move(node);
    }
    return expr;
}

ExpressionPtr Parser::parseUnary() {
    if (match(TokenType::MINUS)) {
        ExpressionPtr operand = parseUnary();
        if (auto* lit = dynamic_cast<LiteralExpr*>(operand.get())) {
            if (lit->kind == LiteralExpr::Kind::Integer) {
                lit->intValue = -lit->intValue;
                return operand;
            }
            if (lit->kind == LiteralExpr::Kind::Float) {
                lit->doubleValue = -lit->doubleValue;
                return operand;
            }
        }
        auto zero = std::make_unique<LiteralExpr>();
        zero->kind = LiteralExpr::Kind::Integer;
        zero->intValue = 0;
        auto node = std::make_unique<ArithmeticExpr>();
        node->op = ArithmeticOp::Sub;
        node->left = std::move(zero);
        node->right = std::move(operand);
        return node;
    }
    if (match(TokenType::PLUS)) {
        return parseUnary();
    }
    return parsePrimary();
}

ExpressionPtr Parser::parsePrimary() {
    if (check(TokenType::CASE)) {
        return parseCase();
    }
    if (check(TokenType::IDENTIFIER) && peekAt(1).type == TokenType::LPAREN) {
        if (isAggregateName(peek().lexeme)) {
            return parseAggregate();
        }
        return parseCall();
    }
    if (match(TokenType::EXISTS)) {
        consume(TokenType::LPAREN, "'('");
        auto sub = std::make_unique<SubqueryExpr>();
        sub->kind = SubqueryExpr::Kind::Exists;
        sub->query = parseSubquery();
        consume(TokenType::RPAREN, "')'");
        return sub;
    }
    if (match(TokenType::LPAREN)) {
        if (check(TokenType::SELECT)) {
            auto sub = std::make_unique<SubqueryExpr>();
            sub->kind = SubqueryExpr::Kind::Scalar;
            sub->query = parseSubquery();
            consume(TokenType::RPAREN, "')'");
            return sub;
        }
        ExpressionPtr expr = parseOr();
        consume(TokenType::RPAREN, "')'");
        return expr;
    }

    const Token& t = peek();
    if (isLiteralToken(t.type)) {
        advance();
        return makeLiteral(t);
    }
    if (t.type == TokenType::IDENTIFIER) {
        return parseColumnRef();
    }

    error(t, "expected an expression");
}

}
