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
    Function,       // 2026-07-11 audit fix: script-block-scope helper
                    // `function NAME(params) { BODY }`. Parser
                    // restricts to ScriptDecl members; inside a
                    // lifecycle body it produces a hard error
                    // ("function declarations only allowed as
                    // script members").
    If,
    Else,
    While,            // R5.0 (2026-07-13): `while (cond) { body }` → `while cond do ... end`
    For,              // R5.0 (2026-07-13): `for (var i : N) { body }` → `for i = 1, N do ... end`
    Break,            // R5.1 (2026-07-13): `break;` (or `break` + stmt-end) inside loop body
    Continue,         // R5.1 (2026-07-13): `continue;` inside loop body
    Do,               // R5.2-A (2026-07-13): block-scope entry marker
                      // `do { ... } end`. Parsed by Parser::parseStatement;
                      // codegens to Lua's `do ... end`. NOT a loop keyword
                      // (matches Lua 5.2+ semantics — `do ... end` is pure
                      // block scope). Slice B (`break :L`) is a separate
                      // commit.
    End,              // R5.2-A (2026-07-13): block-scope closer for explicit
                      // `do { ... } end` form. Tokenizer-only — semantically
                      // a no-op for the parser (BlockStmt stores body as
                      // a vector), but the lexer recognizes `end` so
                      // `do {} end` parses cleanly. (Other control-flow
                      // statements close implicitly on `}`; explicit-`end`
                      // is only for `do`.)
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
    ColonColon,      // R5.2-B (2026-07-14): `::` opener/closer for
                     // `::LABEL::` label declarations. Lexer emits
                     // when scanning `:` and the next char is also
                     // `:`; otherwise emits the single `Colon` (used
                     // for var/param/field type annotations). Parser
                     // treats a `::Identifier::` triplet as a
                     // LabelDeclStmt. Single-`:` colon usage is
                     // preserved.
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
