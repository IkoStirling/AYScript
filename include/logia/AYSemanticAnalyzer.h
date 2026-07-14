#pragma once
// AYSemanticAnalyzer.h - Logia semantic analyzer (S2.5 + S3.0 LG-03)
//
// S2.5: `self` is the canonical way to access the bound ScriptComponent.
// S3.0 (LG-03): the analyzer additionally takes a `LogiaHostContext`
// describing which C++ host kind a `script` block is being bound to.
// LG-03 stores the context but does NOT enable new validation —
// existing S2.5 diagnostics are preserved verbatim. Future S3.1+
// host-aware validation will consult `_ctx`.
//
// Resolves:
//   - `script Name`           — must match a registered C++ host type
//                               (S2.5: ScriptComponent subclass; S3.1+:
//                               host-kind-specific). Unknown name is a
//                               soft warning in LG-03.
//   - `var x: T`              — T is a known type (builtin or AYReflect-registered)
//   - `self.field` / `self.field.subfield` — must resolve against AYReflect fields
//
// Hard errors:
//   - Unknown type name on a var declaration
//   - Unknown script name (must match a registered host type)
//   - Reference to an undeclared identifier
//   - Lifecycle function declared with parameters
//
// Soft warnings (compile passes):
//   - Member access where the field doesn't exist on a registered type
//   - Undeclared identifier read (Lua-style implicit global)

#include "AYAst.h"
#include "AYCompilerError.h"
#include "AYLogia.h"  // S3.0 (LG-03): LogiaHostContext

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ayt::reflect
{
class ITypeRegistry;
class TypeRegistryImpl;
class ITypeInfo;
class IFieldInfo;
} // namespace ayt::reflect

namespace ayt::script::logia
{

struct SemanticOptions {
    bool useCompileTimeTypes = false;
    const char* fileName = "";
};

struct SemanticResult {
    std::vector<LogiaDiagnostic> diagnostics;

    bool hasErrors() const
    {
        for (const auto& d : diagnostics) {
            if (d.severity == DiagnosticSeverity::Error) return true;
        }
        return false;
    }

    size_t warningCount() const
    {
        size_t n = 0;
        for (const auto& d : diagnostics) {
            if (d.severity == DiagnosticSeverity::Warning) ++n;
        }
        return n;
    }
};

class SemanticAnalyzer {
public:
    // LG-03: ctor accepts an optional host context. Defaults to the
    // S2.5 Component host context, so all existing call sites that
    // construct `SemanticAnalyzer` with no context keep their previous
    // behavior (hostType=nullptr, kind=Component, expectSelf=true).
    explicit SemanticAnalyzer(SemanticOptions options = {},
                              const LogiaHostContext& ctx = defaultLogiaHostContext());
    ~SemanticAnalyzer();

    SemanticAnalyzer(const SemanticAnalyzer&) = delete;
    SemanticAnalyzer& operator=(const SemanticAnalyzer&) = delete;

    [[nodiscard]] SemanticResult analyze(Program& program);

    // Inject a custom registry (test hook).
    void setTypeProvider(ayt::reflect::ITypeRegistry* reg);

    // LG-03: read-only access to the host context the analyzer was
    // constructed with. Used by unit tests to assert plumbing.
    const LogiaHostContext& hostContext() const { return _ctx; }

    // R5.2-H.b (2026-07-14): public type-name helpers and
    // for-loop-counter accessor. These are exposed because
    // file-local condition-validator predicates (in
    // AYSemanticAnalyzer.cpp) need to consult them but cannot
    // be friend-declared without polluting the header with
    // unordered_set. Public boolean accessors are simpler
    // than friend declarations. The underlying implementations
    // are unchanged from where they lived as private statics.
    static bool isBuiltInType(const std::string& name);
    static bool isAmbientIdentifier(const std::string& name);
    bool isLoopCounter(const std::string& name) const;

private:
    void analyzeScript(ScriptDecl& s);
    void analyzeVarDecl(VarDeclStmt& v);
    void analyzeLifecycle(LifecycleFuncDecl& fn);
    void analyzeWhileStmt(WhileStmt& w);   // R5.0 (2026-07-13)
    void analyzeForStmt(ForStmt& f);       // R5.0 (2026-07-13)
    void analyzeBreakStmt(BreakStmt& b);   // R5.1 (2026-07-13): no-op (parser already gated)
    void analyzeContinueStmt(ContinueStmt& c); // R5.1 (2026-07-13): no-op
    void analyzeLabelDeclStmt(LabelDeclStmt& l); // R5.2-B (2026-07-14)
    void analyzeStmt(Stmt& s);
    void analyzeExpr(Expr& e);
    void analyzeMemberExpr(MemberExpr& m, const ayt::reflect::ITypeInfo* parent);
    void analyzeIdentifierExpr(IdentifierExpr& id);

