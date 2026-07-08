// AYSemanticAnalyzer.cpp - Logia semantic analyzer (S2.5 + S3.0 LG-03)
//
// S2.5: `script Name` binds to a C++ ScriptComponent subclass (looked up
// in AYReflect TypeRegistry). `self` is the canonical way to access
// fields on that component. `var` is a pure Lua local.
//
// S3.0 (LG-03): the analyzer ctor also takes a `LogiaHostContext`.
// LG-03 stores it but does NOT enable new validation — the S2.5
// lookup path (`findType(name)`) and S2.5 soft-warning diagnostics
// remain exactly as before. Future S3.1+ host-aware validation will
// read `_ctx.hostKind` / `_ctx.hostType` / `_ctx.expectSelf`; LG-03
// keeps the existing call signature stable for that future work.

#include "logia/AYSemanticAnalyzer.h"

#include "AYLogger.h"

// AYReflect surface
#include "IAYReflect.h"
#include "AYReflectRegistry.h"
#include "AYReflect.h"  // full TypeRegistryImpl definition (linkable)

// Force AYEntity's Transform component to be registered with AYReflect.
#include "components/AYTransformComponent.h"
#include "AYEntityModule.h"
#include "components/AYHealthComponent.h"
#include "AYReflectMacros.h"  // ayt::reflect::detail::defaultCreate/Destroy/Copy
#include <AYMathTypes.h>

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace ayt::script::logia
{

namespace
{

const std::unordered_set<std::string>& builtinTypeNames()
{
    static const std::unordered_set<std::string> s = {
        "int", "float", "bool", "string", "Entity"
    };
    return s;
}

const std::unordered_set<std::string>& ambientIdentifiers()
{
    static const std::unordered_set<std::string> s = {
        "input", "log", "time"
    };
    return s;
}

// Explicitly register known AYEntity component types with AYReflect.
void ensureAYEntityTypesRegistered()
{
    auto& reg = ayt::reflect::TypeRegistryImpl::instance();

    // FVector3 / FQuaternion primitives
    auto* fVec3 = reg.findType<ayt::math::FVector3>();
    if (!fVec3) {
        fVec3 = new ayt::reflect::TypeInfoImpl<ayt::math::FVector3>(
            "FVector3",
            ayt::reflect::detail::defaultCreate<ayt::math::FVector3>,
            ayt::reflect::detail::defaultDestroy<ayt::math::FVector3>,
            ayt::reflect::detail::defaultCopy<ayt::math::FVector3>);
        reg.registerTypeInfo("FVector3", fVec3);
    }
    auto* fQuat = reg.findType<ayt::math::FQuaternion>();
    if (!fQuat) {
        fQuat = new ayt::reflect::TypeInfoImpl<ayt::math::FQuaternion>(
            "FQuaternion",
            ayt::reflect::detail::defaultCreate<ayt::math::FQuaternion>,
            ayt::reflect::detail::defaultDestroy<ayt::math::FQuaternion>,
            ayt::reflect::detail::defaultCopy<ayt::math::FQuaternion>);
        reg.registerTypeInfo("FQuaternion", fQuat);
    }

    // Transform — always re-register so its field types are non-null.
    {
        auto* info = new ayt::reflect::TypeInfoImpl<ayt::entity::Transform>(
            "Transform",
            ayt::reflect::detail::defaultCreate<ayt::entity::Transform>,
            ayt::reflect::detail::defaultDestroy<ayt::entity::Transform>,
            ayt::reflect::detail::defaultCopy<ayt::entity::Transform>);
        if (fVec3) {
            info->addField(new ayt::reflect::FieldInfoImpl(
                "position", fVec3,
                offsetof(ayt::entity::Transform, position),
                ayt::reflect::FieldAttribute::Serialize));
            info->addField(new ayt::reflect::FieldInfoImpl(
                "scale", fVec3,
                offsetof(ayt::entity::Transform, scale),
                ayt::reflect::FieldAttribute::Serialize));
        }
        if (fQuat) {
            info->addField(new ayt::reflect::FieldInfoImpl(
                "rotation", fQuat,
                offsetof(ayt::entity::Transform, rotation),
                ayt::reflect::FieldAttribute::Serialize));
        }
        reg.registerTypeInfo("Transform", info);
    }

    if (!reg.findType("HealthComponent")) {
        auto* info = new ayt::reflect::TypeInfoImpl<ayt::entity::HealthComponent>(
            "HealthComponent",
            ayt::reflect::detail::defaultCreate<ayt::entity::HealthComponent>,
            ayt::reflect::detail::defaultDestroy<ayt::entity::HealthComponent>,
            ayt::reflect::detail::defaultCopy<ayt::entity::HealthComponent>);
        reg.registerTypeInfo("HealthComponent", info);
    }
}

} // namespace

SemanticAnalyzer::SemanticAnalyzer(SemanticOptions options, const LogiaHostContext& ctx)
    : _options(options)
    , _fileName(options.fileName ? options.fileName : "")
    , _ctx(ctx)
{
    _registryImpl = &ayt::reflect::TypeRegistryImpl::instance();
    _registry     = _registryImpl;
    ensureAYEntityTypesRegistered();
}

SemanticAnalyzer::~SemanticAnalyzer() = default;

void SemanticAnalyzer::setTypeProvider(ayt::reflect::ITypeRegistry* reg)
{
    _registry = reg;
    _registryImpl = static_cast<ayt::reflect::TypeRegistryImpl*>(reg);
}

bool SemanticAnalyzer::isBuiltInType(const std::string& name)
{
    return builtinTypeNames().count(name) > 0;
}

bool SemanticAnalyzer::isAmbientIdentifier(const std::string& name)
{
    return ambientIdentifiers().count(name) > 0;
}

void SemanticAnalyzer::report(LogiaDiagnostic d)
{
    if (d.location.file.empty() && !_fileName.empty()) {
        d.location.file = _fileName;
    }
    _diagnostics.push_back(std::move(d));
}

const ayt::reflect::ITypeInfo*
SemanticAnalyzer::resolveTypeName(const std::string& name, int line, int column)
{
    if (isBuiltInType(name)) {
        // Built-ins never have an ITypeInfo* — they are ambient primitives.
        // Return nullptr so the caller treats the name as valid (built-in)
        // without stamping a registry entry.
        return nullptr;
    }
    if (_registry) {
        auto* info = _registry->findType(name.c_str());
        if (info) return info;
    }
#if defined(AY_SCRIPT_USE_COMPILE_TIME_TYPES)
    (void)line; (void)column;
#endif
    (void)line; (void)column;
    return nullptr;
}

const ayt::reflect::ITypeInfo*
SemanticAnalyzer::resolveScriptName(const std::string& name, int line, int column)
{
    // S2.5: a script must match a registered ScriptComponent subclass.
    // Lookup goes through the same AYReflect TypeRegistry; component
    // registration happens via AY_COMPONENT(T) in each component header.
    if (_registry) {
        auto* info = _registry->findType(name.c_str());
        if (info) return info;
    }
    (void)line; (void)column;
    return nullptr;
}

SemanticResult SemanticAnalyzer::analyze(Program& program)
{
    _diagnostics.clear();
    for (auto& script : program.scripts) {
        if (script) analyzeScript(*script);
    }
    return SemanticResult{_diagnostics};
}

void SemanticAnalyzer::analyzeScript(ScriptDecl& s)
{
    _scope.clear();
    _currentSelfType = nullptr;

    // Resolve the script name to the bound host type via the AYReflect
    // TypeRegistry. S2.5 / S3.0 (LG-03): an unknown script name is a
    // soft warning, not a hard error — Logia source files are sometimes
    // written before the matching C++ host type is compiled in. The
    // runtime bridge is responsible for the actual binding check.
    //
    // S3.0 (LG-03) intentionally does NOT branch on `hostKind`: the
    // lookup path is the same single `findType(name)` regardless of
    // whether the future host is a ScriptComponent, an ISystem, etc.
    // Host-specific subclass validation is deferred to S3.1+.
    auto* selfType = resolveScriptName(s.name, 0, 0);
    if (!selfType) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.errorCode = ErrorCode::UnknownIdentifier;
        d.message = "script '" + s.name + "' has no matching registered type";
        d.hint = "register a C++ host type named '" + s.name +
                 "' via AYReflect (e.g. AY_FINALIZE_REGISTRATION_METADATA(" +
                 s.name + ")); or fix the name to match an existing registered type";
        report(d);
    }
    _currentSelfType = selfType;

    // Inject `self` into the script's scope so `self.field` resolves.
    ScopeEntry e;
    e.type = selfType;
    e.decl = nullptr;
    _scope["self"] = e;

    // First pass: collect var declarations into the script-local scope.
    for (auto& member : s.members) {
        if (!member) continue;
        if (auto* v = dynamic_cast<VarDeclStmt*>(member.get())) {
            analyzeVarDecl(*v);
        }
        // Lifecycle params (S2.5: must be empty; parser already errors)
        // and var decls are the only named introducers.
    }

    // Second pass: walk lifecycle bodies and var initializers.
    for (auto& member : s.members) {
        if (!member) continue;
        if (auto* v = dynamic_cast<VarDeclStmt*>(member.get())) {
            if (v->initializer) analyzeExpr(*v->initializer);
        } else if (auto* lf = dynamic_cast<LifecycleFuncDecl*>(member.get())) {
            analyzeLifecycle(*lf);
        } else {
            analyzeStmt(*member);
        }
    }

    _currentSelfType = nullptr;
}

