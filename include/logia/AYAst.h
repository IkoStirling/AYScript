#pragma once
// AYAst.h - AST node definitions for Logia (S0/S1/S2) + S2.5 redesign
//
// S2.5 redesign (2026-07-07): `component` keyword renamed to `script`,
// `export` keyword removed, `ComponentDecl` renamed to `ScriptDecl`,
// `VarDeclStmt::exported` field removed. Logia `var` is now always a
// pure Lua local; C++ fields are declared on the ScriptComponent
// subclass via AY_PROPERTY and accessed through `self.field`.

#include "AYToken.h"
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace ayt::reflect
{
class ITypeInfo;
class IFieldInfo;
class IMethodInfo;
} // namespace ayt::reflect

namespace ayt::script::logia
{

enum class LifecycleKind {
    OnStart,
    OnUpdate,
    OnDestroy,
    Run,            // S3.8b: Tool host entry point (no receiver / no self).
};

class Expr;
class Stmt;
class ScriptDecl;
class Program;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

class Expr {
public:
    virtual ~Expr() = default;

    // ---- S2: SemanticAnalyzer stamps these fields in place ----
    // `resolvedType` is the static type of this expression after lookup
    // (nullptr means unresolved — codegen keeps S1 behavior in that case).
    // `resolvedField` is set on the leaf MemberExpr when the chain resolves
    // against a registered type.
    // `resolvedDecl` is set on IdentifierExpr pointing to the VarDeclStmt* /
    // Param* that introduced the name into scope (opaque to avoid extra
    // includes here).
    const ayt::reflect::ITypeInfo*   resolvedType  = nullptr;
    const ayt::reflect::IFieldInfo*  resolvedField = nullptr;
    // S3.12 (track R2 §5.7.4): set on CallExpr when the callee is
    // `self.method(...)` (i.e. MemberExpr on self) and the analyzer
    // resolved `method` to a registered IMethodInfo*. Codegen reads
    // this to emit `ayt_reflect_call_method(self, "<Type>", "<m>")`
    // instead of bare Lua dispatch.
    const ayt::reflect::IMethodInfo* resolvedMethod = nullptr;
    // Owning type name for the resolved method (the AYReflect
    // registry name of `self`). Codegen embeds this as the second
    // stack arg. nullptr when unresolved.
    const char*                      resolvedMethodOwnerName = nullptr;
    const void*                      resolvedDecl  = nullptr;
};

class BinaryExpr : public Expr {
public:
    BinaryExpr(ExprPtr left, Token op, ExprPtr right)
        : left(std::move(left)), op(op), right(std::move(right)) {}
    ExprPtr left;
    Token op;
    ExprPtr right;
};

class UnaryExpr : public Expr {
public:
    UnaryExpr(Token op, ExprPtr operand)
        : op(op), operand(std::move(operand)) {}
    Token op;
    ExprPtr operand;
};

class CallExpr : public Expr {
public:
    CallExpr(ExprPtr callee, std::vector<ExprPtr> args)
        : callee(std::move(callee)), args(std::move(args)) {}
    ExprPtr callee;
    std::vector<ExprPtr> args;
};

class IdentifierExpr : public Expr {
public:
    explicit IdentifierExpr(std::string name) : name(std::move(name)) {}
    std::string name;
};

class LiteralExpr : public Expr {
public:
    using Value = std::variant<std::monostate, bool, float, int, std::string>;
    explicit LiteralExpr(Value value) : value(std::move(value)) {}
    Value value;
};

class MemberExpr : public Expr {
public:
    MemberExpr(ExprPtr object, std::string member)
        : object(std::move(object)), member(std::move(member)) {}
    ExprPtr object;
    std::string member;
};

class IndexExpr : public Expr {
public:
    IndexExpr(ExprPtr object, ExprPtr index)
        : object(std::move(object)), index(std::move(index)) {}
    ExprPtr object;
    ExprPtr index;
};

// S3.12+R3: table literal `{k1=v1, k2=v2, ...}`. Produced by the
// parser when it sees a `{` after an expression position. Each
// entry is `(key_expr, value_expr)`; the analyzer / codegen
// builds a Lua table on the stack and indexes it by string key.
// We use ExprPtr (not Token-based key) so the key can be any
// expression — but in S3.12+R3 the analyzer only stamps `key`
// for IdentifierExpr (field-name lookup). Anonymous entries
// (no `=`, just `value`) are stored as `(nullptr, value_expr)`.
// Currently not used by the parser; reserved for R3.5+ where the
// parser will produce table-literal nodes.
class TableExpr : public Expr {
public:
    struct Entry {
        ExprPtr key;     // nullptr for array-style positional entry
        ExprPtr value;
    };
    TableExpr(std::vector<Entry> entries)
        : entries(std::move(entries)) {}
    std::vector<Entry> entries;
};

class Stmt {
public:
    virtual ~Stmt() = default;
};

class ExprStmt : public Stmt {
public:
    explicit ExprStmt(ExprPtr expr) : expr(std::move(expr)) {}
    ExprPtr expr;
};

class VarDeclStmt : public Stmt {
public:
    // S2.5: `export` keyword removed. Logia `var` is always a pure
    // Lua local. C++ fields are declared via AY_PROPERTY on the
    // ScriptComponent subclass and accessed via `self.field`.
    VarDeclStmt(std::string name, std::string typeName, ExprPtr initializer)
        : name(std::move(name)),
          typeName(std::move(typeName)),
          initializer(std::move(initializer)) {}
    std::string name;
    std::string typeName;
    ExprPtr initializer;