    // R5.2-C (2026-07-14): verify a `for (var i : <bound>)` bound
    // expression statically reduces to int. Recurses into
    // BinaryExpr / UnaryExpr; otherwise checks LiteralExpr /
    // IdentifierExpr / MemberExpr / CallExpr resolvedType. Anything
    // not statically provable as int emits a TypeMismatch hard error.
    //
    // `role` is `"bound"` or `"start"` (R5.0.1 half-open range form)
    // — used in the diagnostic message so the user knows which slot
    // failed. `loc` is currently always empty (Expr / ForStmt don't
    // carry source locations yet); future slice can stamp them.
    void validateForBound(Expr* bound,
                          const SourceLocation& loc,
                          const std::string& role);

    // R5.2-H (2026-07-14): condition validator for `if (cond)` /
    // `while (cond)`. The condition must statically reduce to bool
    // (see isStaticallyBool for the decision matrix). Anything not
    // statically provable as bool emits a TypeMismatch hard error
    // with a hint pointing at the bool type contract. Logia has
    // no C-style truthiness — `while (n)` where `n: int` is a
    // hard error, not a silent truthy-coerce.
    //
    // `role` is `"if"` or `"while"` — used in the diagnostic so the
    // user knows which slot failed.
    void validateCondition(Expr* cond,
                           const SourceLocation& loc,
                           const std::string& role);

    // R5.2-B (2026-07-14): label visibility helper. Walks
    // `_labelStack` from innermost frame outward and returns true
    // on first match. Returns false when the stack is empty.
    bool isLabelVisible(const std::string& name) const;

    // Resolve a type name (string) to an ITypeInfo*. Returns nullptr on
    // miss. Built-in types (int/float/bool/string/Entity) return nullptr
    // (no ITypeInfo*) but the caller treats them as valid.
    const ayt::reflect::ITypeInfo* resolveTypeName(const std::string& name,
                                                   int line, int column);

    // Resolve a script name (string) to an ITypeInfo* of the bound
    // host type. Returns nullptr on miss. The script's own `self`
    // field gets this type via `_currentSelfType` during member
    // analysis. S3.0 (LG-03) keeps the S2.5 lookup path: a single
    // `findType(name)` against the registry, no host-kind-specific
    // branch, no subclass check.
    const ayt::reflect::ITypeInfo* resolveScriptName(const std::string& name,
                                                     int line, int column);

    void report(LogiaDiagnostic d);

    SemanticOptions _options;

    ayt::reflect::ITypeRegistry*       _registry     = nullptr;
    ayt::reflect::TypeRegistryImpl*    _registryImpl = nullptr;

    std::vector<LogiaDiagnostic> _diagnostics;

    // Per-component scope: name → (type, decl).
    struct ScopeEntry {
        const ayt::reflect::ITypeInfo* type = nullptr;
        const void* decl = nullptr;
    };
    std::unordered_map<std::string, ScopeEntry> _scope;

    // R5.2-B (2026-07-14): stack of label-name sets, one frame per
    // enclosing loop body. Pushed in analyzeWhileStmt /
    // analyzeForStmt (loop-scope frames); analyzeLifecycle is the
    // outermost function-body frame (kept empty-of-labels by the
    // rule that labels only live in loop bodies, but pushed anyway
    // so the stack is non-empty when descending into a loop body).
    // Popped on exit. The vector is empty outside a function body.
    std::vector<std::unordered_set<std::string>> _labelStack;

    // R5.2-H (2026-07-14): stack of for-loop counter names, one
    // frame per enclosing ForStmt body. R5.2-C/D/E deliberately
    // omitted the counter from `_scope` (so post-loop references
    // stay implicit-globals and Lua's `for i = 1, N do ... end`
    // loop-locality is preserved), but R5.2-H's
    // `leafIsStaticallyPrimitive` predicate needs to know that
    // `j` in `for (var j : 3) { if (j == 1) { ... } }` is an
    // int-shaped leaf — otherwise `j == 1` (which IS provably
    // bool, returning bool from any sane comparison) is rejected
    // because `j.resolvedType` and `j.resolvedDecl` are both
    // null (counter is deliberately not in `_scope`). Pushed
    // in analyzeForStmt, popped on exit. Empty outside a for
    // body. The vector-of-unordered_set shape mirrors
    // `_labelStack` for symmetry (each loop body frame is its
    // own set of named locals).
    std::vector<std::unordered_set<std::string>> _loopCounters;

    // Type of the currently-analyzed `self` (the host type matching
    // the script's name). nullptr if no current script (shouldn't
    // happen inside analyzeScript).
    const ayt::reflect::ITypeInfo* _currentSelfType = nullptr;

    const std::string _fileName;

    // S3.0 (LG-03): host context captured at construction time. S3.0
    // does NOT consume it for validation — it is stored so the test
    // suite can confirm plumbing, and so S3.1+ can read it without
    // another signature churn.
    LogiaHostContext _ctx;
};

} // namespace ayt::script::logia