void SemanticAnalyzer::analyzeVarDecl(VarDeclStmt& v)
{
    if (isBuiltInType(v.typeName)) {
        ScopeEntry e;
        e.type = nullptr;  // built-in: no ITypeInfo*
        e.decl = &v;
        _scope[v.name] = e;
        return;
    }
    auto* info = resolveTypeName(v.typeName, 0, 0);
    if (!info) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::TypeMismatch;
        d.message = "unknown type '" + v.typeName + "'";
        d.hint = "register the type with AYReflect or use a built-in (int, float, bool, string, Entity)";
        report(d);
        return;
    }
    v.resolvedType = info;
    ScopeEntry e;
    e.type = info;
    e.decl = &v;
    _scope[v.name] = e;
}

void SemanticAnalyzer::analyzeLifecycle(LifecycleFuncDecl& fn)
{
    // S2.5: lifecycle functions take no parameters. The parser silently
    // drops a non-empty parameter list (forgiving old S1 code), so we
    // report it here as a soft warning. The S2.5 codegen also ignores
    // the params field — `self` is always the only argument emitted.
    if (!fn.params.empty()) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.errorCode = ErrorCode::InvalidStatement;
        d.message = "lifecycle functions take no parameters in S2.5";
        d.hint = "use 'self' for the receiver; read entity context via World::instance()";
        report(d);
    }
    for (auto& s : fn.body) {
        if (s) analyzeStmt(*s);
    }
}

