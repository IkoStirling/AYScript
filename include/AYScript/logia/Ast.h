#pragma once
// AYShader\Ast.h - AST node definitions for Logia (S0/S1/S2) + S2.5 redesign
//
// S2.5 redesign (2026-07-07): `component` keyword renamed to `script`,
// `export` keyword removed, `ComponentDecl` renamed to `ScriptDecl`,
// `VarDeclStmt::exported` field removed. Logia `var` is now always a
// pure Lua local; C++ fields are declared on the ScriptComponent
// subclass via AY_PROPERTY and accessed through `self.field`.

#include "AYScript/logia/Token.h"
#include "AYScript/logia/CompilerError.h"   // S5 ED-02 (2026-07-14): SourceLocation for sourceLoc
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

    // S5 ED-02 (2026-07-14): source location stamped by the parser
    // at construction. Read by `SemanticAnalyzer::sourceLocFor(Expr*)`
    // to populate `LogiaDiagnostic::location` for analyzer-side
    // errors. Default-constructed means `{}` (0:0) — preserves the
    // pre-ED-02 behavior for any AST node the parser didn't stamp
    // (e.g. diagnostic emits on a half-built node during error
    // recovery). The "Future slice can add Expr-level source
    // locations" comment in `AYSemanticAnalyzer.cpp:564` is fulfilled
    // by this slice.
    SourceLocation sourceLoc;
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

    // S4.1 (2026-07-15): ambient-call recognition tag. The
    // SemanticAnalyzer stamps this when `callee` is an
    // `IdentifierExpr` matching a known ambient free-function name
    // (currently `emit` / `connect`). Codegen reads it to emit the
    // specialized lowering (`__ay_emit(self, ...)` /
    // `__ay_connect(self, ..., handler)`) instead of falling through
    // to the generic Lua call path — which would invoke a global
    // `emit` / `connect` that doesn't exist and fail at runtime.
    //
    // Default `None` preserves source-compat: any CallExpr produced
    // by code paths that don't know about ambients still works as
    // before (generic lowering emits `callee(args)`).
    //
    // S4.1b (2026-07-18): append `Disconnect` for the
    // `disconnect(id)` ambient — `id` is the int returned by
    // `connect(...)`. Analyzer stamps this when the callee
    // identifier matches, codegen lowers to `__ay_disconnect(self,
    // id)`. Disconnect shares the same shape-recognition pattern
    // as Emit / Connect (free function, IdentifierExpr callee).
    enum class AmbientCallKind { None, Emit, Connect, Disconnect };
    AmbientCallKind ambientCall = AmbientCallKind::None;
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

    // S5 ED-02 (2026-07-14): source location stamped by the parser
    // at construction. Read by `SemanticAnalyzer::sourceLocFor(Stmt*)`
    // (a dynamic_cast-switch file-local helper in
    // AYSemanticAnalyzer.cpp) to populate `LogiaDiagnostic::location`.
    // Placed on the Stmt base (rather than per concrete subclass)
    // because every concrete Stmt subclass inherits it for free —
    // 1 field covers all 13 subclasses, eliminating 13 duplicate
    // declarations. The dynamic_cast switch in sourceLocFor is
    // the small price for centralizing on the base; future slices
    // can convert it to a virtual `sourceLoc()` if/when the
    // cast chain becomes a measured hotspot.
    SourceLocation sourceLoc;
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

// R5.1 (2026-07-13): `break` and `continue` inside loop bodies.
// Both lower to Lua's native `break` / `continue` (Lua 5.2+;
// AYScript runs on Lua 5.5 per the runtime bridge contract).
// Parser enforces "must be inside a loop" via loopDepth — a bare
// `break` at script-block or function-body scope is a hard error
// with message "break outside loop" (and same for continue).
//
// R5.2-B (2026-07-14): each may carry an optional label name.
// `break;` (or `break` with empty label) lowers to Lua `break`.
// `break :L` lowers to Lua `goto L` — the only way to exit an
// outer loop from a nested loop body. Same for `continue`. The
// parser consumes the optional `:IDENT` after the keyword;
// codegen emits `goto <label>` when label is non-empty.
class BreakStmt : public Stmt {
public:
    explicit BreakStmt(std::string label = "")
        : label(std::move(label)) {}
    std::string label;
};

class ContinueStmt : public Stmt {
public:
    explicit ContinueStmt(std::string label = "")
        : label(std::move(label)) {}
    std::string label;
};

// R5.2-A (2026-07-13): explicit block-scope statement. Logia-side
// source: `do { <stmts> } end`. Semantically identical to the
// user's existing `{ <stmts> }` braces that control-flow
// statements (while / for / if / else / function) use for bodies.
// The `do` keyword is the entry marker and `end` the closer.
// Lowered to Lua 5.2+ native `do ... end`.
//
// `do` is NOT a do-while loop keyword — matches Lua semantics
// (Lua's `do ... end` is a pure block, not a post-test loop).
class BlockStmt : public Stmt {
public:
    explicit BlockStmt(std::vector<StmtPtr> body) : body(std::move(body)) {}
    std::vector<StmtPtr> body;
};

