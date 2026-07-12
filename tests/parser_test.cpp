#include <cassert>
#include <iostream>
#include <memory>
#include <string>

#include "frontend/ast.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"

using namespace db::parser;

namespace {

ASTNodePtr parse(const std::string& sql) {
    Lexer lexer(sql);
    Parser parser(lexer.tokenize());
    return parser.parseStatement();
}

void testCreateTable() {
    auto node = parse("BUILD RELATION friend (id INT, name VARCHAR(32), active BOOL);");
    auto* c = dynamic_cast<CreateStatement*>(node.get());
    assert(c != nullptr);
    assert(c->table == "friend");
    assert(c->columns.size() == 3);
    assert(c->columns[0].name == "id" && c->columns[0].type == DataType::Int);
    assert(c->columns[1].type == DataType::Varchar && c->columns[1].varcharLength == 32);
    assert(c->columns[2].name == "active" && c->columns[2].type == DataType::Bool);
}

void testCreateIndex() {
    auto node = parse("BUILD INDEX by_name ON friend (name);");
    auto* ci = dynamic_cast<CreateIdxStatement*>(node.get());
    assert(ci != nullptr);
    assert(ci->indexName == "by_name");
    assert(ci->table == "friend");
    assert(ci->column == "name");
}

void testInsertMultiRow() {
    auto node = parse("PUT INTO t VALUES (1, 'a', TRUE), (2, 'b', FALSE);");
    auto* ins = dynamic_cast<InsertStatement*>(node.get());
    assert(ins != nullptr);
    assert(ins->table == "t");
    assert(ins->columns.empty());
    assert(ins->rows.size() == 2);
    assert(ins->rows[0].size() == 3);
    auto* first = dynamic_cast<LiteralExpr*>(ins->rows[0][0].get());
    assert(first != nullptr && first->kind == LiteralExpr::Kind::Integer && first->intValue == 1);
}

void testInsertWithColumns() {
    auto node = parse("PUT INTO t (id, name) VALUES (7, 'x');");
    auto* ins = dynamic_cast<InsertStatement*>(node.get());
    assert(ins != nullptr);
    assert(ins->columns.size() == 2);
    assert(ins->columns[0] == "id" && ins->columns[1] == "name");
}

void testSelectStar() {
    auto node = parse("FETCH * FROM t;");
    auto* s = dynamic_cast<SelectStatement*>(node.get());
    assert(s != nullptr);
    assert(s->selectStar);
    assert(s->columns.empty());
    assert(s->where == nullptr);
}

void testSelectQualifiedColumns() {
    auto node = parse("FETCH t.a, b FROM t;");
    auto* s = dynamic_cast<SelectStatement*>(node.get());
    assert(s != nullptr);
    assert(!s->selectStar);
    assert(s->columns.size() == 2);
    assert(s->columns[0]->table == "t" && s->columns[0]->column == "a");
    assert(s->columns[1]->table.empty() && s->columns[1]->column == "b");
}

// a = 1 OR b = 2 AND c = 3  ==>  a=1 OR (b=2 AND c=3)
void testWherePrecedence() {
    auto node = parse("FETCH * FROM t WHEN a = 1 OR b = 2 AND c = 3;");
    auto* s = dynamic_cast<SelectStatement*>(node.get());
    assert(s != nullptr && s->where != nullptr);
    auto* root = dynamic_cast<LogicalExpr*>(s->where.get());
    assert(root != nullptr && root->op == LogicalOp::Or);
    assert(dynamic_cast<BinaryExpr*>(root->left.get()) != nullptr);
    auto* rhs = dynamic_cast<LogicalExpr*>(root->right.get());
    assert(rhs != nullptr && rhs->op == LogicalOp::And);
}

void testWhereNotAndParens() {
    auto node = parse("FETCH * FROM t WHEN NOT (a = 1);");
    auto* s = dynamic_cast<SelectStatement*>(node.get());
    assert(s != nullptr && s->where != nullptr);
    auto* un = dynamic_cast<UnaryExpr*>(s->where.get());
    assert(un != nullptr);
    assert(dynamic_cast<BinaryExpr*>(un->operand.get()) != nullptr);
}

void testDelete() {
    auto node = parse("REMOVE FROM t WHEN id >= 5;");
    auto* d = dynamic_cast<DeleteStatement*>(node.get());
    assert(d != nullptr);
    assert(d->table == "t");
    auto* bin = dynamic_cast<BinaryExpr*>(d->where.get());
    assert(bin != nullptr && bin->op == ComparisonOp::Geq);
}

void testTrailingSemicolonOptional() {
    auto node = parse("FETCH * FROM t");  // no ';'
    assert(dynamic_cast<SelectStatement*>(node.get()) != nullptr);
}

void expectParseError(const std::string& sql) {
    bool threw = false;
    try {
        parse(sql);
    } catch (const ParseError&) {
        threw = true;
    }
    if (!threw) {
        std::cerr << "Expected ParseError for: " << sql << "\n";
        assert(false);
    }
}

void testSyntaxErrors() {
    expectParseError("FETCH FROM t;");          // missing projection
    expectParseError("FETCH * t;");             // missing FROM
    expectParseError("BUILD RELATION t (id);");    // missing column type
    expectParseError("PUT INTO t VALUES 1;"); // missing '('
    expectParseError("FETCH * FROM t WHEN;");  // missing predicate
    expectParseError("FETCH * FROM t extra;");  // trailing tokens
    expectParseError("");                        // empty input
}

}  // namespace

int main() {
    testCreateTable();
    testCreateIndex();
    testInsertMultiRow();
    testInsertWithColumns();
    testSelectStar();
    testSelectQualifiedColumns();
    testWherePrecedence();
    testWhereNotAndParens();
    testDelete();
    testTrailingSemicolonOptional();
    testSyntaxErrors();
    std::cout << "All parser tests passed.\n";
    return 0;
}
