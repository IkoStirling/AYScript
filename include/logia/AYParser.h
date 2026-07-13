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
    std::unique_ptr<ScriptDecl> parseScriptDecl();
    std::unique_ptr<Stmt> parseMember();
    std::unique_ptr<Stmt> parseVarDecl();
    std::unique_ptr<Stmt> parseLifecycleFunc(LifecycleKind kind);
    std::unique_ptr<Stmt> parseFunctionDeclStmt();   // 2026-07-11 audit fix
    std::unique_ptr<Stmt> parseStatement();
    std::vector<Param> parseParamList();

    std::unique_ptr<Expr> parseExpression();
    std::unique_ptr<Expr> parseBinary(int precedence = 0);
    std::unique_ptr<Expr> parseUnary();
    std::unique_ptr<Expr> parseCall();
    std::unique_ptr<Expr> parsePrimary();

    std::unique_ptr<Stmt> parseReturnStmt();
    std::unique_ptr<Stmt> parseIfStmt();
    std::unique_ptr<Stmt> parseWhileStmt();   // R5.0 (2026-07-13): while (cond) { body }
    std::unique_ptr<Stmt> parseForStmt();     // R5.0 (2026-07-13): for (var i : N) { body }
    std::unique_ptr<Stmt> parseBreakStmt();   // R5.1 (2026-07-13): break; (only inside loop)
    std::unique_ptr<Stmt> parseContinueStmt();// R5.1 (2026-07-13): continue; (only inside loop)
    std::unique_ptr<Stmt> parseDoBlock();    // R5.2-A (2026-07-13): do { <stmts> } end
    std::vector<StmtPtr> parseBlockBody();

    // R5.1: track loop nesting depth. parseWhileStmt / parseForStmt
    // push +1 around their body's parseBlockBody call and pop -1
    // after. parseBreakStmt / parseContinueStmt read this to enforce
    // "must be inside a loop" — a bare `break` at function-body or
    // script-block scope is a hard error.
    int _loopDepth = 0;

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

    // R4.1 (2026-07-13): in the table-literal branch, the parser
    // peeks to decide between keyed form `{k=v, ...}` and
    // positional form `{v1, v2, ...}`. A "keyed start" is an
    // Identifier immediately followed by `=`; anything else
    // (including Identifier followed by `,` or `}`) is positional.
    // The 1-token lookahead is the only spot we need this helper.
    bool isStartOfKeyedEntry() const;

    std::vector<Token> _tokens;
    int _current = 0;
    CompilerErrorReporter _reporter;
};

} // namespace ayt::script::logia
