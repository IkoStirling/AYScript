// Logia semantic analyzer unit tests (S2.5)
//
// Verifies that SemanticAnalyzer:
//   - hard-errors on unknown var type names
//   - hard-errors on lifecycle functions declared with parameters
//   - hard-errors on `get_component(...)` (S2.5 removed)
//   - soft-warns on member access where the field doesn't exist on a
//     registered type
//   - soft-warns on read of undeclared identifiers (Lua-style implicit
//     global)
//   - resolves `self.field` against AYReflect fields
//   - stamps `VarDeclStmt::resolvedType` for known registered types
//   - accepts the canonical player_controller.logia example

#include "AYScript.h"
#include "logia/AYSemanticAnalyzer.h"
#include "AYTest.h"

#include "ayreflect/IReflect.h"
#include "AYReflect.h"
#include "AYReflectMacros.h"

#include <string>

using namespace ayt::script::logia;

namespace {

// ============================================================================
// S3.2 (LG-04b, B-min) test fixtures
// ----------------------------------------------------------------------------
// LogiaHostContext::strictInheritance=true requires:
//   1. ctx.hostType != nullptr  — must point to a registered ITypeInfo*
//   2. The script's reflect entry, if found, must be derived from hostType
//      per ayt::reflect::isDerivedFrom (single-chain walk).
//
// The fixtures below register a hierarchy entirely from this TU:
//   ScriptComponentBase (root, treated as the expected host base)
//     ^-- StrictDerived     (AY_INHERITS → ScriptComponentBase)
//   StrictUnrelated        (no AY_INHERITS — must be hard-rejected)
// ============================================================================
struct ScriptComponentBase {
    int baseField = 0;
};

struct StrictDerived : ScriptComponentBase {
    int derivedField = 0;
};

struct StrictUnrelated {
    int aloneField = 0;
};

namespace
{
struct StrictFixtureRegistrar {
    StrictFixtureRegistrar() {
        auto& reg = ayt::reflect::TypeRegistryImpl::instance();

        // Register ScriptComponentBase (root). Use a no-op field
        // registrar pattern: we cannot use AY_PROPERTY from a non-class
        // context, so we register a bare TypeInfoImpl<ScriptComponentBase>.
        if (!reg.findType("ScriptComponentBase")) {
            auto* info = new ayt::reflect::TypeInfoImpl<ScriptComponentBase>(
                "ScriptComponentBase",
                ayt::reflect::detail::defaultCreate<ScriptComponentBase>,
                ayt::reflect::detail::defaultDestroy<ScriptComponentBase>,
                ayt::reflect::detail::defaultCopy<ScriptComponentBase>);
            reg.registerTypeInfo("ScriptComponentBase", info);
        }

        // Register StrictDerived (sets base = ScriptComponentBase).
        if (!reg.findType("StrictDerived")) {
            auto* info = new ayt::reflect::TypeInfoImpl<StrictDerived>(
                "StrictDerived",
                ayt::reflect::detail::defaultCreate<StrictDerived>,
                ayt::reflect::detail::defaultDestroy<StrictDerived>,
                ayt::reflect::detail::defaultCopy<StrictDerived>);
            reg.registerTypeInfo("StrictDerived", info);
            // Wire parent: StrictDerived → ScriptComponentBase.
            reg.setBaseTypeByName("StrictDerived", "ScriptComponentBase");
        }

        // Register StrictUnrelated (no parent).
        if (!reg.findType("StrictUnrelated")) {
            auto* info = new ayt::reflect::TypeInfoImpl<StrictUnrelated>(
                "StrictUnrelated",
                ayt::reflect::detail::defaultCreate<StrictUnrelated>,
                ayt::reflect::detail::defaultDestroy<StrictUnrelated>,
                ayt::reflect::detail::defaultCopy<StrictUnrelated>);
            reg.registerTypeInfo("StrictUnrelated", info);
        }
    }
};
static StrictFixtureRegistrar g_strictFixtureRegistrar;
} // namespace



bool hasError(const CompileResult& r, ErrorCode code)
{
    for (const auto& d : r.diagnostics) {
        if (d.severity == DiagnosticSeverity::Error && d.errorCode == code) {
            return true;
        }
    }
    return false;
}

bool hasWarningWithMessage(const CompileResult& r, const std::string& needle)
{
    for (const auto& d : r.diagnostics) {
        if (d.severity == DiagnosticSeverity::Warning
            && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasWarning(const CompileResult& r, ErrorCode code)
{
    for (const auto& d : r.diagnostics) {
        if (d.severity == DiagnosticSeverity::Warning && d.errorCode == code) {
            return true;
        }
    }
    return false;
}

const VarDeclStmt* findVar(const Program& p, const std::string& name)
{
    for (const auto& c : p.scripts) {
        if (!c) continue;
        for (const auto& m : c->members) {
            if (!m) continue;
            if (auto* v = dynamic_cast<VarDeclStmt*>(m.get())) {
                if (v->name == name) return v;
            }
        }
    }
    return nullptr;
}

} // namespace

TEST_SUITE(LogiaSemanticTests)

TEST_CASE(semantic_unknown_var_type_is_hard_error) {
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    var x: NotARealType
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(semantic_known_registered_type_resolves) {
    // Transform is registered by AYEntity's static-init chain; the
    // semantic analyzer must accept it.
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    var t: Transform
}
)");
    CHECK(r.success);
    auto* v = findVar(*r.program, "t");
    CHECK_NOT_NULL(v);
    CHECK_NOT_NULL(v->resolvedType);
    CHECK(std::string(v->resolvedType->getName()) == "Transform");
}

TEST_CASE(semantic_builtin_types_resolve) {
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    var i: int = 0
    var f: float = 0.0
    var b: bool = true
    var s: string = ""
    var e: Entity
}
)");
    CHECK(r.success);
    CHECK_NOT_NULL(findVar(*r.program, "i"));
    CHECK_NOT_NULL(findVar(*r.program, "f"));
    CHECK_NOT_NULL(findVar(*r.program, "b"));
    CHECK_NOT_NULL(findVar(*r.program, "s"));
    CHECK_NOT_NULL(findVar(*r.program, "e"));
}

