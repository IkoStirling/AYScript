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
    std::unique_ptr<Stmt> parseLifecycleFunc(LifecycleKind kind, const Token& keywordTok);   // S5 ED-02: keywordTok for sourceLoc
    std::unique_ptr<Stmt> parseFunctionDeclStmt();   // 2026-07-11 audit fix
    std::unique_ptr<Stmt> parseSignalDecl(const Token& signalTok);   // S4.1 (2026-07-15): per-component signal declaration
    std::unique_ptr<Stmt> parseStatement();
    std::vector<Param> parseParamList();

    std::unique_ptr<Expr> parseExpression();
    std::unique_ptr<Expr> parseBinary(int precedence = 0);
    std::unique_ptr<Expr> parseUnary();
    std::unique_ptr<Expr> parseCall();
    std::unique_ptr<Expr> parsePrimary();

    std::unique_ptr<Stmt> parseReturnStmt(const Token& returnTok);   // S5 ED-02: returnTok for sourceLoc
    std::unique_ptr<Stmt> parseIfStmt(const Token& ifTok);   // S5 ED-02: ifTok for sourceLoc
    std::unique_ptr<Stmt> parseWhileStmt(const Token& whileTok);   // R5.0 (2026-07-13): while (cond) { body }; S5 ED-02: whileTok for sourceLoc
    std::unique_ptr<Stmt> parseForStmt(const Token& forTok);     // R5.0 (2026-07-13): for (var i : N) { body }; S5 ED-02: forTok for sourceLoc
    std::unique_ptr<Stmt> parseBreakStmt(const Token& breakTok);   // R5.1 (2026-07-13): break; (only inside loop); S5 ED-02: breakTok for sourceLoc
    std::unique_ptr<Stmt> parseContinueStmt(const Token& continueTok);// R5.1 (2026-07-13): continue; (only inside loop); S5 ED-02: continueTok for sourceLoc
    std::unique_ptr<Stmt> parseDoBlock(const Token& doTok);    // R5.2-A (2026-07-13): do { <stmts> } end; S5 ED-02: doTok for sourceLoc
    std::unique_ptr<Stmt> parseLabelDecl(const Token& openColonColon);  // R5.2-B (2026-07-14): ::LABEL::; S5 ED-02: openColonColon (currently unused, kept for symmetry)
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