    // ---- S2: SemanticAnalyzer resolves `typeName` to a registered type. ----
    // nullptr when unresolved (built-in or unregistered — analyzer reports
    // an error in the latter case before stamping null).
    const ayt::reflect::ITypeInfo* resolvedType = nullptr;
};

class ReturnStmt : public Stmt {
public:
    explicit ReturnStmt(ExprPtr value) : value(std::move(value)) {}
    ExprPtr value;
};

class IfStmt : public Stmt {
public:
    IfStmt(ExprPtr condition, std::vector<StmtPtr> thenBranch, std::vector<StmtPtr> elseBranch)
        : condition(std::move(condition)),
          thenBranch(std::move(thenBranch)),
          elseBranch(std::move(elseBranch)) {}
    ExprPtr condition;
    std::vector<StmtPtr> thenBranch;
    std::vector<StmtPtr> elseBranch;
};

// R5.0 (2026-07-13): loop statements. See design.md §5.7.6.x.
//
// WhileStmt: `while (cond) { body }` → Lua `while <cond> do ... end`.
//   - `condition` is a Logia boolean expression. The parser consumes
//     the surrounding parens; codegen emits the expression verbatim
//     (parens around BinaryExpr are preserved by emitExpr).
//   - `body` is a non-empty vector (empty body is allowed but unusual).
//   - `condition` is re-evaluated each iteration at runtime (Lua
//     semantics; no precomputation).
class WhileStmt : public Stmt {
public:
    WhileStmt(ExprPtr condition, std::vector<StmtPtr> body)
        : condition(std::move(condition)), body(std::move(body)) {}
    ExprPtr condition;
    std::vector<StmtPtr> body;
};

// ForStmt: `for (var i : N) { body }` → Lua `for i = 1, N do ... end`.
//   - `counterName` is the user-written identifier (always `int`; the
//     type is implicit — Logia for-loops are integer counters only).
//   - `bound` is the upper-bound expression; emitted verbatim into
//     the `N` slot. Any integer-valued expression is accepted
//     (literal, identifier, function call, arithmetic).
//   - The counter starts at 1 and runs to `bound` inclusive (Lua
//     numeric-for default; matches user expectation per R5.0 spec).
//   - The counter is implicitly loop-local at the Lua level; the
//     analyzer does NOT push it into `_scope` (would cause a name
//     leak in analyzer view vs. runtime behavior).
class ForStmt : public Stmt {
public:
    ForStmt(std::string counterName, ExprPtr bound, std::vector<StmtPtr> body)
        : counterName(std::move(counterName)),
          bound(std::move(bound)),
          body(std::move(body)) {}
    std::string counterName;
    ExprPtr bound;
    std::vector<StmtPtr> body;
};

struct Param {
    std::string name;
    std::string typeName;
};

class LifecycleFuncDecl : public Stmt {
public:
    // S2.5: lifecycle functions take no parameters. The parser
    // records the original parameter list (so SemanticAnalyzer can
    // emit a soft warning for old-style `on_start(entity: Entity)` /
    // `on_update(dt: float)` code) but emits the Lua function with
    // just `(self)` — the params don't reach Lua.
    LifecycleFuncDecl(LifecycleKind kind, std::vector<Param> params, std::vector<StmtPtr> body)
        : kind(kind), params(std::move(params)), body(std::move(body)) {}
    LifecycleKind kind;
    std::vector<Param> params;   // S2.5: diagnostic only, not emitted
    std::vector<StmtPtr> body;
};

// 2026-07-11 audit fix: user-defined script-block-scope helper.
// Shaped after `LifecycleFuncDecl` (same `params` + `body` model),
// but emits as a top-level Lua function via `emitFunctionDecl`
// (no `M.` prefix; reachable from every lifecycle body).
//
// Restriction: the parser only accepts this form when called from
// `parseScriptMembers`. Inside lifecycle bodies or anywhere else
// the lexer sees `function`, the parser rejects with an explicit
// error message — see `parseStatement` for the gate.
class FunctionDeclStmt : public Stmt {
public:
    FunctionDeclStmt(std::string name,
                     std::vector<Param> params,
                     std::vector<StmtPtr> body)
        : name(std::move(name)),
          params(std::move(params)),
          body(std::move(body)) {}
    std::string name;
    std::vector<Param> params;
    std::vector<StmtPtr> body;
};

class ScriptDecl {
public:
    ScriptDecl(std::string name, std::vector<StmtPtr> members)
        : name(std::move(name)), members(std::move(members)) {}
    std::string name;
    std::vector<StmtPtr> members;

    // LG-05 / S3.3: stamped by SemanticAnalyzer for codegen. The
    // AYReflect-registered type name matching `name` — used to
    // emit `ayt_reflect_*_field(self, "<hostTypeName>", "<f>")`
    // for `self.field` reads/writes. Empty when the script's name
    // did not resolve (codegen falls back to bare `self.<field>`
    // access, same as S2.5).
    std::string hostTypeName;
};

class Program {
public:
    explicit Program(std::vector<std::unique_ptr<ScriptDecl>> scripts)
        : scripts(std::move(scripts)) {}
    std::vector<std::unique_ptr<ScriptDecl>> scripts;
};

const char* lifecycleKindName(LifecycleKind kind);

} // namespace ayt::script::logia
