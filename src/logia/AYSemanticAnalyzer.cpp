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

#include "aylog/Logger.h"

// AYReflect surface
#include "ayreflect/IReflect.h"
#include "ayreflect/ReflectRegistry.h"
#include "AYReflect.h"  // full TypeRegistryImpl definition (linkable)

// S3.2 (LG-04b, B-min): isDerivedFrom is a free function defined in
// AYReflect.cpp. Forward-declare to keep the include surface small.
namespace ayt::reflect
{
bool isDerivedFrom(const ITypeInfo* type, const ITypeInfo* base);
} // namespace ayt::reflect

// Force AYEntity's Transform component to be registered with AYReflect.
#include "components/AYTransformComponent.h"
#include "AYEntityModule.h"
#include "components/AYHealthComponent.h"
#include "AYReflectMacros.h"  // ayt::reflect::detail::defaultCreate/Destroy/Copy
#include <aymath/MathTypes.h>

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

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

// R5.2-C (2026-07-14): name → true if `name` is an integer-shape
// primitive. Distinct from `isBuiltInType()` (which only answers
// "is the name recognized as ambient"), because some builtins
// (`float`, `bool`, `string`) are not integer-shape. `int` /
// `Int32` / `Int64` are. Logia-script semantics: only
// integer-shape types are valid `for (...)` bounds.
//
// We deliberately exclude PascalCase float aliases (`Float32` /
// `Float64`) — those are real types and a `for (var i : x)`
// where x is `Float32` is a type error.
//
// Used by both the bound validator and (optionally) future
// expression-shape inference. Pure name match — does not touch
// AYReflect.
bool isIntegerTypeName(const std::string& name)
{
    return name == "int"
        || name == "Int32"
        || name == "Int64";
}

// R5.2-C (2026-07-14): pure-tree-shape predicate. Returns true
// iff `e` is statically an int given current analyzer state
// (resolvedType / literal value / constant-folded binary form).
//
// Cases handled (in priority order):
//   - IntLiteralExpr                     → true
//   - FloatLiteralExpr / StringLiteral   → false
//   - BoolLiteralExpr                    → false
//   - IdentifierExpr with resolvedType
//     pointing at "int"/"Int32"/"Int64"  → true
//   - MemberExpr leaf with resolvedType  → check ITypeInfo*'s name
//   - CallExpr with resolvedMethod whose
//     getReturnType() is int-shaped       → true
//   - BinaryExpr where all leaves
//     (recursive) are int                → true (constant-folded)
//   - UnaryExpr where operand (recursive) is int → true
//   - anything else (no resolvedType, IndexExpr, TableExpr, etc.)
//     → false (default-reject per R5.2-C policy)
//
// Free function (not a method): SemanticAnalyzer keeps
// resolvedType stamped on each Expr, so we only need read access.
bool boundIsStaticallyInt(const Expr* e)
{
    if (!e) return false;

    if (auto* lit = dynamic_cast<const LiteralExpr*>(e)) {
        return std::holds_alternative<int>(lit->value);
    }

    if (auto* id = dynamic_cast<const IdentifierExpr*>(e)) {
        // R5.2-C (2026-07-14): for script-declared `var n: int`
        // the scope entry stores type=nullptr (built-in types
        // never have an ITypeInfo*) — but the VarDeclStmt's
        // `typeName` field still carries the user's annotation.
        // resolveDecl points at the VarDeclStmt; reading typeName
        // there is the cleanest way to recover the int shape
        // without disturbing the analyzer's scope semantics.
        if (id->resolvedDecl) {
            if (auto* vd = static_cast<const VarDeclStmt*>(
                    id->resolvedDecl)) {
                if (isIntegerTypeName(vd->typeName)) {
                    return true;
                }
            }
        }
        if (!id->resolvedType) return false;
        return isIntegerTypeName(id->resolvedType->getName());
    }

    if (auto* m = dynamic_cast<const MemberExpr*>(e)) {
        if (!m->resolvedType) return false;
        return isIntegerTypeName(m->resolvedType->getName());
    }

    if (auto* c = dynamic_cast<const CallExpr*>(e)) {
        if (!c->resolvedMethod) return false;
        auto* rt = c->resolvedMethod->getReturnType();
        if (!rt) return false;
        return isIntegerTypeName(rt->getName());
    }

    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        // Constant-folded shape check. Operator-agnostic — `+ - * / % ^`
        // all produce int when both operands are int literals.
        return boundIsStaticallyInt(b->left.get())
            && boundIsStaticallyInt(b->right.get());
    }

    if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
        return boundIsStaticallyInt(u->operand.get());
    }

    return false;
}

