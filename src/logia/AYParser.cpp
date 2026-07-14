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
    // S3.8b: `run` is the Tool host's run-only entry point. The
    // parser accepts it unconditionally as a member; the semantic
    // analyzer decides whether the host kind allows it (Tool only —
    // soft warn on Component / System so a refactor doesn't break
    // the source shape).
    if (match(TokenType::Run)) {
        return parseLifecycleFunc(LifecycleKind::Run);
    }

    // 2026-07-11 audit fix: script-block-scope helper `function`.
    // Distinct from `on_*` lifecycles (which are dispatched by name)
    // — helper-functions emit as a top-level Lua `function NAME(...)
    // ... end` statement that's reachable from every lifecycle body.
    // Restricted to script-block scope via `parseFunctionDeclStmt`'s
    // call site being here in `parseMember` only.
    if (match(TokenType::Function)) {
        return parseFunctionDeclStmt();
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

    // 2026-07-11 (audit fix): lifecycle functions DO forward their
    // declared params to Lua. `on_update(dt: float)` emits
    // `function M.on_update(self, dt)`; the bridge's callLifecycle
    // passes `dt` at the matching position (see
    // AYScriptRuntimeBridge.cpp::callLifecycle when methodName ==
    // "on_update"). The historical S2.5 "params are diagnostic only"
    // rule was overturned when `emitLifecycleFunc` started emitting
    // params after `self`. design.md §2.3 原则 4 describes the
    // post-audit shape.
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

// 2026-07-11 audit fix: parse a user-defined script-block-scope
// helper. Called only from `parseMember` — `parseStatement` (and
// therefore lifecycle bodies) never invokes this. If a user writes
// `function foo() { ... }` inside a lifecycle body, the `function`
// token has already been advanced by `parseMember`'s call site above
// the lifecycle dispatch, so this code path is unreachable there.
// The lexer recognizes `function` as a reserved keyword (added in
// Step 1 of the audit slice); the `match(TokenType::Function)`
// here is what consumes it.
std::unique_ptr<Stmt> Parser::parseFunctionDeclStmt()
{
    const Token name = consumeIdentifier("Expected function name");
    consume(TokenType::LeftParen, "Expected '(' after function name");
    std::vector<Param> params;
    if (!check(TokenType::RightParen)) {
        params = parseParamList();
    }
    consume(TokenType::RightParen, "Expected ')' after parameter list");

    consume(TokenType::LeftBrace, "Expected '{' before function body");
    std::vector<StmtPtr> body = parseBlockBody();
    consume(TokenType::RightBrace, "Expected '}' after function body");

    return std::make_unique<FunctionDeclStmt>(
        name.lexeme, std::move(params), std::move(body));
}

std::unique_ptr<Stmt> Parser::parseStatement()
{
    if (match(TokenType::Return)) {
        return parseReturnStmt();
    }
    if (match(TokenType::If)) {
        return parseIfStmt();
    }
    // R5.0 (2026-07-13): while / for are valid as statements inside
    // lifecycle bodies and inside script-block helper function bodies
    // (parseBlockBody routes both here via parseStatement). They are
    // NOT valid as script-block members — parseMember has no While /
    // For dispatch, so a bare `while` at script-block scope falls
    // through to the existing "Expected script member" error.
    if (match(TokenType::While)) {
        return parseWhileStmt();
    }
    if (match(TokenType::For)) {
        return parseForStmt();
    }
    // R5.1 (2026-07-13): break / continue are valid only inside a
    // loop body. The loopDepth gate inside parseBreakStmt /
    // parseContinueStmt raises a hard error when seen outside.
    if (match(TokenType::Break)) {
        return parseBreakStmt();
    }
    if (match(TokenType::Continue)) {
        return parseContinueStmt();
    }
    // R5.2-A (2026-07-13): `do { <stmts> } end` block-scope statement.
    // Matches `do` keyword; defers to parseDoBlock for the body. NOT a
    // do-while loop — Lua's `do ... end` is purely block scoping, and
    // Logia aligns with Lua here. The body is parsed with the same
    // parseBlockBody entry as every other braced body.
    if (match(TokenType::Do)) {
        return parseDoBlock();
    }
    // R5.2-B (2026-07-14): `::LABEL::` label declaration. The first
    // `::` has already been consumed by the match() here;
    // parseLabelDecl handles the IDENT and the closing `::`. The
    // label's name is recorded for codegen + downstream analyzer use.
    if (match(TokenType::ColonColon)) {
        return parseLabelDecl();
    }
    if (check(TokenType::Var)) {
        return parseVarDecl();
    }
    // 2026-07-11: a bare `function NAME(...)` inside a lifecycle
    // body is rejected explicitly. parseStatement sees the
    // `function` keyword here but has no script-block context, so
    // it raises a typed diagnostic (the audit fix's hard error for
    // helper-functions inside non-script scope).
    if (check(TokenType::Function)) {
        error("function declarations only allowed as script members");
        return nullptr;
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
    // R5.0.1: both `if cond { ... }` and `if (cond) { ... }` are
    // accepted. Parens are optional but balanced when present —
    // `if (cond { ... }` is a clean parse error from `consume(RParen)`.
    ExprPtr condition;
    if (match(TokenType::LeftParen)) {
        condition = parseExpression();
        consume(TokenType::RightParen, "Expected ')' after if condition");
    } else {
        condition = parseExpression();
    }
    consume(TokenType::LeftBrace, "Expected '{' after if condition");

    std::vector<StmtPtr> thenBranch = parseBlockBody();
    consume(TokenType::RightBrace, "Expected '}' after if branch");

    std::vector<StmtPtr> elseBranch;
    if (match(TokenType::Else)) {
        // R5.0.1: same optional-paren treatment for the else-branch
        // condition when chaining `else if`.
        if (match(TokenType::If)) {
            // `else if cond { ... }` — re-enter parseIfStmt recursively
            // so the inner condition can also use either paren style.
            // Build elseBranch via push_back rather than initializer_list
            // brace-init (MSVC std::vector<unique_ptr> initializer_list
            // is fragile when the element is itself a function call).
            std::vector<StmtPtr> chained;
            chained.push_back(parseIfStmt());
            return std::make_unique<IfStmt>(std::move(condition),
                                            std::move(thenBranch),
                                            std::move(chained));
        }
        consume(TokenType::LeftBrace, "Expected '{' after else");
        elseBranch = parseBlockBody();
        consume(TokenType::RightBrace, "Expected '}' after else branch");
    }

    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch));
}

