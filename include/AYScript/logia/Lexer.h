#pragma once
// AYShader\Lexer.h - Lexer for Logia

#include "AYScript/logia/Token.h"
#include <string>
#include <vector>

namespace ayt::script::logia
{

class Lexer {
public:
    explicit Lexer(const std::string& source);

    // Out-parameter avoids MSVC SSO hazards when moving vectors of Token.
    void tokenize(std::vector<Token>& out);

private:
    void scanToken(std::vector<Token>& out);
    Token makeToken(std::vector<Token>& out, TokenType type, int length);
    TokenType identifierType(const std::string& lexeme);
    void skipLineComment();
    char advance();
    bool match(char expected);
    bool isAtEnd() const;
    char peek() const;
    char peekNext() const;

    Token number(std::vector<Token>& out);
    Token identifier(std::vector<Token>& out);
    Token stringLiteral(std::vector<Token>& out);

    std::string _source;
    int _start = 0;
    int _current = 0;
    int _line = 1;
    int _column = 1;
};

} // namespace ayt::script::logia