TEST_CASE(semantic_lifecycle_with_params_is_warning) {
    // S2.5: lifecycle functions take no parameters. The parser forgives
    // non-empty param lists (for backward compat) and SemanticAnalyzer
    // emits a soft warning. The compile still succeeds.
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    on_start(entity: Entity) {
    }
}
)");
    CHECK(r.success);
    CHECK(hasWarning(r, ErrorCode::InvalidStatement));
}

TEST_CASE(semantic_get_component_is_soft_warning) {
    // S2.5: `get_component(...)` is removed from the language.
    // `get_component` is now treated as a regular identifier (not
    // ambient) — reading it produces a soft warning (Lua-style
    // implicit global). The compile still succeeds; runtime would
    // fail if the script actually called it.
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    var t: Transform
    on_start() {
        t = get_component(Transform)
    }
}
)");
    CHECK(r.success);
    CHECK(hasWarning(r, ErrorCode::UnknownIdentifier));
}

TEST_CASE(semantic_export_keyword_is_syntax_error) {
    // S2.5: `export var` is removed. The parser will reject `export`
    // as an unrecognized token inside a script body.
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    export var x: int = 5
}
)");
    CHECK_FALSE(r.success);
}

TEST_CASE(semantic_undeclared_identifier_is_soft_warning) {
    // S2.5: undeclared identifier read = Lua-style implicit global —
    // warn, don't fail.
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    on_update() {
        nonsense + 1
    }
}
)");
    CHECK(r.success);
    CHECK(hasWarning(r, ErrorCode::UnknownIdentifier));
}

TEST_CASE(semantic_ambient_input_log_resolve) {
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    on_update() {
        input.is_pressed("x")
        log.info("hi")
    }
}
)");
    CHECK(r.success);
}