// R5.0 (2026-07-13): `while (cond) { body }` → `while <cond> do ... end`.
// R5.0.1: `while cond { body }` (no parens) also accepted — peek `(`.
std::unique_ptr<Stmt> Parser::parseWhileStmt()
{
    ExprPtr condition;
    if (match(TokenType::LeftParen)) {
        condition = parseExpression();
        consume(TokenType::RightParen, "Expected ')' after while condition");
    } else {
        condition = parseExpression();
    }
    consume(TokenType::LeftBrace, "Expected '{' before while body");
    // R5.1: push loop depth so any `break` / `continue` inside the
    // body is recognized as inside a loop. The body's parseBlockBody
    // is the same recursive entry as before — only the gate changes.
    ++_loopDepth;
    std::vector<StmtPtr> body = parseBlockBody();
    --_loopDepth;
    consume(TokenType::RightBrace, "Expected '}' after while body");
    return std::make_unique<WhileStmt>(std::move(condition), std::move(body));
}

// R5.0 (2026-07-13): `for (var i : N) { body }` → 1..N inclusive.
// R5.0.1: optional parens + `for (var i : start, end) { body }` half-open range.
std::unique_ptr<Stmt> Parser::parseForStmt()
{
    // R5.0.1: optional `(` after `for`. When absent, the header is
    // `var i : N {` or `var i : 0, 10 {` — distinguishable from the
    // body `{` by the `:` (always present in the counter-var form).
    const bool hasParens = match(TokenType::LeftParen);

    consume(TokenType::Var, "Expected 'var' inside for header");
    const Token counterTok = consumeIdentifier("Expected loop variable name");
    consume(TokenType::Colon, "Expected ':' after loop variable name");

    auto firstExpr = parseExpression();

    // R5.0.1 range form: `for (var i : start, end)` — peek `,`.
    // If we see a comma, consume it + the end expression; if `)`
    // (parens form) or `{` (bare form), the first expression is the
    // single bound for the legacy 1..N short form.
    ExprPtr startExpr;
    ExprPtr endExpr;
    ExprPtr stepExpr;                                 // R5.2-D
    if (match(TokenType::Comma)) {
        startExpr = std::move(firstExpr);
        endExpr = parseExpression();
        // R5.2-D (2026-07-14): optional `step` after the second `,`.
        // Only valid in the range form (short form has no comma at all
        // — `for (var i : N, S)` is interpreted as `start = N, end = S`
        // not `bound = N, step = S`, by R5.0.1 precedence).
        if (match(TokenType::Comma)) {
            stepExpr = parseExpression();
        }
    } else {
        endExpr = std::move(firstExpr);
    }

    if (hasParens) {
        consume(TokenType::RightParen, "Expected ')' after for header");
    }
    consume(TokenType::LeftBrace, "Expected '{' before for body");
    // R5.1: push loop depth (see parseWhileStmt's comment).
    ++_loopDepth;
    std::vector<StmtPtr> body = parseBlockBody();
    --_loopDepth;
    consume(TokenType::RightBrace, "Expected '}' after for body");

    // R5.2-D (2026-07-14): range form with step uses the 5-arg ctor;
    // short form stays at the 2-arg ctor (stepExpr is null in that
    // branch — the parser never sets it without first having seen
    // the range-form `,`).
    if (startExpr) {
        return std::make_unique<ForStmt>(
            counterTok.lexeme,
            std::move(startExpr),
            std::move(endExpr),
            std::move(body),
            std::move(stepExpr));
    }
    return std::make_unique<ForStmt>(
        counterTok.lexeme, std::move(endExpr), std::move(body));
}

