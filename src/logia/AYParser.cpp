// AYParser.cpp
//
// S2.5 redesign (2026-07-07): `component` → `script`, `export` removed,
// lifecycle functions take no parameters, `get_component(...)` is no
// longer parsed (it was an S0-S2 path that S2.5 replaces with `self`).

#include "logia/AYParser.h"
#include <stdexcept>

namespace ayt::script::logia
{

Parser::Parser(const std::vector<Token>& tokens)
    : _tokens(tokens), _current(0) {}

std::unique_ptr<Program> Parser::parse()
{
    std::vector<std::unique_ptr<ScriptDecl>> scripts;
    while (!isAtEnd()) {
        const size_t before = static_cast<size_t>(_current);
        if (check(TokenType::Script)) {
            if (auto script = parseScriptDecl()) {
                scripts.push_back(std::move(script));
            } else {
                synchronize();
            }
        } else {
            error("Expected 'script' declaration");
            synchronize();
        }
        if (static_cast<size_t>(_current) == before) {
            advance();
        }
    }
    return std::make_unique<Program>(std::move(scripts));
}

std::unique_ptr<ScriptDecl> Parser::parseScriptDecl()
{
    consume(TokenType::Script, "Expected 'script'");
    const Token name = consumeIdentifier("Expected script name");
    consume(TokenType::LeftBrace, "Expected '{' before script body");

    std::vector<StmtPtr> members;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        const size_t before = static_cast<size_t>(_current);
        if (auto member = parseMember()) {
            members.push_back(std::move(member));
        } else {
            synchronize();
        }
        if (static_cast<size_t>(_current) == before) {
            advance();
        }
    }

    consume(TokenType::RightBrace, "Expected '}' after script body");
    return std::make_unique<ScriptDecl>(name.lexeme, std::move(members));
}

std::unique_ptr<Stmt> Parser::parseMember()
{
    // S2.5: `export var` is gone — `var` is always a pure local.
    if (check(TokenType::Var)) {
        return parseVarDecl();
    }
    if (match(TokenType::OnStart)) {
        return parseLifecycleFunc(LifecycleKind::OnStart);
    }
    if (match(TokenType::OnUpdate)) {
        return parseLifecycleFunc(LifecycleKind::OnUpdate);
    }
    if (match(TokenType::OnDestroy)) {
        return parseLifecycleFunc(LifecycleKind::OnDestroy);
    }

    error("Expected script member (var or lifecycle function)");
    return nullptr;
}

std::unique_ptr<Stmt> Parser::parseVarDecl()
{
    consume(TokenType::Var, "Expected 'var'");
    const Token name = consumeIdentifier("Expected variable name");
    consume(TokenType::Colon, "Expected ':' after variable name");
    const Token typeName = consumeIdentifier("Expected variable type");

    ExprPtr initializer;
    if (match(TokenType::Equal)) {
        initializer = parseExpression();
    }
    match(TokenType::Semicolon);
    // S2.5: no `exported` flag — `var` is always a pure Lua local.
    return std::make_unique<VarDeclStmt>(name.lexeme, typeName.lexeme, std::move(initializer));
}

std::unique_ptr<Stmt> Parser::parseLifecycleFunc(LifecycleKind kind)
{
    consume(TokenType::LeftParen, "Expected '(' after lifecycle function name");

    // S2.5: lifecycle functions take no parameters. The parser still
    // parses a non-empty parameter list (so SemanticAnalyzer can
    // emit a soft warning for old-style code) but codegen ignores
    // the params — only `self` reaches Lua.
    std::vector<Param> legacyParams;
    if (!check(TokenType::RightParen)) {
        legacyParams = parseParamList();
    }
    consume(TokenType::RightParen, "Expected ')' after parameter list");

    consume(TokenType::LeftBrace, "Expected '{' before function body");
    std::vector<StmtPtr> body = parseBlockBody();
    consume(TokenType::RightBrace, "Expected '}' after function body");

    return std::make_unique<LifecycleFuncDecl>(kind, std::move(legacyParams), std::move(body));
}

