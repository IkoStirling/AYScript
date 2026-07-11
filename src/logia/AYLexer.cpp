// AYLexer.cpp

#include "logia/AYLexer.h"
#include <cctype>
#include <unordered_map>

namespace ayt::script::logia
{

Lexer::Lexer(const std::string& source)
    : _source(source), _start(0), _current(0), _line(1), _column(1) {}

void Lexer::tokenize(std::vector<Token>& out)
{
    out.reserve(out.size() + 1024);

    while (!isAtEnd()) {
        _start = _current;
        scanToken(out);
    }

    Token eof;
    eof.type = TokenType::EndOfFile;
    eof.lexeme = "";
    eof.line = _line;
    eof.column = _column;
    out.push_back(eof);
}

void Lexer::scanToken(std::vector<Token>& out)
{
    char c = advance();
    switch (c) {
    case '(':
        makeToken(out, TokenType::LeftParen, 1);
        break;
    case ')':
        makeToken(out, TokenType::RightParen, 1);
        break;
    case '{':
        makeToken(out, TokenType::LeftBrace, 1);
        break;
    case '}':
        makeToken(out, TokenType::RightBrace, 1);
        break;
    case '[':
        makeToken(out, TokenType::LeftBracket, 1);
        break;
    case ']':
        makeToken(out, TokenType::RightBracket, 1);
        break;
    case ',':
        makeToken(out, TokenType::Comma, 1);
        break;
    case ':':
        makeToken(out, TokenType::Colon, 1);
        break;
    case ';':
        makeToken(out, TokenType::Semicolon, 1);
        break;
    case '.':
        makeToken(out, TokenType::Dot, 1);
        break;
    case '+':
        makeToken(out, match('=') ? TokenType::PlusEqual : TokenType::Plus, 1);
        break;
    case '-':
        makeToken(out, match('=') ? TokenType::MinusEqual : TokenType::Minus, 1);
        break;
    case '*':
        makeToken(out, match('=') ? TokenType::StarEqual : TokenType::Star, 1);
        break;
    case '/':
        if (match('/')) {
            skipLineComment();
        } else {
            makeToken(out, match('=') ? TokenType::SlashEqual : TokenType::Slash, 1);
        }
        break;
    case '%':
        makeToken(out, TokenType::Percent, 1);
        break;
    case '=':
        makeToken(out, match('=') ? TokenType::EqualEqual : TokenType::Equal, 1);
        break;
    case '!':
        makeToken(out, match('=') ? TokenType::BangEqual : TokenType::Bang, 1);
        break;
    case '<':
        makeToken(out, match('=') ? TokenType::LessEqual : TokenType::Less, 1);
        break;
    case '>':
        makeToken(out, match('=') ? TokenType::GreaterEqual : TokenType::Greater, 1);
        break;
    case '&':
        makeToken(out, match('&') ? TokenType::And : TokenType::Unknown, 1);
        break;
    case '|':
        makeToken(out, match('|') ? TokenType::Or : TokenType::Unknown, 1);
        break;
    case '"':
        stringLiteral(out);
        break;
    case ' ':
    case '\r':
    case '\t':
        break;
    case '\n':
        _line++;
        _column = 0;
        break;
    default:
        if (std::isdigit(static_cast<unsigned char>(c))) {
            number(out);
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            identifier(out);
        } else {
            makeToken(out, TokenType::Unknown, 1);
        }
    }
}

Token Lexer::makeToken(std::vector<Token>& out, TokenType type, int length)
{
    Token token;
    token.type = type;
    token.lexeme = _source.substr(_start, static_cast<size_t>(length));
    token.line = _line;
    token.column = _column - length + 1;
    out.push_back(token);
    return token;
}

TokenType Lexer::identifierType(const std::string& lexeme)
{
    // S2.5 redesign: `component` → `script`, `export` removed (C++ AY_PROPERTY
    // is now the single source of truth for which fields are exposed).
    static const std::unordered_map<std::string, TokenType> keywords = {
        {"script", TokenType::Script},
        {"var", TokenType::Var},
        {"on_start", TokenType::OnStart},
        {"on_update", TokenType::OnUpdate},
        {"on_destroy", TokenType::OnDestroy},
        {"run", TokenType::Run},  // S3.8b: Tool host entry point.
        {"function", TokenType::Function},  // 2026-07-11: script-block helper.
        // NOTE: `local` is intentionally NOT reserved (would break
        // the 15 R3/R4 tests that write `local s = ...` and rely on
        // the implicit-global declaration). SemanticAnalyzer emits a
        // soft warning `LuaKeywordLeak` to surface the leak.
        {"if", TokenType::If},
        {"else", TokenType::Else},
        {"return", TokenType::Return},
        {"true", TokenType::True},
        {"false", TokenType::False},
    };

    const auto it = keywords.find(lexeme);
    return (it != keywords.end()) ? it->second : TokenType::Identifier;
}

Token Lexer::number(std::vector<Token>& out)
{
    while (std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
    }

    bool hasFraction = false;
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peekNext()))) {
        hasFraction = true;
        advance();
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }

    Token token;
    token.type = hasFraction ? TokenType::FloatLiteral : TokenType::IntLiteral;
    token.lexeme = _source.substr(_start, static_cast<size_t>(_current - _start));
    token.line = _line;
    token.column = _column - static_cast<int>(token.lexeme.length()) + 1;
    out.push_back(token);
    return token;
}