TEST_CASE(semantic_self_field_resolves_against_AYReflect) {
    // `self.position` must resolve against Transform's AY_PROPERTY.
    // SemanticAnalyzer stamps `resolvedField` on the leaf MemberExpr.
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    var t: Transform
    on_update() {
        t.position.x = 0.0
    }
}
)");
    CHECK(r.success);
    bool foundChain = false;
    for (const auto& sc : r.program->scripts) {
        if (!sc) continue;
        for (const auto& m : sc->members) {
            if (!m) continue;
            if (auto* lf = dynamic_cast<LifecycleFuncDecl*>(m.get())) {
                for (const auto& s : lf->body) {
                    if (!s) continue;
                    if (auto* es = dynamic_cast<ExprStmt*>(s.get())) {
                        if (auto* as = dynamic_cast<BinaryExpr*>(es->expr.get())) {
                            if (auto* me = dynamic_cast<MemberExpr*>(as->left.get())) {
                                if (me->member == "x") {
                                    foundChain = true;
                                    // `me` is `.x` (FVector3 leaf — may be null).
                                    // Walk one level down to `.position`.
                                    if (auto* posME = dynamic_cast<MemberExpr*>(me->object.get())) {
                                        CHECK(posME->member == "position");
                                        CHECK_NOT_NULL(posME->resolvedField);
                                        CHECK_NOT_NULL(posME->resolvedType);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(foundChain);
}

TEST_CASE(semantic_self_unknown_field_is_soft_warning) {
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    var t: Transform
    on_update() {
        t.no_such_field = 1
    }
}
)");
    // Soft warning: compile still succeeds.
    CHECK(r.success);
    CHECK(hasWarning(r, ErrorCode::UnknownIdentifier));
}

TEST_CASE(semantic_literal_types) {
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    on_update() {
        1
        1.0
        true
        "x"
    }
}
)");
    CHECK(r.success);
}

TEST_CASE(semantic_binary_expr_int_arith) {
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    var a: int = 0
    var b: int = 0
    on_update() {
        a = a + b
    }
}
)");
    CHECK(r.success);
}

TEST_CASE(semantic_player_controller_example_compiles) {
    // The canonical example from examples/player_controller.logia.
    const char* src = R"(
script PlayerController {
    var tick_counter: int = 0

    on_start() {
        tick_counter = 0
    }

    on_update(dt: float) {
        tick_counter = tick_counter + 1
        if input.is_pressed("jump") {
            self.position.y = self.position.y + self.jump_force * dt
        }
        self.position.x = self.position.x + self.speed * dt
    }

    on_destroy() {
        log.info("PlayerController destroyed")
    }
}
)";
    // Note: `script PlayerController` itself is not registered with
    // AYReflect (only Transform/HealthComponent are). SemanticAnalyzer
    // emits a soft warning on the unknown script name; the compile
    // still succeeds.
    Compiler c;
    auto r = c.compile(src);
    CHECK(r.success);
}

// ============================================================================
// S3.0 (LG-03) — LogiaHostContext plumbing
// ----------------------------------------------------------------------------
// LG-03 exposes `LogiaHostContext` and a new `Compiler::compile(source, ctx)`
// overload. The LG-03 contract is that:
//   1. The default no-ctx overload behaves exactly like S2.5 (no
//      regressions in any of the S2.5 tests above).
//   2. The ctx overload stores the context in both the compiler and the
//      semantic analyzer (verified via `Compiler::lastHostContext()` and
//      `SemanticAnalyzer::hostContext()`), but does NOT introduce new
//      diagnostics — every S2.5 diagnostic continues to appear.
//   3. Unknown script names still emit a soft warning (the LG-03 message
//      is the new generic "no matching registered type" wording).
// ============================================================================

TEST_CASE(lg03_default_compile_matches_s25_implicit_component) {
    // The S2.5 no-ctx overload must construct an implicit Component
    // context. We verify that by calling the ctx overload with the
    // exact same defaults and comparing the diagnostic count + content.
    const char* src = R"(
script UnknownT3 {
    var x: int = 0
    on_update() {
        x = x + 1
    }
}
)";

    Compiler noCtxC;
    auto noCtxRes = noCtxC.compile(src);
    CHECK(noCtxRes.success);  // soft warning only

    Compiler explicitC;
    auto explicitRes = explicitC.compile(src, defaultLogiaHostContext());
    CHECK(explicitRes.success);

    // Same diagnostic count (same set of S2.5 diagnostics).
    CHECK(noCtxRes.diagnostics.size() == explicitRes.diagnostics.size());

    // The no-ctx path recorded the implicit default context.
    CHECK(noCtxC.lastHostContext().kind == LogiaHostKind::Component);
    CHECK(noCtxC.lastHostContext().hostType == nullptr);
    CHECK(noCtxC.lastHostContext().expectSelf == true);
}

