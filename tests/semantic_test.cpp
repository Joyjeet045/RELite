#include <cassert>
#include <iostream>
#include <memory>
#include <string>

#include "frontend/ast.hpp"
#include "frontend/catalog.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semantic_analyzer.hpp"

using namespace db::parser;
using db::semantic::Catalog;
using db::semantic::SemanticAnalyzer;
using db::semantic::SemanticError;

namespace {

ASTNodePtr parse(const std::string& sql) {
    Lexer lexer(sql);
    Parser parser(lexer.tokenize());
    return parser.parseStatement();
}

ASTNodePtr analyze(const std::string& sql) {
    auto node = parse(sql);
    SemanticAnalyzer analyzer(Catalog::instance());
    analyzer.analyze(*node);
    return node;
}

void expectSemanticError(const std::string& sql) {
    bool threw = false;
    try {
        analyze(sql);
    } catch (const SemanticError&) {
        threw = true;
    }
    if (!threw) {
        std::cerr << "Expected SemanticError for: " << sql << "\n";
        assert(false);
    }
}

void testCreateBindsAndRegisters() {
    auto node = analyze("BUILD RELATION friend (id INT, name VARCHAR(16), active BOOL);");
    auto* c = dynamic_cast<CreateStatement*>(node.get());
    assert(c != nullptr && c->tableId >= 0);
    assert(Catalog::instance().hasTable("friend"));
    const auto* schema = Catalog::instance().getTable("friend");
    assert(schema != nullptr && schema->columns.size() == 3);
    assert(schema->columnIndex("name") == 1);
}

void testDuplicateTableRejected() {
    expectSemanticError("BUILD RELATION friend (id INT);");
}

void testDuplicateColumnRejected() {
    expectSemanticError("BUILD RELATION dup (id INT, id BOOL);");
}

void testInsertValidBinds() {
    auto node = analyze("PUT INTO friend VALUES (1, 'garv', TRUE);");
    auto* ins = dynamic_cast<InsertStatement*>(node.get());
    assert(ins != nullptr && ins->tableId >= 0);
}

void testInsertArityMismatch() {
    expectSemanticError("PUT INTO friend VALUES (1, 'garv');");
}

void testInsertTypeMismatch() {
    expectSemanticError("PUT INTO friend VALUES ('nope', 'garv', TRUE);");
}

void testInsertUnknownColumn() {
    expectSemanticError("PUT INTO friend (missing) VALUES (1);");
}

void testSelectBindsColumns() {
    auto node = analyze("FETCH id, name FROM friend WHEN name = 'garv' OR id = 5;");
    auto* s = dynamic_cast<SelectStatement*>(node.get());
    assert(s != nullptr && s->tableId >= 0);
    assert(s->columns[0]->columnIndex == 0);
    assert(s->columns[1]->columnIndex == 1);
    assert(s->where->resolvedType == DataType::Bool);
}

void testSelectBoolColumnPredicate() {
    auto node = analyze("FETCH * FROM friend WHEN active;");
    auto* s = dynamic_cast<SelectStatement*>(node.get());
    assert(s != nullptr);
    assert(s->where->resolvedType == DataType::Bool);
}

void testSelectUnknownTable() {
    expectSemanticError("FETCH * FROM nope;");
}

void testSelectUnknownColumn() {
    expectSemanticError("FETCH missing FROM friend;");
}

void testWhereNonBooleanRejected() {
    expectSemanticError("FETCH * FROM friend WHEN id;");
}

void testWhereTypeMismatchRejected() {
    expectSemanticError("FETCH * FROM friend WHEN id = 'garv';");
}

void testUnknownQualifierRejected() {
    expectSemanticError("FETCH other.id FROM friend;");
}

void testCreateIndexBinds() {
    auto node = analyze("BUILD INDEX by_name ON friend (name);");
    auto* ci = dynamic_cast<CreateIdxStatement*>(node.get());
    assert(ci != nullptr && ci->columnIndices.size() == 1 &&
           ci->columnIndices[0] == 1);
    assert(Catalog::instance().hasIndex("by_name"));
}

void testCreateIndexUnknownColumn() {
    expectSemanticError("BUILD INDEX bad ON friend (missing);");
}

void testDeleteBinds() {
    auto node = analyze("REMOVE FROM friend WHEN id = 3;");
    auto* d = dynamic_cast<DeleteStatement*>(node.get());
    assert(d != nullptr && d->tableId >= 0);
    assert(d->where->resolvedType == DataType::Bool);
}

}

int main() {
    Catalog::instance().reset();

    testCreateBindsAndRegisters();
    testDuplicateTableRejected();
    testDuplicateColumnRejected();
    testInsertValidBinds();
    testInsertArityMismatch();
    testInsertTypeMismatch();
    testInsertUnknownColumn();
    testSelectBindsColumns();
    testSelectBoolColumnPredicate();
    testSelectUnknownTable();
    testSelectUnknownColumn();
    testWhereNonBooleanRejected();
    testWhereTypeMismatchRejected();
    testUnknownQualifierRejected();
    testCreateIndexBinds();
    testCreateIndexUnknownColumn();
    testDeleteBinds();

    Catalog::instance().reset();
    std::cout << "All semantic tests passed.\n";
    return 0;
}
