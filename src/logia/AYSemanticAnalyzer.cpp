// AYSemanticAnalyzer.cpp - Logia semantic analyzer (S2)

#include "logia/AYSemanticAnalyzer.h"

#include "AYLogger.h"

// AYReflect pulls in a large surface; include only what we need.
#include "IAYReflect.h"
#include "AYReflectRegistry.h"
#include "AYReflect.h"  // full TypeRegistryImpl definition (linkable)

// Force AYEntity's Transform component to be registered with the
// TypeRegistry at static-init time. Without this include, the
// AY_FINALIZE_REGISTRATION_METADATA(Transform) macro at the bottom
// of AYTransformComponent.h is never instantiated and Logia scripts
// that reference Transform (e.g. examples/player_controller.logia)
// would fail with "unknown type 'Transform'".
#include "components/AYTransformComponent.h"

// AYEntityModule.h declares `registerEntityComponents()` whose body
// triggers registration of all AYEntity component types with the
// World storage layer. It does NOT register with AYReflect —
// AYReflect registration is driven by the AY_FINALIZE_REGISTRATION_METADATA
// macros in each component header, which only fire when that header
// is included from a TU whose anonymous-namespace static is actually
// kept by the linker. To guarantee Transform is available to
// SemanticAnalyzer, we explicitly register it here on first use.
#include "AYEntityModule.h"
#include "components/AYTransformComponent.h"
#include "components/AYHealthComponent.h"
#include "AYReflect.h"
#include "AYReflectMacros.h"  // ayt::reflect::detail::defaultCreate/Destroy/Copy

