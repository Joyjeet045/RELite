#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "frontend/token.hpp"

namespace db::parser {

class Lexer {
public:
    explicit Lexer(std::string source);

    std::vector<Token> tokenize();

private:
    std::string source_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;

    bool isAtEnd() const;
    char peek() const;
    char peekNext() const;
    char advance();

    void skipWhitespaceAndComments();

    Token scanToken();
    Token scanIdentifierOrKeyword();
    Token scanNumber();
    Token scanString();

    Token makeToken(TokenType type, std::string lexeme, int startColumn) const;
};

}