TEST_CASE(lg03_explicit_ctx_is_stored) {
    // A non-default ctx is preserved verbatim on Compiler. We pass a
    // System-kind context with hostType explicitly null and
    // expectSelf=false. LG-03 does not consume these fields for
    // validation, but the storage round-trip must be lossless so S3.1+
    // can read them.
    LogiaHostContext custom;
    custom.kind = LogiaHostKind::System;
    custom.hostType = nullptr;
    custom.expectSelf = false;

    const char* src = R"(
script MovementSystemT {
    on_update() {
        x = 1
    }
}
)";

    Compiler c;
    auto r = c.compile(src, custom);
    CHECK(r.success);  // soft warning only (unknown script + implicit global)
    CHECK(c.lastHostContext().kind == LogiaHostKind::System);
    CHECK(c.lastHostContext().hostType == nullptr);
    CHECK(c.lastHostContext().expectSelf == false);

    // The analyzer instance also sees the same context (round-trip
    // plumbing verified by re-invoking analyze() through a public
    // SemanticAnalyzer constructed the same way).
    SemanticAnalyzer sem({}, custom);
    CHECK(sem.hostContext().kind == LogiaHostKind::System);
    CHECK(sem.hostContext().expectSelf == false);
}

TEST_CASE(lg03_unknown_script_warning_unchanged_in_shape) {
    // LG-03 changes the warning text from "no matching registered
    // ScriptComponent subclass" to the more generic "no matching
    // registered type" (since the registry may eventually hold host
    // types other than ScriptComponent). The diagnostic *shape* — one
    // soft warning with code UnknownIdentifier, compile still
    // succeeds — must remain identical so S2.5 callers don't break.
    const char* src = R"(
script NotARealHostType {
    on_update() {
        x = 1
    }
}
)";
    Compiler c;
    auto r = c.compile(src);
    CHECK(r.success);

    bool foundUnknownHostWarning = false;
    for (const auto& d : r.diagnostics) {
        if (d.severity == DiagnosticSeverity::Warning
            && d.errorCode == ErrorCode::UnknownIdentifier
            && d.message.find("NotARealHostType") != std::string::npos) {
            foundUnknownHostWarning = true;
            // LG-03 wording: the new generic message. If this assertion
            // ever changes, the S2.5 doc on §1.6/§5.6 must be revisited.
            CHECK(d.message.find("no matching registered type")
                  != std::string::npos);
        }
    }
    CHECK(foundUnknownHostWarning);
}

// ============================================================================
// S3.2 (LG-04b, B-min) — LogiaHostContext::strictInheritance
// ----------------------------------------------------------------------------
// Verifies the Component-host strict inheritance check:
//   - Default ctx (strictInheritance=false) accepts any registered
//     script name, even unrelated types — matches LG-03 / S2.5
//     behavior.
//   - With strictInheritance=true AND hostType=ScriptComponentBase:
//       * script StrictDerived      → compiles (parent chain matches).
//       * script StrictUnrelated    → hard error (TypeMismatch).
//       * script TotallyUnknownName → no strict check applies (the
//         type was not found, so the soft warning path triggers but
//         strictInheritance does not fire).
// ============================================================================

TEST_CASE(lg04b_strict_off_accepts_unrelated_registered_type) {
    // Sanity: the S2.5 / LG-03 / LG-04 default path tolerates any
    // registered name. We turn strictInheritance OFF and confirm an
    // unrelated registered type still compiles.
    const char* src = R"(
script StrictUnrelated {
    on_update() {
        x = 1
    }
}
)";
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Component;
    ctx.strictInheritance = false;

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK(r.success);
}