// R5.2-C (2026-07-14): best-effort source location lookup for
// expressions. The Expr base class does not currently carry a
// source location (S2.5 parser doesn't stamp it). For R5.2-C
// diagnostics we report at empty location — the
// `LogiaDiagnostic::toHumanString` already handles missing
// line/column gracefully. Future slice can add Expr-level source
// locations if finer-grained diagnostics are ever needed.
SourceLocation sourceLocFor(Expr* /*bound*/) { return {}; }

const std::unordered_set<std::string>& ambientIdentifiers()
{
    static const std::unordered_set<std::string> s = {
        "input", "log", "time"
    };
    return s;
}

// S3.10: register primitive types in AYReflect so `resolveTypeName`
// stamps a non-null ITypeInfo on primitive field accesses
// (`self.moveSpeed` where moveSpeed is `AY_PROPERTY(float, ...)`).
// Without this, the analyzer's `analyzeMemberExpr` leaves
// `m->resolvedType` as nullptr and the codegen rewrite to
// `ayt_reflect_set_field(self, ..., ...)` is skipped (the helper
// gates on `resolvedType->getFieldCount() == 0`, which would crash
// on null). Idempotent — safe to call from every analyzer ctor.
template <typename T>
void registerPrimitive(const char* name)
{
    auto& reg = ayt::reflect::TypeRegistryImpl::instance();
    if (reg.findType(name) != nullptr) return;
    auto* info = new ayt::reflect::TypeInfoImpl<T>(
        name,
        ayt::reflect::detail::defaultCreate<T>,
        ayt::reflect::detail::defaultDestroy<T>,
        ayt::reflect::detail::defaultCopy<T>);
    reg.registerTypeInfo(name, info);
}

void ensurePrimitiveTypesRegistered()
{
    registerPrimitive<int32_t>("int");
    registerPrimitive<int32_t>("Int32");
    registerPrimitive<int64_t>("Int64");
    registerPrimitive<float>("float");
    registerPrimitive<float>("Float32");
    registerPrimitive<double>("double");
    registerPrimitive<double>("Float64");
    registerPrimitive<bool>("bool");
    registerPrimitive<bool>("Bool");
}