void SemanticAnalyzer::analyzeStmt(Stmt& s)
{
    if (auto* es = dynamic_cast<ExprStmt*>(&s)) {
        if (es->expr) analyzeExpr(*es->expr);
    } else if (auto* is = dynamic_cast<IfStmt*>(&s)) {
        if (is->condition) analyzeExpr(*is->condition);
        for (auto& t : is->thenBranch) if (t) analyzeStmt(*t);
        for (auto& e : is->elseBranch) if (e) analyzeStmt(*e);
    } else if (auto* rs = dynamic_cast<ReturnStmt*>(&s)) {
        if (rs->value) analyzeExpr(*rs->value);
    } else if (auto* vd = dynamic_cast<VarDeclStmt*>(&s)) {
        analyzeVarDecl(*vd);
        if (vd->initializer) analyzeExpr(*vd->initializer);
    } else if (auto* lf = dynamic_cast<LifecycleFuncDecl*>(&s)) {
        analyzeLifecycle(*lf);
    }
}

void SemanticAnalyzer::analyzeExpr(Expr& e)
{
    if (auto* b = dynamic_cast<BinaryExpr*>(&e)) {
        // Assignment (`=`) target is allowed to be an undeclared
        // identifier — codegen lowers it to a Lua assignment that
        // implicitly creates a global. This matches the S1 codegen
        // tests that use bare assignments to `x` / `speed` / etc.
        if (b->op.type == TokenType::Equal && b->left) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(b->left.get())) {
                auto it = _scope.find(id->name);
                if (it != _scope.end()) {
                    id->resolvedType = it->second.type;
                    id->resolvedDecl = it->second.decl;
                }
                // Undeclared assignment target — silently allowed
                // (Lua-style implicit global).
            } else {
                analyzeExpr(*b->left);
            }
        } else if (b->left) {
            analyzeExpr(*b->left);
        }
        if (b->right) analyzeExpr(*b->right);
    } else if (auto* u = dynamic_cast<UnaryExpr*>(&e)) {
        if (u->operand) analyzeExpr(*u->operand);
    } else if (auto* c = dynamic_cast<CallExpr*>(&e)) {
        // S2.5: no more `get_component` magic. Calls go through the
        // generic identifier-resolution path.
        if (c->callee) analyzeExpr(*c->callee);
        for (auto& a : c->args) if (a) analyzeExpr(*a);
    } else if (auto* id = dynamic_cast<IdentifierExpr*>(&e)) {
        analyzeIdentifierExpr(*id);
    } else if (auto* m = dynamic_cast<MemberExpr*>(&e)) {
        if (m->object) analyzeExpr(*m->object);
        const ayt::reflect::ITypeInfo* parent = m->object ? m->object->resolvedType : nullptr;
        analyzeMemberExpr(*m, parent);
    } else if (auto* i = dynamic_cast<IndexExpr*>(&e)) {
        if (i->object) analyzeExpr(*i->object);
        if (i->index)  analyzeExpr(*i->index);
    } else if (auto* lit = dynamic_cast<LiteralExpr*>(&e)) {
        // No type stamping for primitives in S2.5 (codegen doesn't read it).
        (void)lit;
    }
}