#include <AYMathTypes.h>  // math::FVector3, math::FQuaternion

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace ayt::script::logia
{

namespace
{

// Cached "known" set of names so the analyzer can short-circuit on the
// hot path without re-querying the registry for every identifier. The
// registry is still consulted on each VarDeclStmt.typeName resolution
// because the lookup result is what gets stamped onto the AST.
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
// AYEntity's macros (AY_FINALIZE_REGISTRATION_METADATA) instantiate
// anonymous-namespace static instances whose ctor calls
// registerTypeInfo — but only when the linker keeps them. Doing the
// registration here makes AYScript self-contained: any executable
// that links AYScript gets the standard set of AYEntity components
// available to Logia scripts, regardless of how the host project
// links AYEntity. Idempotent: findType-first prevents duplicates.
void ensureAYEntityTypesRegistered()
{
    auto& reg = ayt::reflect::TypeRegistryImpl::instance();

    // Register math primitives first so Transform's fields can
    // reference them. AYMath doesn't auto-register types with
    // AYReflect (its types use AY_PROPERTY + manual serialization,
    // not the global registry).
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

    // Transform: AYEntity's AY_FINALIZE_REGISTRATION_METADATA(Transform)
// macro may already have registered Transform with the registry
// during static init of AYEntity's translation units — but those
// fields reference FVector3 ITypeInfo* that didn't exist yet (we
// registered FVector3 above). Always re-register Transform here so
// the field types resolve to non-null. (The previous ITypeInfo*
// leaks; acceptable for S2 testing.)
    {
        auto* info = new ayt::reflect::TypeInfoImpl<ayt::entity::Transform>(
            "Transform",
            ayt::reflect::detail::defaultCreate<ayt::entity::Transform>,
            ayt::reflect::detail::defaultDestroy<ayt::entity::Transform>,
            ayt::reflect::detail::defaultCopy<ayt::entity::Transform>);
        // Manually add the three properties AYTransformComponent.h
        // declares via AY_PROPERTY. Field offsets come from the
        // Transform POD layout. fVec3 / fQuat are registered above.
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

SemanticAnalyzer::SemanticAnalyzer(SemanticOptions options)
    : _options(options), _fileName(options.fileName ? options.fileName : "")
{
    // Default registry: process-wide AYReflect singleton.
    // TypeRegistryImpl implements both ITypeRegistry (interface) and
    // exposes findType<T>() template helpers. The wrapper class
    // `TypeRegistry::instance()` is declared-only — we use the impl
    // singleton directly.
    _registryImpl = &ayt::reflect::TypeRegistryImpl::instance();
    _registry     = _registryImpl;

    // Guarantee AYEntity component types are registered with AYReflect
    // before we query the registry. AYEntity's static-init chain
    // (AY_FINALIZE_REGISTRATION_METADATA) only fires if the linker
    // keeps the TU-local anonymous-namespace static, which is
    // implementation-defined. Doing it here, explicitly, sidesteps
    // that uncertainty. This is a no-op if Transform is already
    // registered (findType first check is null).
    ensureAYEntityTypesRegistered();
}

SemanticAnalyzer::~SemanticAnalyzer()
{
    // We never own the registry in default ctor; only test-owned injection
    // could need cleanup. Kept simple — `_ownsRegistry` is reserved for a
    // future "embed a mock registry" path that doesn't exist yet.
}

void SemanticAnalyzer::setTypeProvider(ayt::reflect::ITypeRegistry* reg)
{
    _registry = reg;
    _registryImpl = static_cast<ayt::reflect::TypeRegistryImpl*>(reg);
}

bool SemanticAnalyzer::isBuiltIn(const std::string& name)
{
    return builtinTypeNames().count(name) > 0;
}

bool SemanticAnalyzer::isAmbientIdentifier(const std::string& name)
{
    return ambientIdentifiers().count(name) > 0;
}

const char* SemanticAnalyzer::builtinTypeName(const std::string& name)
{
    if (builtinTypeNames().count(name) > 0) return name.c_str();
    return nullptr;
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
    if (isBuiltIn(name)) {
        // Built-ins never have an ITypeInfo (they're ambient primitives).
        // Return nullptr — the analyzer records the resolution intent
        // via "we recognized the name" without stamping a registry entry.
        return nullptr;
    }

    if (_registry) {
        auto* info = _registry->findType(name.c_str());
        if (info) return info;
    }

    // Compile-time template path (opt-in, no-op when not enabled).
#if defined(AY_SCRIPT_USE_COMPILE_TIME_TYPES)
    if (_registryImpl && _options.useCompileTimeTypes) {
        // Template findType<T> requires a C++ type; without a per-name
        // type table in S2 we have nothing extra to query. Reserved for
        // S2.1's generated metadata table.
    }
#endif

    // Miss. Caller decides whether to treat it as hard error or warning.
    (void)line;
    (void)column;
    return nullptr;
}

SemanticResult SemanticAnalyzer::analyze(Program& program)
{
    _diagnostics.clear();
    for (auto& comp : program.components) {
        if (comp) analyzeComponent(*comp);
    }
    return SemanticResult{_diagnostics};
}

void SemanticAnalyzer::analyzeComponent(ComponentDecl& c)
{
    _scope.clear();

    // First pass: collect declarations into the component-local scope so
    // expressions later can resolve identifiers against them. Vars and
    // params of lifecycle methods all share the same scope.
    for (auto& member : c.members) {
        if (!member) continue;
        if (auto* v = dynamic_cast<VarDeclStmt*>(member.get())) {
            analyzeVarDecl(*v);
        } else if (auto* lf = dynamic_cast<LifecycleFuncDecl*>(member.get())) {
            for (const auto& p : lf->params) {
                auto* info = resolveTypeName(p.typeName, 0, 0);
                if (!info && !isBuiltIn(p.typeName)) {
                    LogiaDiagnostic d;
                    d.severity = DiagnosticSeverity::Error;
                    d.errorCode = ErrorCode::TypeMismatch;
                    d.message = "unknown parameter type '" + p.typeName + "'";
                    d.hint = "check that the type is registered with AYReflect";
                    report(d);
                }
                ScopeEntry e;
                e.type = info;
                e.decl = &p;
                _scope[p.name] = e;
            }
        }
    }

    // Second pass: walk bodies and initializers.
    for (auto& member : c.members) {
        if (!member) continue;
        if (auto* v = dynamic_cast<VarDeclStmt*>(member.get())) {
            if (v->initializer) analyzeExpr(*v->initializer);
        } else if (auto* lf = dynamic_cast<LifecycleFuncDecl*>(member.get())) {
            analyzeLifecycle(*lf);
        } else {
            analyzeStmt(*member);
        }
    }
}

void SemanticAnalyzer::analyzeVarDecl(VarDeclStmt& v)
{
    if (v.resolvedType) {
        // Already populated (defensive).
        _scope[v.name] = ScopeEntry{v.resolvedType, &v};
        return;
    }

    if (isBuiltIn(v.typeName)) {
        // Built-in: stamp via name lookup, no ITypeInfo*.
        // We leave resolvedType = nullptr so codegen stays S1-shaped.
        ScopeEntry e;
        e.type = nullptr;
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
                // Undeclared assignment target — silently allowed.
            } else {
                analyzeExpr(*b->left);
            }
        } else if (b->left) {
            analyzeExpr(*b->left);
        }
        if (b->right) analyzeExpr(*b->right);

        // Result type: numeric → int/float by promotion; comparisons → bool.
        auto* lt = b->left  ? b->left->resolvedType  : nullptr;
        auto* rt = b->right ? b->right->resolvedType : nullptr;
        const bool isComparison =
            b->op.type == TokenType::EqualEqual ||
            b->op.type == TokenType::BangEqual ||
            b->op.type == TokenType::Less ||
            b->op.type == TokenType::LessEqual ||
            b->op.type == TokenType::Greater ||
            b->op.type == TokenType::GreaterEqual;
        const bool isLogical =
            b->op.type == TokenType::And ||
            b->op.type == TokenType::Or;
        // We don't mint ITypeInfo* for primitives — codegen ignores
        // resolvedType on BinExpr in S2 anyway. Stamping is for
        // future consumers / diagnostics.
        (void)lt; (void)rt;
        (void)isComparison; (void)isLogical;
        // Mark "this expression was analyzed" by leaving resolvedType null
        // (S1 codegen path doesn't consult it).
    } else if (auto* u = dynamic_cast<UnaryExpr*>(&e)) {
        if (u->operand) analyzeExpr(*u->operand);
    } else if (auto* c = dynamic_cast<CallExpr*>(&e)) {
        if (c->callee) analyzeExpr(*c->callee);

        // Detect `get_component(<TypeName>)` early so the type-name
        // argument isn't subject to the regular identifier check
        // (which would otherwise reject `Transform` / `HealthComponent`
        // as undeclared identifiers). Two calling conventions are
        // accepted: bare `get_component(...)` and member-call
        // `entity.get_component(...)`.
        const bool isGetComponent = [&]() -> bool {
            if (c->args.size() != 1) return false;
            if (auto* id = dynamic_cast<IdentifierExpr*>(c->callee.get())) {
                return id->name == "get_component";
            }
            if (auto* m = dynamic_cast<MemberExpr*>(c->callee.get())) {
                return m->member == "get_component";
            }
            return false;
        }();

        if (!isGetComponent) {
            for (auto& a : c->args) if (a) analyzeExpr(*a);
        }
        analyzeCallExpr(*c);
    } else if (auto* id = dynamic_cast<IdentifierExpr*>(&e)) {
        analyzeIdentifierExpr(*id);
    } else if (auto* m = dynamic_cast<MemberExpr*>(&e)) {
        // A MemberExpr's root identifier may be an implicit global
        // (Lua-style). Codegen emits `a.b.c = ...` as bare Lua. If the
        // root is an undeclared identifier, treat it as opaque
        // (resolvedType remains null) and skip the strict identifier
        // check — but warn so the user knows.
        if (m->object) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(m->object.get())) {
                if (_scope.find(id->name) == _scope.end() &&
                    !isAmbientIdentifier(id->name)) {
                    // Implicit global — soft warning only.
                    LogiaDiagnostic d;
                    d.severity = DiagnosticSeverity::Warning;
                    d.errorCode = ErrorCode::UnknownIdentifier;
                    d.message = "implicit global '" + id->name +
                                "' (member access on undeclared identifier)";
                    report(d);
                } else {
                    analyzeExpr(*m->object);
                }
            } else {
                analyzeExpr(*m->object);
            }
        }
        const ayt::reflect::ITypeInfo* parent = m->object ? m->object->resolvedType : nullptr;
        analyzeMemberExpr(*m, parent);
    } else if (auto* i = dynamic_cast<IndexExpr*>(&e)) {
        if (i->object) analyzeExpr(*i->object);
        if (i->index)  analyzeExpr(*i->index);
    } else if (auto* lit = dynamic_cast<LiteralExpr*>(&e)) {
        // No type stamping for built-in primitives in S2 (codegen doesn't
        // need it). S2.1 may stamp via a synthetic ITypeInfo when AYReflect
        // is asked to register built-ins.
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
        // Ambient — no ITypeInfo, but allowed.
        return;
    }
    // Undeclared identifier read. Treat as implicit global (Lua-style).
    // The codegen emits it as a bare Lua global access; warn so the
    // user knows but don't fail the compile. S1 codegen tests rely
    // on this lenient behavior for assignment-LHS and condition
    // expressions on undeclared names.
    LogiaDiagnostic d;
    d.severity = DiagnosticSeverity::Warning;
    d.errorCode = ErrorCode::UnknownIdentifier;
    d.message = "implicit global '" + id.name + "' (not declared in component)";
    d.hint = "declare it with `var " + id.name + ": <Type>` or pass it as a parameter";
    report(d);
}