// S3.10: static-init guard so consumers that depend on AYReflect
// builtins (e.g. the System-host test fixture's MovementSystemRegistrar
// in Test_LogiaSystemHost.cpp, which calls `reg.findType("float")` at
// static init time) see a populated registry regardless of TU init
// order. The guard runs once on first access and is MT-safe by C++11
// static-init rules.
namespace {
struct PrimitiveBootstrap {
    PrimitiveBootstrap() { ensurePrimitiveTypesRegistered(); }
};
static PrimitiveBootstrap g_primitiveBootstrap;
} // namespace

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
    // S3.11: register FVector3's primitive fields so the analyzer's
    // `analyzeMemberExpr` stamps `resolvedField` + `resolvedType=Float32`
    // on `self.position.x` leaves. Without this, the chain probe in
    // codegen never fires (leaf has null resolvedType) and the
    // legacy `__tmp_lhs` path is emitted — runtime no-op on the
    // lightuserdata receiver. Idempotent: re-entry guards on
    // `getFieldCount() == 0` so a partial prior registration is
    // completed on the next analyzer ctor. Field type is registered
    // as "Float32" to match `pushFieldPrimitive`'s dispatch (which
    // accepts both "float" and "Float32"). FQuaternion fields are
    // out of S3.11 scope (added by §5.7.4 track R2).
    if (fVec3 && fVec3->getFieldCount() == 0) {
        auto* floatInfo = reg.findType("float");
        if (floatInfo) {
            fVec3->addField(new ayt::reflect::FieldInfoImpl(
                "x", floatInfo,
                offsetof(ayt::math::FVector3, x),
                ayt::reflect::FieldAttribute::Serialize));
            fVec3->addField(new ayt::reflect::FieldInfoImpl(
                "y", floatInfo,
                offsetof(ayt::math::FVector3, y),
                ayt::reflect::FieldAttribute::Serialize));
            fVec3->addField(new ayt::reflect::FieldInfoImpl(
                "z", floatInfo,
                offsetof(ayt::math::FVector3, z),
                ayt::reflect::FieldAttribute::Serialize));
        }
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
    // S3.10: also ensure the primitive types (int / float / bool /
    // double / int64 / their PascalCase aliases) are registered so
    // the analyzer's `resolveTypeName` can stamp `resolvedType` on
    // `self.<primitiveField>` MemberExpr leaves. Without this, the
    // codegen rewrite `self.field = X → ayt_reflect_set_field(...)`
    // is skipped (the helper guards on resolvedType->getFieldCount()
    // == 0, which can't be evaluated on null). Idempotent.
    ensurePrimitiveTypesRegistered();
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
    // S3.1 (LG-04): the same `findType(name)` lookup is used for all
    // host kinds. No subclass check (per LG-04b / S3.2 deferral). The
    // host-kind-specific hint text below tells the user which C++ side
    // binding to add (AY_SYSTEM for ECS systems, ScriptComponent for
    // entity scripts).
    auto* selfType = resolveScriptName(s.name, 0, 0);
    if (!selfType) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.errorCode = ErrorCode::UnknownIdentifier;
        d.message = "script '" + s.name + "' has no matching registered type";
        std::string hint;
        if (_ctx.kind == LogiaHostKind::System) {
            hint = "register an ISystem subclass named '" + s.name +
                   "' with AY_SYSTEM(" + s.name + ", priority) so World " +
                   "ticks it; or fix the name to match an existing ISystem";
        } else {
            hint = "register a C++ host type named '" + s.name +
                   "' via AYReflect (e.g. AY_FINALIZE_REGISTRATION_METADATA(" +
                   s.name + ")); or fix the name to match an existing registered type";
        }
        d.hint = hint;
        report(d);
    }
    // S3.2 (LG-04b, B-min): Component host strict inheritance. Only
    // applies when the caller asked for it via `ctx.strictInheritance`
    // AND supplied a non-null `ctx.hostType` AND the host kind is
    // Component. We hard-reject scripts that are registered in
    // AYReflect but NOT derived from `hostType` (per
    // ayt::reflect::isDerivedFrom single-chain walk). Scripts not
    // registered at all still emit the S2.5 / LG-03 soft warning
    // above and are not covered by this check (the caller is
    // responsible for catching genuinely missing types before
    // enabling strict mode).
    if (_ctx.kind == LogiaHostKind::Component
        && _ctx.strictInheritance
        && _ctx.hostType != nullptr
        && selfType != nullptr) {
        if (!ayt::reflect::isDerivedFrom(selfType, _ctx.hostType)) {
            LogiaDiagnostic d;
            d.severity = DiagnosticSeverity::Error;
            d.errorCode = ErrorCode::TypeMismatch;
            d.message = "script '" + s.name + "' must derive from '" +
                        std::string(_ctx.hostType->getName()) +
                        "' (strict Component host binding)";
            d.hint = "either add an AY_INHERITS(" + s.name + ", " +
                     std::string(_ctx.hostType->getName()) +
                     ") declaration, or remove `strictInheritance` from "
                     "the LogiaHostContext";
            report(d);
        }
    }

    _currentSelfType = selfType;

    // LG-05 / S3.3: stamp the host type name on the ScriptDecl so
    // LuaCodegen can emit `ayt_reflect_*_field(self, "<name>", "<f>")`
    // for `self.field` reads/writes. Only set when the script name
    // resolved in AYReflect — otherwise codegen keeps the S2.5 bare
    // `self.<field>` form (a no-op at runtime) so unknown hosts do
    // not crash the reflect path.
    if (selfType) {
        s.hostTypeName = std::string(selfType->getName());
    }

    // S3.8b: Tool hosts (and any other host that sets `expectSelf`
    // = false) do not bind a receiver to the script — ToolRunner
    // calls `run()` with `receiver = nullptr`, so injecting `self`
    // into the scope would be misleading (every read would be a
    // soft warning, every write would target a nil lightuserdata).
    // Skip the scope injection; a `run()` body that mentions `self`
    // will hit the implicit-global path and produce the standard
    // soft warning, which is the right surface to the user.
    if (_ctx.expectSelf) {
        ScopeEntry e;
        e.type = selfType;
        e.decl = nullptr;
        _scope["self"] = e;
    }

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

    // S3.1 (LG-04): the System host (ctx.kind == System) defines its
    // own lifecycle method set. `on_start` is one-shot at world tick
    // start; `on_update` is the per-tick hook. `on_destroy` is not
    // meaningful for an ISystem — emit a soft warning if a System-
    // bound script declares it. Component host keeps the full S2.5
    // set (on_start / on_update / on_destroy) unchanged.
    if (_ctx.kind == LogiaHostKind::System
        && fn.kind == LifecycleKind::OnDestroy) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.errorCode = ErrorCode::InvalidStatement;
        d.message = "on_destroy is not invoked on System host scripts";
        d.hint = "ISystem instances are owned by World for the whole "
                 "process lifetime; use on_start (one-shot) or on_update "
                 "(per-tick) instead";
        report(d);
    }

    // S3.8b (LG-07) — Tool host policy. A Tool script is a run-only
    // one-shot (editor / CLI) whose entry point is the new `run()`
    // lifecycle. The legacy `on_start` / `on_update` / `on_destroy`
    // methods are never invoked by the ToolRunner — emit soft
    // warnings so refactors from Component / System hosts keep
    // parsing. The shape mirrors the S3.1 System-on_destroy policy.
    if (_ctx.kind == LogiaHostKind::Tool) {
        const char* name = nullptr;
        if (fn.kind == LifecycleKind::OnStart) {
            name = "on_start";
        } else if (fn.kind == LifecycleKind::OnUpdate) {
            name = "on_update";
        } else if (fn.kind == LifecycleKind::OnDestroy) {
            name = "on_destroy";
        }
        if (name) {
            LogiaDiagnostic d;
            d.severity = DiagnosticSeverity::Warning;
            d.errorCode = ErrorCode::InvalidStatement;
            d.message = std::string(name) +
                        " is not invoked on Tool host scripts";
            d.hint = "Tool host runs once via the run() entry point; "
                     "use `run()` for the one-shot body, or move this "
                     "script to a Component / System host if you need "
                     "tick-driven lifecycle";
            report(d);
        }
    }

    // S3.8b (LG-07) — symmetric warning: `run()` is meaningful only
    // under the Tool host. On Component / System hosts it would be a
    // dead method (neither the dispatcher nor the runner ever calls
    // it). Soft warn so a copy-paste from a Tool source still parses
    // under a Component host but the user knows to drop it.
    if (_ctx.kind != LogiaHostKind::Tool
        && fn.kind == LifecycleKind::Run) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.errorCode = ErrorCode::InvalidStatement;
        d.message = "run() is a Tool host lifecycle and is not invoked on "
                    + std::string(_ctx.kind == LogiaHostKind::Component
                                      ? "Component"
                                      : (_ctx.kind == LogiaHostKind::System
                                             ? "System"
                                             : "EventHandler"))
                    + " host scripts";
        d.hint = "move this script to a Tool host (toolLogiaHostContext()) "
                 "and bind it via runTool(), or replace `run()` with the "
                 "appropriate lifecycle (on_start / on_update) for this "
                 "host kind";
        report(d);
    }

    // R5.2-B (2026-07-14): push a function-body label-scope frame.
    // The frame is kept empty of labels by the rule that `::L::`
    // only lives in loop bodies, but pushed for stack consistency
    // so that the "label outside loop" check has a stable parent.
    _labelStack.emplace_back();
    for (auto& s : fn.body) {
        if (s) analyzeStmt(*s);
    }
    _labelStack.pop_back();
}