TEST_CASE(lg04b_strict_on_accepts_derived_type) {
    // strictInheritance=true with hostType=ScriptComponentBase AND
    // a script whose Reflect entry IS derived from that base → pass.
    const char* src = R"(
script StrictDerived {
    on_update() {
        x = 1
    }
}
)";
    auto* base = ayt::reflect::TypeRegistryImpl::instance()
                    .findType("ScriptComponentBase");
    CHECK_NOT_NULL(base);

    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Component;
    ctx.hostType = base;
    ctx.strictInheritance = true;

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK(r.success);
    // The soft "no matching registered type" warning must NOT fire
    // because StrictDerived IS in the registry.
    CHECK_FALSE(hasWarningWithMessage(r, "no matching registered type"));
    // The strict-mode error must NOT fire either.
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(lg04b_strict_on_rejects_unrelated_type) {
    // strictInheritance=true with hostType=ScriptComponentBase AND
    // a script whose Reflect entry is NOT derived → hard error.
    const char* src = R"(
script StrictUnrelated {
    on_update() {
        x = 1
    }
}
)";
    auto* base = ayt::reflect::TypeRegistryImpl::instance()
                    .findType("ScriptComponentBase");
    CHECK_NOT_NULL(base);

    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Component;
    ctx.hostType = base;
    ctx.strictInheritance = true;

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(lg04b_strict_on_ignores_unknown_names) {
    // strictInheritance only checks script names that ARE in the
    // registry. A genuinely unknown name still emits the S2.5 / LG-03
    // soft warning and does NOT also raise a strict-mode hard error
    // (the caller is expected to handle missing types separately).
    const char* src = R"(
script Lg04bNotInRegistry {
    on_update() {
        x = 1
    }
}
)";
    auto* base = ayt::reflect::TypeRegistryImpl::instance()
                    .findType("ScriptComponentBase");
    CHECK_NOT_NULL(base);

    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Component;
    ctx.hostType = base;
    ctx.strictInheritance = true;

    Compiler c;
    auto r = c.compile(src, ctx);
    // Compile still succeeds (soft warning, not hard error).
    CHECK(r.success);
    CHECK(hasWarningWithMessage(r, "Lg04bNotInRegistry"));
}

TEST_CASE(lg04b_strict_only_applies_to_component_host) {
    // strictInheritance=true is a no-op when kind != Component — the
    // System host keeps its hostKind + registry + World registration
    // checks only.
    const char* src = R"(
script StrictUnrelated {
    on_update() {
        x = 1
    }
}
)";
    auto* base = ayt::reflect::TypeRegistryImpl::instance()
                    .findType("ScriptComponentBase");
    CHECK_NOT_NULL(base);

    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::System;  // System host, not Component
    ctx.hostType = base;
    ctx.strictInheritance = true;

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK(r.success);  // strict check skipped for System host
}

// ============================================================================
// R5.2-C (2026-07-14) — `for`-loop bound type validation
// ----------------------------------------------------------------------------
// R5.2-C closes the long-deferred bound type-check (the R5.0.1 followup
// note at AYSemanticAnalyzer.cpp:571-574). Any bound that cannot be
// statically reduced to int is a hard `ErrorCode::TypeMismatch` — the
// compile fails. Decision matrix lives on validateForBound's comment.
// ============================================================================

