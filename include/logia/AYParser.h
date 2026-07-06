#pragma once
// AYParser.h - Parser for Logia

#include "AYAst.h"
#include "AYCompilerError.h"
#include "AYToken.h"
#include <initializer_list>
#include <memory>
#include <vector>

namespace ayt::script::logia
{

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens);

    std::unique_ptr<Program> parse();

    const std::vector<CompilerError>& errors() const { return _reporter.errors(); }
    bool hasErrors() const { return _reporter.hasErrors(); }

private:
    std::unique_ptr<ComponentDecl> parseComponentDecl();
    std::unique_ptr<Stmt> parseMember();
    std::unique_ptr<Stmt> parseVarDecl(bool exported);
    std::unique_ptr<Stmt> parseLifecycleFunc(LifecycleKind kind);
    std::unique_ptr<Stmt> parseStatement();
    std::vector<Param> parseParamList();

    std::unique_ptr<Expr> parseExpression();
    std::unique_ptr<Expr> parseBinary(int precedence = 0);
    std::unique_ptr<Expr> parseUnary();
    std::unique_ptr<Expr> parseCall();
    std::unique_ptr<Expr> parsePrimary();

    std::unique_ptr<Stmt> parseReturnStmt();
    std::unique_ptr<Stmt> parseIfStmt();
    std::vector<StmtPtr> parseBlockBody();

    const Token& current() const;
    const Token& previous() const;
    bool check(TokenType type) const;
    bool match(TokenType type);
    bool match(std::initializer_list<TokenType> types);
    Token advance();
    bool isAtEnd() const;
    void error(const std::string& message);
    Token consume(TokenType type, const std::string& message);
    Token consumeIdentifier(const std::string& message);
    int getPrecedence(TokenType op);
    void synchronize();

    std::vector<Token> _tokens;
    int _current = 0;
    CompilerErrorReporter _reporter;
};

} // namespace ayt::script::logia