// R5.2-B (2026-07-14): `::NAME::` label declaration. Valid only
// inside a loop body (while / for) — the analyzer enforces this
// via a stack of label-scope frames pushed/popped by
// analyzeWhileStmt / analyzeForStmt. A label's visibility extends
// outward — a `break :L` deep in the body targets the nearest
// enclosing `::L::`, and Lua's `goto` reaches the same name.
//
// Codegen emits Lua 5.2+ `::NAME::` natively.
class LabelDeclStmt : public Stmt {
public:
    explicit LabelDeclStmt(std::string name) : name(std::move(name)) {}
    std::string name;
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
//
// R5.0.1 (2026-07-13) — optional `start, end` range form:
//   `for (var i : start, end) { body }` → `for i = start, end - 1 do`.
//   When `start` is non-null, the for-loop has an explicit half-open
//   range `[start, end)` — emitted as `for i = start, (end - 1) do`
//   so Lua's inclusive numeric-for semantics match the user's
//   half-open expectation. When `start` is null, the legacy R5.0
//   1..N inclusive form is used.
class ForStmt : public Stmt {
public:
    // R5.0 short form: `for (var i : N) { body }` → 1..N inclusive.
    // R5.2-D (2026-07-14): short form does NOT accept a step —
    // users who want a step must use the range form below.
    ForStmt(std::string counterName,
            ExprPtr bound,
            std::vector<StmtPtr> body)
        : counterName(std::move(counterName)),
          start(nullptr),
          bound(std::move(bound)),
          step(nullptr),                              // R5.2-D
          body(std::move(body)) {}
    // R5.0.1 range form: `for (var i : start, end) { body }` → [start, end).
    // R5.2-D (2026-07-14): optional `step` (`for (var i : start, end, step)`).
    // `step` defaults to nullptr, meaning default Lua step = +1.
    ForStmt(std::string counterName,
            ExprPtr start,
            ExprPtr end,
            std::vector<StmtPtr> body,
            ExprPtr step = nullptr)                   // R5.2-D
        : counterName(std::move(counterName)),
          start(std::move(start)),
          bound(std::move(end)),
          step(std::move(step)),                     // R5.2-D
          body(std::move(body)) {}
    std::string counterName;
    // R5.0.1: non-null for `for (var i : start, end)` range form.
    // Null for the legacy R5.0 `for (var i : N)` short form.
    ExprPtr start;
    ExprPtr bound;
    // R5.2-D (2026-07-14): optional step (e.g. `for (var i : 0, 10, 2)`).
    // Null → default +1. Only ever non-null when `start` is also
    // non-null (parser ensures this); short form has no step.
    ExprPtr step;
    std::vector<StmtPtr> body;
};

struct Param {
    std::string name;
    std::string typeName;
};

class LifecycleFuncDecl : public Stmt {
public:
    // Lifecycle parameters are forwarded to the generated Lua function
    // after the optional `self` receiver. The semantic analyzer also
    // exposes them only within this function body's scope.
    LifecycleFuncDecl(LifecycleKind kind, std::vector<Param> params, std::vector<StmtPtr> body)
        : kind(kind), params(std::move(params)), body(std::move(body)) {}
    LifecycleKind kind;
    std::vector<Param> params;
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

// S4.1 (2026-07-15): per-component signal declaration. Lives as a
// metadata-only member of `ScriptDecl` (alongside `VarDeclStmt`,
// `LifecycleFuncDecl`, `FunctionDeclStmt`). Carries NO body — the
// signal is just a named event with a typed parameter list. The
// analyzer collects these in a first pass so that forward references
// work (e.g. `on_start { connect("damaged", on_damaged) }` can
// reference a signal declared textually below it). Codegen reads
// the names for the `_signalNames` module table and seeds the
// per-instance `_signals` map lazily at first connect.
//
// Restriction: same as `FunctionDeclStmt` — the parser only accepts
// this form when called from `parseScriptMembers`; inside lifecycle
// bodies or anywhere else the lexer sees `signal`, the parser
// rejects with an explicit error (see `parseStatement`).
class SignalDeclStmt : public Stmt {
public:
    SignalDeclStmt(std::string name, std::vector<Param> params)
        : name(std::move(name)), params(std::move(params)) {}
    std::string name;
    std::vector<Param> params;
};

class ScriptDecl {
public:
    ScriptDecl(std::string name, std::vector<StmtPtr> members)
        : name(std::move(name)), members(std::move(members)) {}
    std::string name;
    std::vector<StmtPtr> members;

    // S5 ED-02 (2026-07-14): source location of the `script` keyword
    // (set by the parser). Used by analyzer-side diagnostics
    // attached to the script-level declaration (e.g. "script 'Bogus'
    // has no matching registered type" warning, or strictInheritance
    // hard errors). Stamped by `parseScript` in AYParser.cpp.
    SourceLocation sourceLoc;

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
