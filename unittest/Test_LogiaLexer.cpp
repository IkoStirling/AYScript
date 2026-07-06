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

TEST_SUITE_END
