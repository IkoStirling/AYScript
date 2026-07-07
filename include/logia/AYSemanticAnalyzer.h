#pragma once
// AYSemanticAnalyzer.h - Logia semantic analyzer (S2)
//
// Walks a parsed Logia `Program` and resolves type names against
// AYReflect's TypeRegistry. Stamps `Expr::resolvedType` /
// `VarDeclStmt::resolvedType` in place so downstream consumers
// (LuaCodegen, future type-directed optimizations) can read them.
//
// Hard errors (compile fails):
//   - Unknown type name on a var declaration or parameter
//   - Reference to an undeclared identifier
//   - `get_component(<unknown>)`
//
// Soft warnings (compile passes):
//   - Member access on a registered type where the field doesn't exist
//   - Calls to identifiers not in scope / not ambient

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
    // Toggle compile-time metadata table lookup. When false (default),
    // the analyzer only queries the runtime AYReflect TypeRegistry.
    // When true, `AY_SCRIPT_USE_COMPILE_TIME_TYPES` must also be defined
    // at compile time (set by the AYScript CMakeLists option).
    bool useCompileTimeTypes = false;

    // File name used in diagnostic locations. May be empty.
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

    // Walks `program`, stamps Expr/VarDeclStmt fields. Returns the
    // diagnostic list. The result.success / error semantics are decided
    // by the caller (`Compiler::compile`); this function always returns
    // whatever it observed.
    [[nodiscard]] SemanticResult analyze(Program& program);

    // Inject a custom registry. Defaults to the global AYReflect
    // TypeRegistry singleton. Used by tests to provide deterministic
    // type lookups without depending on AYEntity static-init ordering.
    void setTypeProvider(ayt::reflect::ITypeRegistry* reg);

private:
    void analyzeComponent(ComponentDecl& c);
    void analyzeVarDecl(VarDeclStmt& v);
    void analyzeLifecycle(LifecycleFuncDecl& fn);
    void analyzeStmt(Stmt& s);
    void analyzeExpr(Expr& e);
    void analyzeMemberExpr(MemberExpr& m, const ayt::reflect::ITypeInfo* parent);
    void analyzeCallExpr(CallExpr& c);
    void analyzeIdentifierExpr(IdentifierExpr& id);

    // Resolve a type name (string from VarDeclStmt.typeName) to an
    // ITypeInfo*. Returns nullptr on miss. Errors are reported via
    // `report()` for non-built-in names that miss.
    const ayt::reflect::ITypeInfo* resolveTypeName(const std::string& name,
                                                   int line, int column);

    void report(LogiaDiagnostic d);

    static bool isBuiltIn(const std::string& name);
    static bool isAmbientIdentifier(const std::string& name);
    static const char* builtinTypeName(const std::string& name); // "int"/"float"/etc. or nullptr

    SemanticOptions _options;

    // Default registry (process-wide). The default ctor captures this once
    // and reuses it; `setTypeProvider` overrides it.
    ayt::reflect::ITypeRegistry* _registry = nullptr;
    ayt::reflect::TypeRegistryImpl* _registryImpl = nullptr;
    bool _ownsRegistry = false;

    std::vector<LogiaDiagnostic> _diagnostics;

    // Per-component symbol table: name -> (type info, decl ptr).
    struct ScopeEntry {
        const ayt::reflect::ITypeInfo* type = nullptr;
        const void* decl = nullptr;
    };
    std::unordered_map<std::string, ScopeEntry> _scope;

    const std::string _fileName;
};

} // namespace ayt::script::logia