// R5.0 (2026-07-13): walk a while loop's condition and body.
//
// We do NOT push a new scope onto `_scope` — Lua's `while cond do ...
// end` does not introduce a lexical scope, so the body is in the same
// scope as the enclosing block. Variables declared in the body
// (`var x: int = ...`) live for the iteration of the loop only at the
// Logia-source level (the user's reading); each iteration sees the
// same `local x` re-declaration in the emitted Lua, which Lua tolerates
// as long as it's at the same block scope (which it is, since
// `parseBlockBody` re-enters here).
void SemanticAnalyzer::analyzeWhileStmt(WhileStmt& w)
{
    if (w.condition) analyzeExpr(*w.condition);
    // R5.2-B (2026-07-14): push a label-scope frame for the
    // while-body. Labels declared here are visible to `break :L`
    // / `continue :L` inside this body and to nested-loop bodies
    // (the inner frames have the outer in their `_labelStack`
    // ancestry). Popped on exit.
    _labelStack.emplace_back();
    for (auto& s : w.body) {
        if (s) analyzeStmt(*s);
    }
    _labelStack.pop_back();
}

// R5.0 (2026-07-13): walk a for loop's bound and body.
//
// The counter variable (e.g. `i` in `for (var i : 10)`) is deliberately
// NOT added to `_scope` here. Lua's `for i = 1, N do ... end` already
// makes `i` loop-local at the Lua level. If we added `i` to the
// analyzer's `_scope`, then:
//   1. Reads of `i` inside the body would resolve as if `i` were a
//      Logia-script-level var (correct behavior, but only by accident).
//   2. Reads/writes to `i` AFTER the loop body would also resolve to
//      the analyzer's view of the counter, but at runtime the counter
//      is gone (Lua's `for` is closed-over). This mismatch would
//      produce silent type errors in future R5.0.1 diagnostic passes.
//
// Skipping the scope injection keeps the analyzer's view aligned with
// runtime semantics: `i` is loop-local inside the body, and post-loop
// references to `i` are implicit globals (same as any other
// undeclared-identifier write in Logia S1).
//
// Bound type-check is deferred to R5.0.1: ideally we'd verify the
// bound expression reduces to `int` and emit a soft warning otherwise.
// S2.5 / LG-05 only stamps types on `self.<field>` leaf reads via
// AYReflect; adding general expression-type inference is out of scope.
//
// R5.0.1 (2026-07-13): also walk the optional `start` expression in
// the half-open range form `for (var i : start, end)` — analyzer
// needs to validate the start expression the same way it validates
// the bound.
//
// R5.2-C (2026-07-14): the bound type-check is no longer deferred.
// validateForBound() emits `ErrorCode::TypeMismatch` hard error when
// the bound cannot be statically reduced to int. Decision matrix
// (allowed vs rejected shapes) lives on validateForBound's comment
// block. `validateForBound` also internally walks the bound subtree
// (calling analyzeExpr-like logic on each leaf) so we no longer
// need the prior `analyzeExpr(*f.bound)` call.
void SemanticAnalyzer::analyzeForStmt(ForStmt& f)
{
    // R5.2-C (2026-07-14): validate the upper bound expression.
    // validateForBound internally calls analyzeExpr to stamp
    // resolvedType on identifier-shaped leaves before checking.
    validateForBound(f.bound.get(), sourceLocFor(f.bound.get()),
                     "bound");

    // R5.0.1 (2026-07-13): range form `for (var i : start, end)`.
    // R5.2-C (2026-07-14): validate `start` the same way.
    if (f.start) {
        validateForBound(f.start.get(), sourceLocFor(f.start.get()),
                         "start");
    }

    // R5.2-B (2026-07-14): symmetric label-scope frame for the
    // for-body. See analyzeWhileStmt's comment.
    _labelStack.emplace_back();
    for (auto& s : f.body) {
        if (s) analyzeStmt(*s);
    }
    _labelStack.pop_back();
}

