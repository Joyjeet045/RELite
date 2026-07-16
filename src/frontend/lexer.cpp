#include "frontend/lexer.hpp"

#include <cctype>
#include <unordered_map>
#include <utility>

namespace db::parser {

namespace {

const std::unordered_map<std::string, TokenType>& keywordTable() {
    static const std::unordered_map<std::string, TokenType> table = {
        {"FETCH", TokenType::SELECT},    {"FROM", TokenType::FROM},
        {"WHEN", TokenType::WHERE},      {"PUT", TokenType::INSERT},
        {"INTO", TokenType::INTO},       {"VALUES", TokenType::VALUES},
        {"BUILD", TokenType::CREATE},    {"RELATION", TokenType::TABLE},
        {"INDEX", TokenType::INDEX},     {"ON", TokenType::ON},
        {"REMOVE", TokenType::DELETE},   {"AND", TokenType::AND},
        {"OR", TokenType::OR},           {"NOT", TokenType::NOT},
        {"INT", TokenType::INT_TYPE},    {"INTEGER", TokenType::INT_TYPE},
        {"BOOL", TokenType::BOOL_TYPE},  {"BOOLEAN", TokenType::BOOL_TYPE},
        {"TEXT", TokenType::TEXT_TYPE},  {"VARCHAR", TokenType::VARCHAR},
        {"FLOAT", TokenType::FLOAT_TYPE}, {"DOUBLE", TokenType::FLOAT_TYPE},
        {"REAL", TokenType::FLOAT_TYPE},
        {"TRUE", TokenType::TRUE},       {"FALSE", TokenType::FALSE},
        {"MODIFY", TokenType::UPDATE},   {"SET", TokenType::SET},
        {"DISCARD", TokenType::DROP},    {"SORT", TokenType::ORDER},
        {"BY", TokenType::BY},           {"GROUP", TokenType::GROUP},
        {"HAVING", TokenType::HAVING},   {"TAKE", TokenType::LIMIT},
        {"SKIP", TokenType::OFFSET},
        {"AS", TokenType::AS},           {"ASC", TokenType::ASC},
        {"DESC", TokenType::DESC},       {"LINK", TokenType::JOIN},
        {"LEFT", TokenType::LEFT},       {"CROSS", TokenType::CROSS},
        {"IS", TokenType::IS},
        {"IN", TokenType::IN},           {"BETWEEN", TokenType::BETWEEN},
        {"LIKE", TokenType::LIKE},       {"NULL", TokenType::NULL_LITERAL},
        {"START", TokenType::BEGIN},     {"SAVE", TokenType::COMMIT},
        {"UNDO", TokenType::ROLLBACK},
        {"RESHAPE", TokenType::ALTER},   {"ADD", TokenType::ADD},
        {"COLUMN", TokenType::COLUMN},   {"REFERENCES", TokenType::REFERENCES},
        {"FOREIGN", TokenType::FOREIGN}, {"KEY", TokenType::KEY},
        {"PRIMARY", TokenType::PRIMARY}, {"UNIQUE", TokenType::UNIQUE},
        {"DEFAULT", TokenType::DEFAULT}, {"CHECK", TokenType::CHECK},
        {"UNIQUEONLY", TokenType::DISTINCT},
        {"EXISTS", TokenType::EXISTS},
    };
    return table;
}

char toUpper(char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

bool isIdentifierStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isIdentifierPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool isDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

}

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

bool Lexer::isAtEnd() const {
    return pos_ >= source_.size();
}

char Lexer::peek() const {
    return isAtEnd() ? '\0' : source_[pos_];
}

char Lexer::peekNext() const {
    return (pos_ + 1 >= source_.size()) ? '\0' : source_[pos_ + 1];
}

char Lexer::advance() {
    char c = source_[pos_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

void Lexer::skipWhitespaceAndComments() {
    for (;;) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '-' && peekNext() == '-') {
            while (!isAtEnd() && peek() != '\n') {
                advance();
            }
        } else {
            return;
        }
    }
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    for (;;) {
        skipWhitespaceAndComments();
        if (isAtEnd()) {
            tokens.push_back(makeToken(TokenType::END_OF_FILE, "", column_));
            break;
        }
        tokens.push_back(scanToken());
    }
    return tokens;
}

Token Lexer::makeToken(TokenType type, std::string lexeme, int startColumn) const {
    return Token{type, std::move(lexeme), line_, startColumn};
}

Token Lexer::scanToken() {
    int startColumn = column_;
    char c = peek();

    if (isIdentifierStart(c)) {
        return scanIdentifierOrKeyword();
    }
    if (isDigit(c)) {
        return scanNumber();
    }
    if (c == '\'') {
        return scanString();
    }

    advance();
    switch (c) {
        case '(': return makeToken(TokenType::LPAREN, "(", startColumn);
        case ')': return makeToken(TokenType::RPAREN, ")", startColumn);
        case ',': return makeToken(TokenType::COMMA, ",", startColumn);
        case ';': return makeToken(TokenType::SEMICOLON, ";", startColumn);
        case '*': return makeToken(TokenType::STAR, "*", startColumn);
        case '+': return makeToken(TokenType::PLUS, "+", startColumn);
        case '-': return makeToken(TokenType::MINUS, "-", startColumn);
        case '/': return makeToken(TokenType::SLASH, "/", startColumn);
        case '.': return makeToken(TokenType::DOT, ".", startColumn);
        case '=': return makeToken(TokenType::EQ, "=", startColumn);
        case '<':
            if (peek() == '=') { advance(); return makeToken(TokenType::LEQ, "<=", startColumn); }
            if (peek() == '>') { advance(); return makeToken(TokenType::NEQ, "<>", startColumn); }
            return makeToken(TokenType::LT, "<", startColumn);
        case '>':
            if (peek() == '=') { advance(); return makeToken(TokenType::GEQ, ">=", startColumn); }
            return makeToken(TokenType::GT, ">", startColumn);
        case '!':
            if (peek() == '=') { advance(); return makeToken(TokenType::NEQ, "!=", startColumn); }
            return makeToken(TokenType::UNKNOWN, "!", startColumn);
        default:
            return makeToken(TokenType::UNKNOWN, std::string(1, c), startColumn);
    }
}

Token Lexer::scanIdentifierOrKeyword() {
    int startColumn = column_;
    std::string lexeme;
    while (!isAtEnd() && isIdentifierPart(peek())) {
        lexeme.push_back(advance());
    }

    std::string upper;
    upper.reserve(lexeme.size());
    for (char c : lexeme) {
        upper.push_back(toUpper(c));
    }

    const auto& table = keywordTable();
    auto it = table.find(upper);
    if (it != table.end()) {
        return makeToken(it->second, lexeme, startColumn);
    }
    return makeToken(TokenType::IDENTIFIER, lexeme, startColumn);
}

Token Lexer::scanNumber() {
    int startColumn = column_;
    std::string lexeme;
    while (!isAtEnd() && isDigit(peek())) {
        lexeme.push_back(advance());
    }
    if (peek() == '.' && isDigit(peekNext())) {
        lexeme.push_back(advance());
        while (!isAtEnd() && isDigit(peek())) {
            lexeme.push_back(advance());
        }
        return makeToken(TokenType::FLOAT_LITERAL, lexeme, startColumn);
    }
    return makeToken(TokenType::INTEGER_LITERAL, lexeme, startColumn);
}

Token Lexer::scanString() {
    int startColumn = column_;
    advance();
    std::string value;
    for (;;) {
        if (isAtEnd()) {
            return makeToken(TokenType::UNKNOWN, value, startColumn);
        }
        char c = advance();
        if (c == '\'') {
            if (peek() == '\'') {
                advance();
                value.push_back('\'');
                continue;
            }
            break;
        }
        value.push_back(c);
    }
    return makeToken(TokenType::STRING_LITERAL, value, startColumn);
}

std::string_view tokenTypeName(TokenType type) {
    switch (type) {
        case TokenType::SELECT: return "SELECT";
        case TokenType::FROM: return "FROM";
        case TokenType::WHERE: return "WHERE";
        case TokenType::INSERT: return "INSERT";
        case TokenType::INTO: return "INTO";
        case TokenType::VALUES: return "VALUES";
        case TokenType::CREATE: return "CREATE";
        case TokenType::TABLE: return "TABLE";
        case TokenType::INDEX: return "INDEX";
        case TokenType::ON: return "ON";
        case TokenType::DELETE: return "DELETE";
        case TokenType::UPDATE: return "UPDATE";
        case TokenType::SET: return "SET";
        case TokenType::DROP: return "DROP";
        case TokenType::ALTER: return "ALTER";
        case TokenType::ADD: return "ADD";
        case TokenType::COLUMN: return "COLUMN";
        case TokenType::REFERENCES: return "REFERENCES";
        case TokenType::FOREIGN: return "FOREIGN";
        case TokenType::KEY: return "KEY";
        case TokenType::PRIMARY: return "PRIMARY";
        case TokenType::UNIQUE: return "UNIQUE";
        case TokenType::DEFAULT: return "DEFAULT";
        case TokenType::CHECK: return "CHECK";
        case TokenType::DISTINCT: return "DISTINCT";
        case TokenType::ORDER: return "ORDER";
        case TokenType::BY: return "BY";
        case TokenType::GROUP: return "GROUP";
        case TokenType::HAVING: return "HAVING";
        case TokenType::LIMIT: return "LIMIT";
        case TokenType::OFFSET: return "OFFSET";
        case TokenType::AS: return "AS";
        case TokenType::ASC: return "ASC";
        case TokenType::DESC: return "DESC";
        case TokenType::JOIN: return "JOIN";
        case TokenType::INNER: return "INNER";
        case TokenType::LEFT: return "LEFT";
        case TokenType::CROSS: return "CROSS";
        case TokenType::IS: return "IS";
        case TokenType::IN: return "IN";
        case TokenType::BETWEEN: return "BETWEEN";
        case TokenType::LIKE: return "LIKE";
        case TokenType::NULL_LITERAL: return "NULL";
        case TokenType::EXISTS: return "EXISTS";
        case TokenType::BEGIN: return "BEGIN";
        case TokenType::COMMIT: return "COMMIT";
        case TokenType::ROLLBACK: return "ROLLBACK";
        case TokenType::TRANSACTION: return "TRANSACTION";
        case TokenType::AND: return "AND";
        case TokenType::OR: return "OR";
        case TokenType::NOT: return "NOT";
        case TokenType::INT_TYPE: return "INT_TYPE";
        case TokenType::BOOL_TYPE: return "BOOL_TYPE";
        case TokenType::TEXT_TYPE: return "TEXT_TYPE";
        case TokenType::VARCHAR: return "VARCHAR";
        case TokenType::FLOAT_TYPE: return "FLOAT_TYPE";
        case TokenType::TRUE: return "TRUE";
        case TokenType::FALSE: return "FALSE";
        case TokenType::IDENTIFIER: return "IDENTIFIER";
        case TokenType::INTEGER_LITERAL: return "INTEGER_LITERAL";
        case TokenType::FLOAT_LITERAL: return "FLOAT_LITERAL";
        case TokenType::STRING_LITERAL: return "STRING_LITERAL";
        case TokenType::EQ: return "EQ";
        case TokenType::NEQ: return "NEQ";
        case TokenType::LT: return "LT";
        case TokenType::LEQ: return "LEQ";
        case TokenType::GT: return "GT";
        case TokenType::GEQ: return "GEQ";
        case TokenType::PLUS: return "PLUS";
        case TokenType::MINUS: return "MINUS";
        case TokenType::SLASH: return "SLASH";
        case TokenType::LPAREN: return "LPAREN";
        case TokenType::RPAREN: return "RPAREN";
        case TokenType::COMMA: return "COMMA";
        case TokenType::SEMICOLON: return "SEMICOLON";
        case TokenType::STAR: return "STAR";
        case TokenType::DOT: return "DOT";
        case TokenType::END_OF_FILE: return "EOF";
        case TokenType::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

}