// R5.1 (2026-07-13): `break;` (or `break` + stmt-end) inside a loop body.
// R5.2-B (2026-07-14): optionally followed by `:LABEL` to break out
// of an outer loop. The `match(TokenType::Break)` in parseStatement
// has already consumed the keyword token by the time we get here.
// We enforce the loop-depth gate, then optionally consume `:IDENT`,
// then the trailing semicolon.
std::unique_ptr<Stmt> Parser::parseBreakStmt()
{
    if (_loopDepth == 0) {
        error("'break' outside loop");
        return nullptr;
    }
    std::string label;
    if (match(TokenType::Colon)) {
        const Token nameTok = consumeIdentifier(
            "Expected label name after 'break :'");
        label = nameTok.lexeme;
    }
    match(TokenType::Semicolon);
    return std::make_unique<BreakStmt>(std::move(label));
}

// R5.1 (2026-07-13): `continue;` inside a loop body. Same gate
// as break.
// R5.2-B (2026-07-14): `continue :LABEL` is NOT supported in this
// slice. Lua 5.2+ has no clean way to "continue the outer
// loop's next iteration" from inside a nested loop (a label
// that lets `break :L` jump to loop-end would let
// `continue :L` re-execute the loop header, which Lua
// faithfully does — same loop counter, dead loop). Until a
// future slice picks that up, `continue` is bare-only and
// `continue :L` is a hard error.
std::unique_ptr<Stmt> Parser::parseContinueStmt()
{
    if (_loopDepth == 0) {
        error("'continue' outside loop");
        return nullptr;
    }
    if (match(TokenType::Colon)) {
        const Token nameTok = consumeIdentifier(
            "Expected label name after 'continue :'");
        error("'continue :' labels are not supported in R5.2-B "
              "(only 'break :L' is; bare 'continue' is)");
        return nullptr;
    }
    match(TokenType::Semicolon);
    return std::make_unique<ContinueStmt>();
}

// R5.2-A (2026-07-13): `do { <stmts> } end` — explicit block scope.
// The `do` keyword has already been consumed by parseStatement's
// `match(TokenType::Do)` dispatch before we get here. Mirrors the
// other braced-body paths (parseWhileStmt, parseForStmt, parseIfStmt):
// consume `{`, parseBlockBody, consume `}`, then consume `end`. The
// `end` literal in the source is Logia-side only — codegen emits
// Lua's `do ... end` from the BlockStmt AST node.
std::unique_ptr<Stmt> Parser::parseDoBlock()
{
    consume(TokenType::LeftBrace, "Expected '{' after 'do'");
    std::vector<StmtPtr> body = parseBlockBody();
    consume(TokenType::RightBrace, "Expected '}' after do-block body");
    consume(TokenType::End, "Expected 'end' to close do-block");
    return std::make_unique<BlockStmt>(std::move(body));
}

