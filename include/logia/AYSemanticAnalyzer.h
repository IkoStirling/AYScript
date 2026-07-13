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

private:
    void analyzeScript(ScriptDecl& s);
    void analyzeVarDecl(VarDeclStmt& v);
    void analyzeLifecycle(LifecycleFuncDecl& fn);
    void analyzeWhileStmt(WhileStmt& w);   // R5.0 (2026-07-13)
    void analyzeForStmt(ForStmt& f);       // R5.0 (2026-07-13)
    void analyzeStmt(Stmt& s);
    void analyzeExpr(Expr& e);
    void analyzeMemberExpr(MemberExpr& m, const ayt::reflect::ITypeInfo* parent);
    void analyzeIdentifierExpr(IdentifierExpr& id);

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

    static bool isBuiltInType(const std::string& name);
    static bool isAmbientIdentifier(const std::string& name);

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