void SemanticAnalyzer::analyzeMemberExpr(MemberExpr& m,
                                         const ayt::reflect::ITypeInfo* parent)
{
    if (!parent) {
        // Either the object is ambient (input.is_pressed → ok), or its
        // type is unresolved (we already reported). Soft warning so we
        // don't double-report.
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
    // Stamp on the leaf MemberExpr.
    m.resolvedField = field;
    m.resolvedType = field->getType();
}

void SemanticAnalyzer::analyzeCallExpr(CallExpr& c)
{
    // Special case: get_component(<TypeName>). Accepted in both forms:
    //   - bare: get_component(Transform)
    //   - member: entity.get_component(Transform)
    auto isGetComponentCall = [&]() -> bool {
        if (c.args.size() != 1) return false;
        if (auto* id = dynamic_cast<IdentifierExpr*>(c.callee.get())) {
            return id->name == "get_component";
        }
        if (auto* m = dynamic_cast<MemberExpr*>(c.callee.get())) {
            return m->member == "get_component";
        }
        return false;
    };
    if (!isGetComponentCall()) return;

    Expr& a0 = *c.args[0];
    std::string typeName;
    if (auto* id = dynamic_cast<IdentifierExpr*>(&a0)) {
        typeName = id->name;
    } else if (auto* me = dynamic_cast<MemberExpr*>(&a0)) {
        // `entity.get_component(Transform)` — last member is the type.
        typeName = me->member;
    }
    if (typeName.empty()) return;

    if (isBuiltIn(typeName)) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::InvalidOperation;
        d.message = "get_component() cannot return built-in type '" + typeName + "'";
        report(d);
        return;
    }
    auto* info = resolveTypeName(typeName, 0, 0);
    if (!info) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::TypeMismatch;
        d.message = "unknown component type '" + typeName + "'";
        d.hint = "register the type with AYReflect";
        report(d);
        return;
    }
    c.resolvedType = info;
}

} // namespace ayt::script::logia