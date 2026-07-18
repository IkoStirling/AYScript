#pragma once
// AYLuaCodegen.h - AST → Lua source generator for Logia (S1) + S2.5
//
// S2.5 redesign: `component` → `script`, `export var` removed, all `var`
// are local Lua variables. The generated Lua module exposes `M.on_*`
// functions that receive `self` (lightuserdata) as the first argument.
// Field access on `self` is left as bare Lua member syntax — S3 will
// route it through AYReflect-backed accessors.

#include "AYAst.h"
#include "AYCompilerError.h"
#include "AYLogia.h"  // S3.8b: LogiaHostContext for expectSelf-aware codegen.
#include <string>
#include <vector>

namespace ayt::script::logia
{

struct LuaCodegenOptions {
    // Script name for header comment; not required for codegen correctness.
    std::string scriptName;
    // S3.8b: optional host context. When `expectSelf == false` the
    // codegen omits the `self` parameter from emitted lifecycle
    // functions (used by the Tool host's `run()` entry point so the
    // generated Lua signature matches the no-receiver contract). The
    // default `LogiaHostContext{}` mirrors S2.5 (kind=Component,
    // expectSelf=true) so existing callers that construct a bare
    // options struct keep emitting `function M.on_*(self)`.
    LogiaHostContext hostContext = defaultLogiaHostContext();
};

// S5 ED-03 (2026-07-15): per-line source map produced by LuaCodegen
// alongside the generated Lua source. Index N is the Logia
// SourceLocation corresponding to the Lua line emitted at Lua line N
// (1-based). Index 0 is reserved as a sentinel (zero / "no anchor")
// to match Lua's 1-based line numbering — callers always check
// `luaLine >= 1 && luaLine < luaLineToSource.size()` before
// dereferencing. Memory footprint: one SourceLocation per emitted
// Lua line; a typical ~50-line script's map is trivial.
//
// The `file` field of each SourceLocation is empty at codegen time —
// the bridge populates it from the originating `.logia` path on
// consumption (codegen has no knowledge of the script's filesystem
// path; the source passed to `Compiler::compile` is a string).
struct LogiaSourceMap {
    std::vector<SourceLocation> luaLineToSource;

    // Look up the Logia SourceLocation for a Lua line. Returns
    // `{}` (line 0) when `luaLine` is out of range. Codegen
    // initializes every emitted line, so the in-range case is the
    // common path; the out-of-range check fails closed (no assert)
    // because Lua-side tracebacks may reference lines outside the
    // script chunk (e.g. internal Lua trampolines) — those simply
    // surface as "untranslatable" to the bridge.
    SourceLocation lookup(int luaLine) const;
};

struct LuaCodegenResult {
    bool success = false;
    std::vector<CompilerError> errors;
    // Generated Lua source. Empty when success == false (and when codegen
    // produces a recoverable but partial source we still keep it for debug).
    std::string source;
    // S5 ED-03 (2026-07-15): per-line Lua-line → Logia-source-location
    // map produced alongside `source`. Same lifetime semantics
    // (populated when success == true). Empty when codegen never
    // reached the emit pass (e.g. parser produced no script decl).
    LogiaSourceMap sourceMap;
};

// For compile + codegen together, prefer compileLogiaToLua() in
// AYLogiaPipeline.h instead of stack-allocating Compiler + LuaCodegen
// in the same frame (MSVC Debug /GS stack-cookie issues).
class LuaCodegen {
public:
    explicit LuaCodegen(LuaCodegenOptions options = {});

    // Non-copyable: the codegen holds accumulators (_out, _indent,
    // _tmpCounter, _errors) that would silently double-emit if copied.
    LuaCodegen(const LuaCodegen&) = delete;
    LuaCodegen& operator=(const LuaCodegen&) = delete;

    // Generate Lua source from a successfully-parsed Program.
    // Caller should pass a non-null program; the parser's errors are
    // separate from codegen's — codegen assumes the AST is structurally OK
    // and only fails when an AST node cannot be lowered to Lua.
    [[nodiscard]] LuaCodegenResult generate(const Program& program);

private:
    void emitScript(const ScriptDecl& script);
    void emitLocalVar(const VarDeclStmt& var);
    void emitLifecycleFunc(const LifecycleFuncDecl& func);
    void emitFunctionDecl(const FunctionDeclStmt& fn);   // 2026-07-11 audit fix

    void emitBlock(const std::vector<StmtPtr>& body);
    void emitBlockSkipLabels(const std::vector<StmtPtr>& body);  // R5.2-B (2026-07-14)
    void emitStmt(const Stmt& stmt);

    // Expressions — return a string that is a valid Lua expression in the
    // current context. The string may contain `__tmp` temporaries that were
    // emitted via emitTmp() before the expression is read by the caller.
    std::string emitExpr(const Expr& expr);

    // Compound-assignment helpers — emit the equivalent expanded Lua.
    // Returns the right-hand side expression that should be assigned.
    std::string emitAssignmentTarget(const Expr& target);  // a.b.c → temp
    std::string emitCompoundRhs(const Expr& target, const std::string& op, const Expr& rhs);

