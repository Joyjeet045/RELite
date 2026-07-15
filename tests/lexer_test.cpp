#include <cassert>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "frontend/lexer.hpp"

using db::parser::Lexer;
using db::parser::Token;
using db::parser::TokenType;

namespace {

std::vector<Token> lex(const std::string& src) {
    Lexer lexer(src);
    return lexer.tokenize();
}

void expectTypes(const std::string& src, const std::vector<TokenType>& expected) {
    auto tokens = lex(src);
    if (tokens.size() != expected.size()) {
        std::cerr << "Token count mismatch for source: " << src << " (got "
                  << tokens.size() << ", expected " << expected.size() << ")\n";
        assert(false);
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (tokens[i].type != expected[i]) {
            std::cerr << "Type mismatch at index " << i << " for source: " << src << "\n";
            assert(false);
        }
    }
}

void testSelect() {
    expectTypes("FETCH * FROM users;", {
        TokenType::SELECT, TokenType::STAR, TokenType::FROM,
        TokenType::IDENTIFIER, TokenType::SEMICOLON, TokenType::END_OF_FILE});
}

void testKeywordsCaseInsensitive() {
    expectTypes("fetch From wHeN", {
        TokenType::SELECT, TokenType::FROM, TokenType::WHERE, TokenType::END_OF_FILE});
}

void testOperators() {
    expectTypes("= != <> < <= > >=", {
        TokenType::EQ, TokenType::NEQ, TokenType::NEQ, TokenType::LT,
        TokenType::LEQ, TokenType::GT, TokenType::GEQ, TokenType::END_OF_FILE});
}

void testLiterals() {
    auto tokens = lex("PUT INTO t VALUES (42, 'Alice', TRUE);");
    bool sawInt = false, sawStr = false, sawBool = false;
    for (const auto& t : tokens) {
        if (t.type == TokenType::INTEGER_LITERAL && t.lexeme == "42") sawInt = true;
        if (t.type == TokenType::STRING_LITERAL && t.lexeme == "Alice") sawStr = true;
        if (t.type == TokenType::TRUE) sawBool = true;
    }
    assert(sawInt && sawStr && sawBool);
}

void testEscapedQuoteInString() {
    auto tokens = lex("'O''Brien'");
    assert(tokens.size() == 2);
    assert(tokens[0].type == TokenType::STRING_LITERAL);
    assert(tokens[0].lexeme == "O'Brien");
}

void testLineComment() {
    expectTypes("FETCH -- comment here\n *", {
        TokenType::SELECT, TokenType::STAR, TokenType::END_OF_FILE});
}

void testColumnTypes() {
    expectTypes("INT BOOL TEXT VARCHAR", {
        TokenType::INT_TYPE, TokenType::BOOL_TYPE, TokenType::TEXT_TYPE,
        TokenType::VARCHAR, TokenType::END_OF_FILE});
}

}

int main() {
    testSelect();
    testKeywordsCaseInsensitive();
    testOperators();
    testLiterals();
    testEscapedQuoteInString();
    testLineComment();
    testColumnTypes();
    std::cout << "All lexer tests passed.\n";
    return 0;
}
