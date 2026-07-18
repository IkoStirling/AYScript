// Logia lexer unit tests (S0/S2.5)

#include "AYScript.h"
#include "AYTest.h"

using namespace ayt::script::logia;

TEST_SUITE(LogiaLexerTests)

TEST_CASE(keyword_script) {
    // S2.5: `component` keyword renamed to `script`.
    std::vector<Token> tokens;
    tokenize("script", tokens);
    CHECK(tokens.size() == 2u);
    CHECK(tokens[0].type == TokenType::Script);
    CHECK(tokens[1].type == TokenType::EndOfFile);
}

TEST_CASE(keyword_lifecycle) {
    std::vector<Token> tokens;
    tokenize("on_start on_update on_destroy", tokens);
    CHECK(tokens.size() == 4u);
    CHECK(tokens[0].type == TokenType::OnStart);
    CHECK(tokens[1].type == TokenType::OnUpdate);
    CHECK(tokens[2].type == TokenType::OnDestroy);
}

TEST_CASE(compound_assignment) {
    std::vector<Token> tokens;
    tokenize("+= -=", tokens);
    CHECK(tokens.size() == 3u);
    CHECK(tokens[0].type == TokenType::PlusEqual);
    CHECK(tokens[1].type == TokenType::MinusEqual);
}

TEST_CASE(line_comment_skipped) {
    std::vector<Token> tokens;
    tokenize("// comment\nscript", tokens);
    CHECK(tokens.size() == 2u);
    CHECK(tokens[0].type == TokenType::Script);
}

TEST_CASE(export_is_identifier_not_keyword) {
    // S2.5: `export` is no longer a keyword. It now tokenizes as an
    // Identifier (the parser will reject it as a syntax error).
    std::vector<Token> tokens;
    tokenize("export", tokens);
    CHECK(tokens.size() == 2u);
    CHECK(tokens[0].type == TokenType::Identifier);
    CHECK(tokens[0].lexeme == "export");
}

// String literal escape sequences
//
// The Logia string literal accepts the common escape sequences. The lexer
// resolves them when building the token lexeme, so downstream consumers
// (parser, codegen) see the unescaped value.

TEST_CASE(string_literal_basic) {
    std::vector<Token> tokens;
    tokenize("\"hello\"", tokens);
    CHECK(tokens.size() == 2u);
    CHECK(tokens[0].type == TokenType::StringLiteral);
    CHECK(tokens[0].lexeme == "hello");
}

TEST_CASE(string_literal_escape_quote) {
    std::vector<Token> tokens;
    tokenize("\"a\\\"b\"", tokens);
    CHECK(tokens.size() == 2u);
    CHECK(tokens[0].type == TokenType::StringLiteral);
    CHECK(tokens[0].lexeme == "a\"b");
}

TEST_CASE(string_literal_escape_backslash) {
    std::vector<Token> tokens;
    tokenize("\"a\\\\b\"", tokens);
    CHECK(tokens[0].type == TokenType::StringLiteral);
    CHECK(tokens[0].lexeme == "a\\b");
}

TEST_CASE(string_literal_escape_n_r_t_0) {
    std::vector<Token> tokens;
    tokenize("\"\\n\\r\\t\\0\"", tokens);
    CHECK(tokens[0].type == TokenType::StringLiteral);
    CHECK(tokens[0].lexeme.size() == 4u);
    CHECK(tokens[0].lexeme[0] == '\n');
    CHECK(tokens[0].lexeme[1] == '\r');
    CHECK(tokens[0].lexeme[2] == '\t');
    CHECK(tokens[0].lexeme[3] == '\0');
}

TEST_CASE(string_literal_escape_hex) {
    std::vector<Token> tokens;
    tokenize("\"\\x41\"", tokens);   // 'A'
    CHECK(tokens[0].type == TokenType::StringLiteral);
    CHECK(tokens[0].lexeme.size() == 1u);
    CHECK(tokens[0].lexeme[0] == 'A');
}

TEST_CASE(string_literal_escape_unknown) {
    // Unknown \X → keep X literally (don't error)
    std::vector<Token> tokens;
    tokenize("\"\\q\"", tokens);
    CHECK(tokens[0].type == TokenType::StringLiteral);
    CHECK(tokens[0].lexeme == "q");
}

TEST_CASE(string_literal_line_continuation) {
    // Backslash before newline discards both; the literal continues on the
    // next line.
    std::vector<Token> tokens;
    tokenize("\"a\\\nb\"", tokens);
    CHECK(tokens[0].type == TokenType::StringLiteral);
    CHECK(tokens[0].lexeme == "ab");
}

TEST_CASE(string_literal_unterminated) {
    // Missing closing quote → Unknown token
    std::vector<Token> tokens;
    tokenize("\"oops", tokens);
    CHECK(tokens.size() == 2u);
    CHECK(tokens[0].type == TokenType::Unknown);
}

// S4.1 (2026-07-15): `signal` is now a lexer-reserved keyword for the
// per-component signal-declaration surface. The companion call surfaces
// `emit` and `connect` are deliberately NOT keywords — they stay
// Identifiers and are recognized by analyzer shape (free-function
// ambient pattern, same as input/log/time). These two tests pin both
// halves so a future commit can't accidentally blanket-reserve emit /
// connect.

TEST_CASE(keyword_signal) {
    std::vector<Token> tokens;
    tokenize("signal", tokens);
    CHECK(tokens.size() == 2u);
    CHECK(tokens[0].type == TokenType::Signal);
    CHECK(tokens[0].lexeme == "signal");
    CHECK(tokens[1].type == TokenType::EndOfFile);
}

TEST_CASE(emit_and_connect_are_identifiers_not_keywords) {
    // S4.1: `emit` and `connect` are analyzer shape-recognised
    // ambient call names — NOT reserved by the lexer. This mirrors how
    // `input` / `log` / `time` work: they're ordinary Identifiers that
    // the analyzer treats specially when they appear as a CallExpr
    // callee. Pinning this here means a future refactor can't blanket-
    // reserve them without breaking user code that uses those names as
    // variables / helpers (e.g. `var emit = ...` should still parse).
    std::vector<Token> tokens;
    tokenize("emit connect", tokens);
    CHECK(tokens.size() == 3u);
    CHECK(tokens[0].type == TokenType::Identifier);
    CHECK(tokens[0].lexeme == "emit");
    CHECK(tokens[1].type == TokenType::Identifier);
    CHECK(tokens[1].lexeme == "connect");
}

TEST_SUITE_END