    // Statement emitters
    void emitVarDecl(const VarDeclStmt& var);
    void emitIfStmt(const IfStmt& stmt);
    void emitWhileStmt(const WhileStmt& stmt);   // R5.0 (2026-07-13)
    void emitForStmt(const ForStmt& stmt);       // R5.0 (2026-07-13)
    void emitBreakStmt(const BreakStmt& stmt);   // R5.1 (2026-07-13)
    void emitContinueStmt(const ContinueStmt& stmt); // R5.1 (2026-07-13)
    void emitBlockStmt(const BlockStmt& stmt);  // R5.2-A (2026-07-13)
    void emitLabelDeclStmt(const LabelDeclStmt& stmt);  // R5.2-B (2026-07-14)

    // R5.2-B (2026-07-14): helper used by emitWhileStmt /
    // emitForStmt. Walks `body` (the loop's direct child
    // statements); emits each top-level `LabelDeclStmt`'s
    // `::L::` marker at the current indent. Caller invokes
    // this AFTER writing the loop's `end` keyword, so the
    // label lands immediately AFTER the loop. This is what
    // makes `break :L` work: the compiled `goto L` jumps to
    // the label at loop end, falling out of the loop.
    //
    // The label is NOT emitted inside the loop body because
    // Lua 5.2+ forbids re-declaring a label in the same block
    // (a loop's body is one block across all iterations).
    // emitBlockSkipLabels is paired with this helper to walk
    // the body and skip top-level labels at body-walk time.
    void hoistLabelsFromBody(const std::vector<StmtPtr>& body);
    void emitReturnStmt(const ReturnStmt& stmt);
    void emitExprStmt(const ExprStmt& stmt);

    // LG-05 / S3.3 helpers — return true if `e` is a single-hop
    // `self.<primitiveField>` leaf whose resolved type has no fields
    // of its own. Returns the leaf field name via out-param on the
    // second helper. Used by emitExprStmt to decide between the
    // legacy compound-assign lowering and a direct reflect call.
    static bool isSingleHopSelfFieldExpr(const Expr& e);
    static std::string singleHopSelfFieldName(const Expr& e);

    // S3.11: multi-hop self chain probe. Returns true and fills
    // `fieldNames` (root→leaf order) when `e` is a chain
    // `self.<f1>.<f2>...<leaf>` where every MemberExpr node has
    // `resolvedField != nullptr` and the leaf type is a primitive
    // (getFieldCount() == 0). Requires hopCount >= 2 — single-hop
    // matches keep using the S3.10 path.
    static bool isSelfFieldChainExpr(const Expr& e,
                                     std::vector<std::string>& fieldNames,
                                     std::size_t& hopCount);

    // Helpers
    void line(const std::string& s = {});   // legacy helper — kept but unused after S5 ED-03.
    void writeLine(const std::string& content);  // S5 ED-03 (2026-07-15): anchor-tracking replacement for `line()`. Writes a single Lua line and records its anchor in `_sourceMap`.
    void writeBlankLine();  // S5 ED-03 (2026-07-15): blank-line emitter for codegen readability; anchors to {} (no Logia source).
    void writeNoAnchor(const std::string& content);  // S5 ED-03 (2026-07-15): codegen preamble / epilogue line; anchors to {}.
    void indent();
    void dedent();
    std::string escapeString(const std::string& s) const;
    void errorAt(const Token& tok, const std::string& message);

    // S4.1 D-2: inside script-block `function` helpers, bare `self`
    // is not a Lua parameter (helpers are top-level). Emit the
    // module-local `__ay_bound_self` slot that `__ay_emit` sets for
    // the duration of handler dispatch — so connected handlers see
    // the same receiver the emit ran on.
    const char* selfLua() const;

    LuaCodegenOptions _options;
    std::string _out;
    int _indent = 0;
    // Temporary counter for `__tmp` variables used in compound assignment
    // and chained member assignment lowering.
    int _tmpCounter = 0;
    std::string freshTmp(const std::string& hint = "");

    // LG-05 / S3.3 — AYReflect host type name stamped by the most
    // recent `emitScript` call (read from `script.hostTypeName`).
    // Empty when the script name did not resolve in AYReflect;
    // codegen then skips the reflect-call rewrite and keeps S2.5
    // bare member access (a runtime no-op, matches existing tests).
    std::string _currentHostTypeName;

    // S4.1 D-2: true while emitting a script-block FunctionDeclStmt
    // body. Drives `selfLua()` toward `__ay_bound_self`.
    bool _emittingScriptHelper = false;

    // S5 ED-03 (2026-07-15): per-line Lua-line → Logia-source-location
    // map populated alongside `_out` by `writeLine()`. Exposed via
    // `LuaCodegenResult.sourceMap`. Index N is the Logia anchor for
    // the Lua line written at position N (1-based; index 0 reserved).
    LogiaSourceMap _sourceMap;
    // S5 ED-03: anchor set by `emitStmt()` to the current Stmt's
    // `sourceLoc`. Every line written by `writeLine()` records
    // `_currentAnchor` in `_sourceMap` until `emitStmt()` updates
    // it again. The header/footer emit path in `generate()` does
    // NOT update this — those lines map to `{}` (no anchor).
    SourceLocation _currentAnchor;
    // S5 ED-03: total emitted Lua lines (1-based). Drives
    // `_sourceMap.luaLineToSource` growth in `writeLine()`.
    int _luaLineCount = 0;

    std::vector<CompilerError> _errors;
};

} // namespace ayt::script::logia