// R5.2-C (2026-07-14): verify a `for (...)` bound statically
// reduces to int. Hard error otherwise.
//
// We use a small visitor instead of overloading the existing
// `analyzeExpr` dispatch because (a) we want different error
// wording (point at the bound role, not "implicit global"), and
// (b) we must short-circuit (no further diagnostic on subtree)
// once we know the answer.
//
// Two-stage walk:
//   1. Call analyzeExpr(*bound) first to stamp resolvedType on
//      identifier-shaped leaves (existing S2.5 / S3.0 plumbing).
//   2. Then run boundIsStaticallyInt to inspect the stamped tree.
//
// Decision matrix:
//   IntLiteral / BinaryExpr of int literals / UnaryExpr of int /
//   IdentifierExpr whose resolvedType is int / MemberExpr whose
//   resolvedType is int / CallExpr whose resolvedMethod returns int
//     → OK
//   FloatLiteral / StringLiteral / BoolLiteral / IdentifierExpr
//   with non-int resolvedType / IndexExpr / TableExpr / etc.
//     → hard error TypeMismatch
void SemanticAnalyzer::validateForBound(
    Expr* bound, const SourceLocation& loc, const std::string& role)
{
    if (!bound) {
        // Defensive — parser shouldn't produce null bound.
        return;
    }

    // Stage 1: walk the bound subtree so identifier / member / call
    // paths get their resolvedType stamped before we inspect it.
    analyzeExpr(*bound);

    // Stage 2: shape check.
    if (boundIsStaticallyInt(bound)) {
        return;
    }

    // Fall-through: hard error. The bound is not provably int.
    LogiaDiagnostic d;
    d.severity = DiagnosticSeverity::Error;
    d.errorCode = ErrorCode::TypeMismatch;
    d.location = loc;
    if (auto* lit = dynamic_cast<LiteralExpr*>(bound)) {
        // Literal but wrong-shape (float/string/bool).
        std::visit([&](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, float>) {
                d.message = "for-loop " + role +
                            " must be int, got float literal";
            } else if constexpr (std::is_same_v<T, std::string>) {
                d.message = "for-loop " + role +
                            " must be int, got string literal";
            } else if constexpr (std::is_same_v<T, bool>) {
                d.message = "for-loop " + role +
                            " must be int, got bool literal";
            } else {
                d.message = "for-loop " + role +
                            " must be int, got non-int literal";
            }
        }, lit->value);
    } else if (auto* id = dynamic_cast<IdentifierExpr*>(bound)) {
        d.message = "for-loop " + role + " '" + id->name +
                    "' does not have a known int type";
    } else if (auto* m = dynamic_cast<MemberExpr*>(bound)) {
        d.message = "for-loop " + role + " (" + m->member +
                    ") does not have a known int type";
    } else if (auto* c = dynamic_cast<CallExpr*>(bound)) {
        d.message = "for-loop " + role +
                    " call expression does not return int";
    } else {
        d.message = "for-loop " + role +
                    " expression does not statically reduce to int";
    }
    d.hint = "use an int literal, an int-typed variable, "
             "an int-typed self.<field>, or a self.<method>() "
             "returning int";
    report(d);
}

