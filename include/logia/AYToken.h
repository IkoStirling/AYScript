#pragma once
// AYToken.h - Token definitions for Logia lexer

#include <cstdint>
#include <string>

namespace ayt::script::logia
{

enum class TokenType : uint8_t {
    Script,         // S2.5: was Component. Top-level `script Name { ... }`.
    Var,
    OnStart,
    OnUpdate,
    OnDestroy,
    Run,            // S3.8b: Tool host run-only entry point.
    If,
    Else,
    Return,
    True,
    False,

    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    PlusEqual,
    MinusEqual,
    StarEqual,
    SlashEqual,
    Equal,
    EqualEqual,
    Bang,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    And,
    Or,

    Dot,
    Comma,
    Colon,
    Semicolon,
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,

    Identifier,
    FloatLiteral,
    IntLiteral,
    StringLiteral,

    EndOfFile,
    Unknown
};

struct Token {
    TokenType type = TokenType::Unknown;
    std::string lexeme;
    int line = 0;
    int column = 0;
};

} // namespace ayt::script::logia
