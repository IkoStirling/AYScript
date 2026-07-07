// Logia lexer unit tests (S0)

#include "AYScript.h"
#include "AYTest.h"

using namespace ayt::script::logia;

TEST_SUITE(LogiaLexerTests)

TEST_CASE(keyword_component) {
    std::vector<Token> tokens;
    tokenize("component", tokens);
    CHECK(tokens.size() == 2u);
    CHECK(tokens[0].type == TokenType::Component);
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
    tokenize("// comment\ncomponent", tokens);
    CHECK(tokens.size() == 2u);
    CHECK(tokens[0].type == TokenType::Component);
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

TEST_SUITE_END