std::vector<Param> Parser::parseParamList()
{
    std::vector<Param> params;
    do {
        const Token name = consumeIdentifier("Expected parameter name");
        consume(TokenType::Colon, "Expected ':' after parameter name");
        const Token typeName = consumeIdentifier("Expected parameter type");
        params.push_back(Param{name.lexeme, typeName.lexeme});
    } while (match(TokenType::Comma));
    return params;
}

std::unique_ptr<Stmt> Parser::parseStatement()
{
    if (match(TokenType::Return)) {
        return parseReturnStmt();
    }
    if (match(TokenType::If)) {
        return parseIfStmt();
    }
    if (check(TokenType::Var)) {
        return parseVarDecl();
    }

    auto expr = parseExpression();
    match(TokenType::Semicolon);
    return std::make_unique<ExprStmt>(std::move(expr));
}

std::vector<StmtPtr> Parser::parseBlockBody()
{
    std::vector<StmtPtr> body;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        const size_t before = static_cast<size_t>(_current);
        if (auto stmt = parseStatement()) {
            if (auto* exprStmt = dynamic_cast<ExprStmt*>(stmt.get())) {
                if (!exprStmt->expr) {
                    advance();
                    continue;
                }
            }
            body.push_back(std::move(stmt));
        } else {
            advance();
        }
        if (static_cast<size_t>(_current) == before) {
            advance();
        }
    }
    return body;
}

std::unique_ptr<Stmt> Parser::parseReturnStmt()
{
    ExprPtr value;
    if (!check(TokenType::Semicolon) && !check(TokenType::RightBrace)) {
        value = parseExpression();
    }
    match(TokenType::Semicolon);
    return std::make_unique<ReturnStmt>(std::move(value));
}

std::unique_ptr<Stmt> Parser::parseIfStmt()
{
    auto condition = parseExpression();
    consume(TokenType::LeftBrace, "Expected '{' after if condition");

    std::vector<StmtPtr> thenBranch = parseBlockBody();
    consume(TokenType::RightBrace, "Expected '}' after if branch");

    std::vector<StmtPtr> elseBranch;
    if (match(TokenType::Else)) {
        consume(TokenType::LeftBrace, "Expected '{' after else");
        elseBranch = parseBlockBody();
        consume(TokenType::RightBrace, "Expected '}' after else branch");
    }

    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch));
}

std::unique_ptr<Expr> Parser::parseExpression()
{
    return parseBinary();
}

std::unique_ptr<Expr> Parser::parseBinary(int precedence)
{
    auto left = parseUnary();

    while (!isAtEnd()) {
        const TokenType op = current().type;
        const int nextPrecedence = getPrecedence(op);
        if (nextPrecedence < precedence) {
            break;
        }
        if (nextPrecedence == 0) {
            break;
        }

        const Token opTok = current();
        advance();
        auto right = parseBinary(nextPrecedence);
        left = std::make_unique<BinaryExpr>(std::move(left), opTok, std::move(right));
    }

    return left;
}

std::unique_ptr<Expr> Parser::parseUnary()
{
    if (match(TokenType::Minus) || match(TokenType::Bang)) {
        const Token op = previous();
        auto operand = parseUnary();
        return std::make_unique<UnaryExpr>(op, std::move(operand));
    }
    return parseCall();
}

std::unique_ptr<Expr> Parser::parseCall()
{
    auto expr = parsePrimary();

    while (true) {
        if (match(TokenType::LeftParen)) {
            std::vector<ExprPtr> args;
            if (!check(TokenType::RightParen)) {
                do {
                    args.push_back(parseExpression());
                } while (match(TokenType::Comma));
            }
            consume(TokenType::RightParen, "Expected ')' after arguments");
            expr = std::make_unique<CallExpr>(std::move(expr), std::move(args));
        } else if (match(TokenType::Dot)) {
            const Token name = consumeIdentifier("Expected property name after '.'");
            expr = std::make_unique<MemberExpr>(std::move(expr), name.lexeme);
        } else if (match(TokenType::LeftBracket)) {
            auto index = parseExpression();
            consume(TokenType::RightBracket, "Expected ']' after index");
            expr = std::make_unique<IndexExpr>(std::move(expr), std::move(index));
        } else {
            break;
        }
    }

    return expr;
}

