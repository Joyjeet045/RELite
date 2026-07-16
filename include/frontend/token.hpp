#pragma once

#include <string>
#include <string_view>

namespace db::parser {

enum class TokenType {
    SELECT,
    FROM,
    WHERE,
    INSERT,
    INTO,
    VALUES,
    CREATE,
    TABLE,
    INDEX,
    ON,
    DELETE,
    UPDATE,
    SET,
    DROP,
    ALTER,
    ADD,
    COLUMN,
    REFERENCES,
    FOREIGN,
    KEY,
    PRIMARY,
    UNIQUE,
    DEFAULT,
    CHECK,

    ORDER,
    BY,
    GROUP,
    HAVING,
    LIMIT,
    OFFSET,
    AS,
    ASC,
    DESC,
    JOIN,
    INNER,
    LEFT,
    CROSS,
    DISTINCT,

    IS,
    IN,
    BETWEEN,
    LIKE,
    NULL_LITERAL,
    EXISTS,

    BEGIN,
    COMMIT,
    ROLLBACK,
    TRANSACTION,

    AND,
    OR,
    NOT,

    INT_TYPE,
    BOOL_TYPE,
    TEXT_TYPE,
    VARCHAR,
    FLOAT_TYPE,

    TRUE,
    FALSE,

    IDENTIFIER,
    INTEGER_LITERAL,
    FLOAT_LITERAL,
    STRING_LITERAL,

    EQ,
    NEQ,
    LT,
    LEQ,
    GT,
    GEQ,

    PLUS,
    MINUS,
    SLASH,

    LPAREN,
    RPAREN,
    COMMA,
    SEMICOLON,
    STAR,
    DOT,

    END_OF_FILE,
    UNKNOWN
};

struct Token {
    TokenType type;
    std::string lexeme;
    int line;
    int column;
};

std::string_view tokenTypeName(TokenType type);

}