// R5.2-B (2026-07-14): `::LABEL::` — label declaration. The
// opening `::` has already been consumed by parseStatement's
// match dispatch. We expect: IDENT, then `::`. The label's name
// is the only payload (codegen emits `::NAME::`; analyzer
// tracks it for break/continue visibility checks).
std::unique_ptr<Stmt> Parser::parseLabelDecl()
{
    const Token nameTok = consumeIdentifier(
        "Expected label name after '::'");
    consume(TokenType::ColonColon,
            "Expected '::' to close label declaration");
    return std::make_unique<LabelDeclStmt>(nameTok.lexeme);
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

    // S3.12+R3: table literal `{k1=v1, k2=v2, ...}`. The grammar
    // allows: identifier = expression, separated by commas, with
    // an optional trailing comma. Empty `{}` is also accepted.
    //
    // R4.1 (2026-07-13): also support positional entries
    // `{v1, v2, v3}` (Lua array-style). The parser peeks for an
    // identifier followed by `=`; if so, it's a keyed entry. If
    // the first non-brace token is a value expression, the
    // subsequent entries are all positional until a `}` or an
    // explicit identifier=` pair (which is a hard error — mixing
    // positional and keyed entries in one literal is not allowed
    // in Lua and we mirror that).
    if (match(TokenType::LeftBrace)) {
        std::vector<TableExpr::Entry> entries;
        if (!check(TokenType::RightBrace)) {
            // Decide the literal's shape by the first non-brace
            // token: identifier+Equal → keyed; anything else →
            // positional. We don't try to be cleverer (e.g. parse
            // one entry and look at the next); the rule is "all
            // entries share the same shape".
            const bool positional = !isStartOfKeyedEntry();
            if (positional) {
                // Positional form: `{v1, v2, v3}`.
                do {
                    TableExpr::Entry entry;
                    entry.key = nullptr;
                    entry.value = parseExpression();
                    if (!entry.value) return nullptr;
                    entries.push_back(std::move(entry));
                } while (match(TokenType::Comma));
            } else {
                // Keyed form: `{k1=v1, k2=v2, ...}`.
                do {
                    TableExpr::Entry entry;
                    if (!match(TokenType::Identifier)) {
                        error("Expected identifier in table literal key");
                        return nullptr;
                    }
                    std::string key = previous().lexeme;
                    entry.key = std::make_unique<IdentifierExpr>(key);
                    consume(TokenType::Equal, "Expected '=' after table key");
                    entry.value = parseExpression();
                    if (!entry.value) return nullptr;
                    entries.push_back(std::move(entry));
                } while (match(TokenType::Comma));
            }
        }
        consume(TokenType::RightBrace, "Expected '}' after table literal");
        return std::make_unique<TableExpr>(std::move(entries));
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

// R4.1 (2026-07-13): table-literal shape peek. Returns true iff
// the current token is Identifier AND the next token is `=`. Used
// to disambiguate `{k=v, ...}` (keyed) from `{v1, v2, ...}`
// (positional) at the first entry of a table literal. We use a
// safe 1-token lookahead (no side effects on `_current`).
bool Parser::isStartOfKeyedEntry() const
{
    if (current().type != TokenType::Identifier) return false;
    const size_t next = static_cast<size_t>(_current) + 1;
    if (next >= _tokens.size()) return false;
    return _tokens[next].type == TokenType::Equal;
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
        case TokenType::Run:
        case TokenType::Return:
        case TokenType::If:
        case TokenType::While:   // R5.0 (2026-07-13): loop recovery.
        case TokenType::For:     // R5.0 (2026-07-13): loop recovery.
        case TokenType::Break:   // R5.1 (2026-07-13): loop-body statement.
        case TokenType::Continue:// R5.1 (2026-07-13): loop-body statement.
        case TokenType::ColonColon: // R5.2-B (2026-07-14): label-decl recovery.
        case TokenType::RightBrace:
            return;
        default:
            advance();
        }
    }
}

} // namespace ayt::script::logia