// R5.1 (2026-07-13): no-op for `break;` inside a loop.
// R5.2-B (2026-07-14): verify label visibility when `break :L` is
// used. The parser's `loopDepth` gate has already verified this
// BreakStmt appears inside a while / for body; the label check
// uses the analyzer's `_labelStack` (pushed in analyzeWhileStmt /
// analyzeForStmt) to confirm the label was declared in some
// enclosing loop. The BreakStmt has no sub-nodes to walk.
void SemanticAnalyzer::analyzeBreakStmt(BreakStmt& b)
{
    if (!b.label.empty() && !isLabelVisible(b.label)) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::InvalidStatement;
        d.message = "label '" + b.label +
                    "' not found in any enclosing loop";
        d.hint = "declare with `::" + b.label + "::` inside an "
                 "enclosing while or for body";
        report(d);
    }
}

// R5.1 (2026-07-13): no-op for `continue;` inside a loop.
// R5.2-B (2026-07-14): `continue :L` is not supported in this
// slice — the parser rejects it before we get here. The
// `label` field is always empty in practice (kept on the AST
// for forward compatibility with a future R5.x slice that
// may add name-mangled continue labels).
void SemanticAnalyzer::analyzeContinueStmt(ContinueStmt& /*c*/)
{
}

// R5.2-B (2026-07-14): `::LABEL::` — register the name in the
// innermost label-scope frame. The frame corresponds to the
// enclosing loop body (analyzeWhileStmt / analyzeForStmt pushed
// it; the function-body frame from analyzeLifecycle is also
// present but labels in it are illegal). Duplicate names in the
// same frame are a hard error.
void SemanticAnalyzer::analyzeLabelDeclStmt(LabelDeclStmt& l)
{
    if (_labelStack.empty()) {
        // Should not happen in practice — analyzeLifecycle pushes
        // the outermost frame, so the stack is non-empty inside
        // any lifecycle body. Defensive guard.
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::InvalidStatement;
        d.message = "label '" + l.name + "' declared outside loop";
        d.hint = "labels may only be declared inside a while or for body";
        report(d);
        return;
    }
    // The outermost frame is the function-body frame
    // (analyzeLifecycle); labels declared there are illegal per
    // the loop-scoped rule. We detect that case by checking the
    // frame's size relative to its position. Simpler: check
    // whether the current frame is the only one on the stack.
    if (_labelStack.size() == 1u) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::InvalidStatement;
        d.message = "label '" + l.name + "' declared outside loop";
        d.hint = "labels may only be declared inside a while or for body";
        report(d);
        return;
    }
    if (!_labelStack.back().insert(l.name).second) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::InvalidStatement;
        d.message = "duplicate label '" + l.name + "' in this loop";
        d.hint = "each label name must be unique within its enclosing loop";
        report(d);
    }
}

