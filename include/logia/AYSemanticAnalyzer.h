#pragma once
// AYSemanticAnalyzer.h - Logia semantic analyzer (S2.5)
//
// S2.5 redesign: `self` is the canonical way to access the bound
// ScriptComponent. The analyzer resolves:
//   - `script Name`           — must match a registered ScriptComponent subclass
//   - `var x: T`              — T is a known type (builtin or AYReflect-registered)
//   - `self.field` / `self.field.subfield` — must resolve against AYReflect fields
//
// Hard errors:
//   - Unknown type name on a var declaration
//   - Unknown script name (must match a ScriptComponent subclass)
//   - Reference to an undeclared identifier
//   - Lifecycle function declared with parameters
//
// Soft warnings (compile passes):
//   - Member access where the field doesn't exist on a registered type
//   - Undeclared identifier read (Lua-style implicit global)

#include "AYAst.h"
#include "AYCompilerError.h"

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
    explicit SemanticAnalyzer(SemanticOptions options = {});
    ~SemanticAnalyzer();

    SemanticAnalyzer(const SemanticAnalyzer&) = delete;
    SemanticAnalyzer& operator=(const SemanticAnalyzer&) = delete;

    [[nodiscard]] SemanticResult analyze(Program& program);

    // Inject a custom registry (test hook).
    void setTypeProvider(ayt::reflect::ITypeRegistry* reg);

private:
    void analyzeScript(ScriptDecl& s);
    void analyzeVarDecl(VarDeclStmt& v);
    void analyzeLifecycle(LifecycleFuncDecl& fn);
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
    // ScriptComponent subclass. Returns nullptr on miss. The script's
    // own `self` field gets this type via `currentSelfType` during
    // member analysis.
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

    // Type of the currently-analyzed `self` (the ScriptComponent
    // subclass matching the script's name). nullptr if no current
    // script (shouldn't happen inside analyzeScript).
    const ayt::reflect::ITypeInfo* _currentSelfType = nullptr;

    const std::string _fileName;
};

} // namespace ayt::script::logia