std::unique_ptr<Expr> Parser::parsePrimary()
{
    if (match(TokenType::FloatLiteral)) {
        const Token token = previous();
        try {
            return std::make_unique<LiteralExpr>(std::stof(token.lexeme));
        } catch (const std::exception&) {
            return std::make_unique<LiteralExpr>(0.0f);
        }
    }
    if (match(TokenType::IntLiteral)) {
        const Token token = previous();
        try {
            return std::make_unique<LiteralExpr>(static_cast<int>(std::stol(token.lexeme)));
        } catch (const std::exception&) {
            return std::make_unique<LiteralExpr>(0);
        }
    }
    if (match(TokenType::StringLiteral)) {
        return std::make_unique<LiteralExpr>(previous().lexeme);
    }
    if (match(TokenType::True)) {
        return std::make_unique<LiteralExpr>(true);
    }
    if (match(TokenType::False)) {
        return std::make_unique<LiteralExpr>(false);
    }
    if (match(TokenType::Identifier)) {
        return std::make_unique<IdentifierExpr>(previous().lexeme);
    }
    if (match(TokenType::LeftParen)) {
        auto expr = parseExpression();
        consume(TokenType::RightParen, "Expected ')' after expression");
        return expr;
    }

    error("Expected expression");
    return nullptr;
}

const Token& Parser::current() const
{
    return _tokens[static_cast<size_t>(_current)];
}

const Token& Parser::previous() const
{
    return _tokens[static_cast<size_t>(_current - 1)];
}

bool Parser::check(TokenType type) const
{
    if (isAtEnd()) {
        return false;
    }
    return current().type == type;
}

bool Parser::match(TokenType type)
{
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::match(std::initializer_list<TokenType> types)
{
    for (const TokenType type : types) {
        if (check(type)) {
            advance();
            return true;
        }
    }
    return false;
}

Token Parser::advance()
{
    if (!isAtEnd()) {
        _current++;
    }
    return previous();
}

bool Parser::isAtEnd() const
{
    return current().type == TokenType::EndOfFile;
}

void Parser::error(const std::string& message)
{
    const Token& tok = current();
    _reporter.error(ErrorCode::UnexpectedToken, message, tok.line, tok.column);
}

Token Parser::consume(TokenType type, const std::string& message)
{
    if (check(type)) {
        return advance();
    }
    error(message);
    return previous();
}

Token Parser::consumeIdentifier(const std::string& message)
{
    if (check(TokenType::Identifier)) {
        return advance();
    }
    error(message);
    return advance();
}

int Parser::getPrecedence(TokenType op)
{
    switch (op) {
    case TokenType::Equal:
    case TokenType::PlusEqual:
    case TokenType::MinusEqual:
    case TokenType::StarEqual:
    case TokenType::SlashEqual:
        return 1;
    case TokenType::Or:
        return 1;
    case TokenType::And:
        return 2;
    case TokenType::EqualEqual:
    case TokenType::BangEqual:
        return 3;
    case TokenType::Less:
    case TokenType::LessEqual:
    case TokenType::Greater:
    case TokenType::GreaterEqual:
        return 4;
    case TokenType::Plus:
    case TokenType::Minus:
        return 5;
    case TokenType::Star:
    case TokenType::Slash:
    case TokenType::Percent:
        return 6;
    default:
        return 0;
    }
}

void Parser::synchronize()
{
    while (!isAtEnd()) {
        switch (current().type) {
        case TokenType::Script:
        case TokenType::Var:
        case TokenType::OnStart:
        case TokenType::OnUpdate:
        case TokenType::OnDestroy:
        case TokenType::Return:
        case TokenType::If:
        case TokenType::RightBrace:
            return;
        default:
            advance();
        }
    }
}

} // namespace ayt::script::logia