// R5.2-B (2026-07-14): walk `_labelStack` from innermost frame
// outward and return true on first match. False when the stack
// is empty.
bool SemanticAnalyzer::isLabelVisible(const std::string& name) const
{
    for (auto it = _labelStack.rbegin(); it != _labelStack.rend(); ++it) {
        if (it->count(name)) return true;
    }
    return false;
}

void SemanticAnalyzer::analyzeStmt(Stmt& s)
{
    if (auto* es = dynamic_cast<ExprStmt*>(&s)) {
        // 2026-07-11 audit fix: surface Lua-keyword leaks as a soft
        // diagnostic. The Logia lexer does not reserve `local`,
        // `nil`, `function`, etc., so a bare-expression statement
        // becomes an IdentifierExpr whose name is itself a Lua
        // keyword. This is how `local s = expr` parses today
        // (ExprStmt(IdentifierExpr("local")) followed by a separate
        // BinaryExpr assignment). Until R5+ locks the grammar down,
        // surface the leak once per script — analyzer deduplicates by
        // script-level set since every R3/R4 audit fixture trips
        // this repeatedly and we don't want to flood diagnostics.
        if (es->expr) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(es->expr.get())) {
                static const std::unordered_set<std::string>
                    kLuaKeywordLeaks = {"local", "nil", "function",
                                         "then", "end", "do", "while",
                                         "for", "repeat", "until",
                                         "break", "continue", "return"};
                if (kLuaKeywordLeaks.count(id->name)) {
                    LogiaDiagnostic d;
                    d.severity = DiagnosticSeverity::Warning;
                    d.errorCode = ErrorCode::LuaKeywordLeak;
                    d.message = std::string("'") + id->name +
                               "' is a Lua keyword; Logia recommends "
                               "`var NAME : TYPE = expr` (at script-block "
                               "scope) instead of `" + id->name + " = expr`.";
                    d.hint = "Use `var x: int = 0` at script-block scope, "
                             "then `x = new_value` inside the lifecycle body. "
                             "Add `function NAME(...)` for a script-block "
                             "helper (2026-07-11 audit fix).";
                    report(d);
                }
            }
            analyzeExpr(*es->expr);
        }
    } else if (auto* is = dynamic_cast<IfStmt*>(&s)) {
        if (is->condition) analyzeExpr(*is->condition);
        for (auto& t : is->thenBranch) if (t) analyzeStmt(*t);
        for (auto& e : is->elseBranch) if (e) analyzeStmt(*e);
    } else if (auto* ws = dynamic_cast<WhileStmt*>(&s)) {     // R5.0 (2026-07-13)
        analyzeWhileStmt(*ws);
    } else if (auto* fs = dynamic_cast<ForStmt*>(&s)) {       // R5.0 (2026-07-13)
        analyzeForStmt(*fs);
    } else if (auto* bs = dynamic_cast<BreakStmt*>(&s)) {    // R5.1 (2026-07-13)
        analyzeBreakStmt(*bs);
    } else if (auto* cs = dynamic_cast<ContinueStmt*>(&s)) { // R5.1 (2026-07-13)
        analyzeContinueStmt(*cs);
    } else if (auto* ld = dynamic_cast<LabelDeclStmt*>(&s)) { // R5.2-B (2026-07-14)
        analyzeLabelDeclStmt(*ld);
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
        // S3.12 (track R2 §5.7.4): detect `self.<method>(...)` and
        // stamp resolvedMethod. Codegen reads this to emit
        // `ayt_reflect_call_method(self, "<Type>", "<m>", ...)`
        // instead of bare Lua dispatch. Only fires for the
        // self-receiver case (kind=Component/System, expectSelf=true).
        if (c->callee) {
            if (auto* mem = dynamic_cast<MemberExpr*>(c->callee.get())) {
                // Check `mem->object` is the `self` identifier.
                auto* selfId = dynamic_cast<IdentifierExpr*>(mem->object.get());
                if (selfId && selfId->name == "self" && _ctx.hostType != nullptr) {
                    auto* m = _ctx.hostType->findMethod(mem->member.c_str());
                    if (m) {
                        c->resolvedMethod = m;
                        c->resolvedMethodOwnerName = _ctx.hostType->getName();
                        c->resolvedType = m->getReturnType();
                    }
                }
            }
        }
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