Token Lexer::identifier(std::vector<Token>& out)
{
    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
        advance();
    }

    Token token;
    token.lexeme = _source.substr(_start, static_cast<size_t>(_current - _start));
    token.type = identifierType(token.lexeme);
    token.line = _line;
    token.column = _column - static_cast<int>(token.lexeme.length()) + 1;
    out.push_back(token);
    return token;
}

Token Lexer::stringLiteral(std::vector<Token>& out)
{
    std::string value;
    value.reserve(16);

    while (peek() != '"' && !isAtEnd()) {
        char c = peek();

        if (c == '\\') {
            // Escape sequence
            advance();  // consume '\'
            if (isAtEnd()) break;
            char esc = peek();
            switch (esc) {
            case '"':  value += '"';  advance(); break;
            case '\\': value += '\\'; advance(); break;
            case 'n':  value += '\n'; advance(); break;
            case 'r':  value += '\r'; advance(); break;
            case 't':  value += '\t'; advance(); break;
            case '0':  value += '\0'; advance(); break;
            case '\n':
                // Line continuation: '\' + newline → discard both
                advance();
                _line++;
                _column = 0;
                break;
            case 'x': {
                // \xHH — exactly two hex digits required
                advance();
                if (isAtEnd() ||
                    !std::isxdigit(static_cast<unsigned char>(peek()))) {
                    // Malformed: drop the 'x', keep raw chars
                    value += 'x';
                    break;
                }
                int hi = peek();
                advance();
                if (isAtEnd() ||
                    !std::isxdigit(static_cast<unsigned char>(peek()))) {
                    // Only one hex digit — keep as-is
                    value += static_cast<char>(hi);
                    break;
                }
                int lo = peek();
                advance();
                auto hexVal = [](int d) -> int {
                    if (d >= '0' && d <= '9') return d - '0';
                    if (d >= 'a' && d <= 'f') return 10 + (d - 'a');
                    return 10 + (d - 'A');
                };
                value += static_cast<char>((hexVal(hi) << 4) | hexVal(lo));
                break;
            }
            default:
                // Unknown escape: keep the character literally
                value += esc;
                advance();
                break;
            }
            continue;
        }

        if (c == '\n') {
            // Unescaped newline inside a string literal — keep as-is in the
            // value but advance the line counter so subsequent error
            // messages have correct locations.
            value += '\n';
            _line++;
            _column = 0;
        } else {
            value += c;
        }
        advance();
    }

    if (isAtEnd()) {
        // Unterminated string — emit Unknown token covering the span we read
        return makeToken(out, TokenType::Unknown, _current - _start);
    }

    // Consume closing '"'
    advance();

    Token token;
    token.type = TokenType::StringLiteral;
    token.lexeme = std::move(value);
    token.line = _line;
    token.column = _column - static_cast<int>(token.lexeme.length()) - 2 + 1;
    out.push_back(token);
    return token;
}

void Lexer::skipLineComment()
{
    while (peek() != '\n' && !isAtEnd()) {
        advance();
    }
}

char Lexer::advance()
{
    _current++;
    _column++;
    return _source[static_cast<size_t>(_current - 1)];
}

bool Lexer::match(char expected)
{
    if (isAtEnd()) {
        return false;
    }
    if (_source[static_cast<size_t>(_current)] != expected) {
        return false;
    }
    _current++;
    _column++;
    return true;
}

bool Lexer::isAtEnd() const
{
    return _current >= static_cast<int>(_source.size());
}

char Lexer::peek() const
{
    if (isAtEnd()) {
        return '\0';
    }
    return _source[static_cast<size_t>(_current)];
}

char Lexer::peekNext() const
{
    if (_current + 1 >= static_cast<int>(_source.size())) {
        return '\0';
    }
    return _source[static_cast<size_t>(_current + 1)];
}

} // namespace ayt::script::logia