void SemanticAnalyzer::analyzeIdentifierExpr(IdentifierExpr& id)
{
    auto it = _scope.find(id.name);
    if (it != _scope.end()) {
        id.resolvedType = it->second.type;
        id.resolvedDecl = it->second.decl;
        return;
    }
    if (isAmbientIdentifier(id.name)) {
        return;  // ambient — no ITypeInfo, allowed
    }
    // Undeclared identifier read. Lua-style implicit global.
    LogiaDiagnostic d;
    d.severity = DiagnosticSeverity::Warning;
    d.errorCode = ErrorCode::UnknownIdentifier;
    d.message = "implicit global '" + id.name + "' (not declared in script)";
    d.hint = "declare it with `var " + id.name + ": <Type>`, or pass it as a parameter";
    report(d);
}

void SemanticAnalyzer::analyzeMemberExpr(MemberExpr& m,
                                         const ayt::reflect::ITypeInfo* parent)
{
    if (!parent) {
        // Parent type unresolved (ambient or unknown). Don't warn here —
        // the caller already reported an issue or the chain root is
        // ambient (input.is_pressed → ok).
        return;
    }
    auto* field = parent->findField(m.member.c_str());
    if (!field) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.errorCode = ErrorCode::UnknownIdentifier;
        d.message = "type '" + std::string(parent->getName()) +
                    "' has no field '" + m.member + "'";
        report(d);
        return;
    }
    m.resolvedField = field;
    m.resolvedType = field->getType();
}

} // namespace ayt::script::logia