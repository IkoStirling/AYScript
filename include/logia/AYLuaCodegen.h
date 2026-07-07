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
#include <string>
#include <vector>

namespace ayt::script::logia
{

struct LuaCodegenOptions {
    // Script name for header comment; not required for codegen correctness.
    std::string scriptName;
};

struct LuaCodegenResult {
    bool success = false;
    std::vector<CompilerError> errors;
    // Generated Lua source. Empty when success == false (and when codegen
    // produces a recoverable but partial source we still keep it for debug).
    std::string source;
};

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

    void emitBlock(const std::vector<StmtPtr>& body);
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
    void emitReturnStmt(const ReturnStmt& stmt);
    void emitExprStmt(const ExprStmt& stmt);

    // Helpers
    void line(const std::string& s = {});
    void indent();
    void dedent();
    std::string escapeString(const std::string& s) const;
    void errorAt(const Token& tok, const std::string& message);

    LuaCodegenOptions _options;
    std::string _out;
    int _indent = 0;
    // Temporary counter for `__tmp` variables used in compound assignment
    // and chained member assignment lowering.
    int _tmpCounter = 0;
    std::string freshTmp(const std::string& hint = "");

    std::vector<CompilerError> _errors;
};

} // namespace ayt::script::logia