TEST_CASE(r52c_bound_int_literal_is_ok) {
    // R5.2-C: an int-literal bound is the canonical OK case.
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : 10) { } } }
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52c_bound_float_literal_is_hard_error) {
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : 3.14) { } } }
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52c_bound_string_literal_is_hard_error) {
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : "hello") { } } }
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52c_bound_bool_literal_is_hard_error) {
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : true) { } } }
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52c_bound_undeclared_identifier_is_hard_error) {
    // `n` is not declared anywhere → resolvedType=nullptr → reject.
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : n) { } } }
)");
    CHECK_FALSE(r.success);
    // R5.2-C takes precedence over the prior UnknownIdentifier warning
    // because the TypeMismatch check fires on null resolvedType.
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52c_bound_self_var_int_is_ok) {
    // Declare an `int` script var and use it as bound.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var n: int = 5
    on_start() { for (var i : n) { } }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52c_bound_self_var_float_is_hard_error) {
    Compiler c;
    auto r = c.compile(R"(
script T {
    var f: float = 1.5
    on_start() { for (var i : f) { } }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52c_bound_binary_int_literals_is_ok) {
    // Constant-folded shape: `3 + 5` reduces to 8 statically.
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : 3 + 5) { } } }
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52c_bound_binary_with_non_int_leaf_is_hard_error) {
    // `n` is undeclared → resolvedType=nullptr → BinaryExpr check
    // fails because the right leaf is not statically int.
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : 3 + n) { } } }
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52c_range_form_both_bounds_validated) {
    // `for (var i : lo, hi)` — both must be int.
    // (Logia R5.2-A reserved `end` as a do-block closer keyword, so
    // use `lo` / `hi` for the range-form fixture.)
    Compiler c;
    auto r = c.compile(R"(
script T {
    var lo: int = 0
    var hi: int = 10
    on_start() { for (var i : lo, hi) { } }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52c_range_form_non_int_end_is_hard_error) {
    Compiler c;
    auto r = c.compile(R"(
script T {
    var lo: int = 0
    var f: float = 1.5
    on_start() { for (var i : lo, f) { } }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52c_regression_r50_short_form_int_literal_still_works) {
    // Regression: the R5.0 short form with int literal must still
    // compile clean (the most common existing pattern, asserted by
    // all prior for-loop tests).
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() {
        var sum: int = 0
        for (var i : 10) { sum = sum + 1 }
    }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

// R5.2-D (2026-07-14): optional `step` parameter — must be a
// positive int constant. Non-literal int (var / self.field) and
// step <= 0 (incl. step == 0) are hard errors. Static-only.

TEST_CASE(r52d_step_int_literal_is_ok) {
    // R5.2-D: simplest positive step case.
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : 0, 10, 2) { } } }
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52d_step_constant_folded_positive_is_ok) {
    // R5.2-D: `3 + 5` constant-folds to 8, which is > 0.
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : 0, 10, 3 + 5) { } } }
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52d_step_zero_is_hard_error) {
    // R5.2-D: step == 0 is rejected (would infinite-loop at runtime).
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : 0, 10, 0) { } } }
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52d_step_negative_literal_is_hard_error) {
    // R5.2-D: step < 0 is rejected.
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : 0, 10, -1) { } } }
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52d_step_unary_negation_of_positive_is_hard_error) {
    // R5.2-D: `-5` folds to -5 which is < 0.
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : 0, 10, -5) { } } }
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52d_step_constant_folded_negative_is_hard_error) {
    // R5.2-D: `3 - 5` folds to -2, which is < 0.
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : 0, 10, 3 - 5) { } } }
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52d_step_identifier_is_hard_error) {
    // R5.2-D: step-by-variable is rejected, even when the
    // variable is int-typed. Stricter than bound semantics.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var s: int = 2
    on_start() { for (var i : 0, 10, s) { } }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52d_step_float_literal_is_hard_error) {
    // R5.2-D: float literal can't be a step either (inherits
    // R5.2-C TypeMismatch before getting to the value check).
    Compiler c;
    auto r = c.compile(R"(
script T { on_start() { for (var i : 0, 10, 1.5) { } } }
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52d_regression_short_form_unchanged) {
    // R5.2-D regression: short form must keep R5.0 behavior,
    // no step accepted, no codegen change.
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() {
        var sum: int = 0
        for (var i : 5) { sum = sum + i }
    }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52d_regression_range_form_without_step_unchanged) {
    // R5.2-D regression: range form without step keeps
    // R5.0.1 half-open behavior, no codegen change.
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() {
        var sum: int = 0
        for (var i : 3, 7) { sum = sum + i }
    }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_SUITE_END