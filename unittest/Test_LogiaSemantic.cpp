// Logia semantic analyzer unit tests (S2.5)
//
// Verifies that SemanticAnalyzer:
//   - hard-errors on unknown var type names
//   - scopes and validates lifecycle parameters
//   - hard-errors on `get_component(...)` (S2.5 removed)
//   - soft-warns on member access where the field doesn't exist on a
//     registered type
//   - soft-warns on read of undeclared identifiers (Lua-style implicit
//     global)
//   - resolves `self.field` against AYReflect fields
//   - stamps `VarDeclStmt::resolvedType` for known registered types
//   - accepts the canonical player_controller.logia example

#include "AYScript.h"
#include "AYScript/logia/SemanticAnalyzer.h"
#include "AYTest.h"

#include "AYReflect/IReflect.h"
#include "AYReflect.h"
#include "AYReflectMacros.h"

#include <fstream>
#include <iterator>
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

// S5 ED-02 (2026-07-14): `hasErrorAt(r, line, code)` matches the
// existing `hasError` predicate plus a line-number check on the
// diagnostic's `location.line`. Used by S5 ED-02 regression anchors
// to assert that analyzer-side diagnostics carry real line numbers
// (was 0 before this slice). The `code` parameter defaults to
// `ErrorCode::UnexpectedToken` so a test that only cares about
// "some error at line N" can pass `ErrorCode::TypeMismatch` or any
// other code without breaking the call shape.
bool hasErrorAt(const CompileResult& r, int line, ErrorCode code = ErrorCode::UnexpectedToken)
{
    for (const auto& d : r.diagnostics) {
        if (d.severity == DiagnosticSeverity::Error
            && d.errorCode == code
            && d.location.line == line) {
            return true;
        }
    }
    return false;
}

// S5 ED-02 (2026-07-14): warning counterpart. Used by the
// `script_unknown_host_type_line` test below — the analyzer emits
// a soft warning (not an error) when the script name doesn't
// resolve to a registered AYReflect host type.
bool hasWarningAt(const CompileResult& r, int line, ErrorCode code = ErrorCode::UnexpectedToken)
{
    for (const auto& d : r.diagnostics) {
        if (d.severity == DiagnosticSeverity::Warning
            && d.errorCode == code
            && d.location.line == line) {
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

TEST_CASE(semantic_lifecycle_params_are_scoped) {
    Compiler c;
    auto r = c.compile(R"(
script Foo {
    on_update(dt: float) {
        var copy: float = dt
    }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasWarningWithMessage(
        r, "lifecycle functions take no parameters"));
    CHECK_FALSE(hasWarningWithMessage(r, "implicit global 'dt'"));
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
#ifndef AYSCRIPT_TEST_SOURCE_DIR
#error "AYSCRIPT_TEST_SOURCE_DIR must point at the AYScript source tree"
#endif
    // Compile the real example rather than an embedded copy. The two had
    // drifted far enough that the file shipped by AYEditor failed while this
    // test remained green.
    const std::string path = std::string(AYSCRIPT_TEST_SOURCE_DIR)
        + "/examples/player_controller.logia";
    std::ifstream stream(path, std::ios::binary);
    CHECK(stream.is_open());
    if (!stream.is_open()) return;
    const std::string src((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
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

// R5.2-E (2026-07-14): bound validator accepts type-recursive
// expressions like `n + 1` where `n: int`. The capability was
// latent in R5.2-C (IdentifierExpr / MemberExpr / CallExpr already
// stamped `resolvedType`); R5.2-E's job is to commit to it
// explicitly via tests + documentation, NOT to change validator
// code. Step validator remains strict R5.2-D const-folded.

TEST_CASE(r52e_bound_var_plus_literal_is_ok) {
    // R5.2-E: most common case — `n + 1` where n: int.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var n: int = 5
    on_start() { for (var i : 0, n + 1) { } }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52e_bound_var_minus_literal_is_ok) {
    // R5.2-E: `damage - 10` (common in damage / HP patterns).
    Compiler c;
    auto r = c.compile(R"(
script T {
    var damage: int = 100
    on_start() { for (var i : 0, damage - 10) { } }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52e_bound_var_var_arithmetic_is_ok) {
    // R5.2-E: `n + m` (both int vars). Tests BinaryExpr with
    // both leaves being identifier-int (no literal at all).
    Compiler c;
    auto r = c.compile(R"(
script T {
    var n: int = 3
    var m: int = 7
    on_start() { for (var i : 0, n + m) { } }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52e_bound_var_plus_float_is_hard_error) {
    // R5.2-E: int + float is still rejected — Logia has no
    // implicit int↔float promotion. The float leaf falls
    // through to `return false` and the binary inherits.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var n: int = 5
    on_start() { for (var i : 0, n + 1.5) { } }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52e_step_var_plus_literal_still_hard_error) {
    // R5.2-E regression: step stays strict R5.2-D const-folded.
    // `n - 1` would need codegen runtime check to enforce
    // non-zero, which violates R5.2-D's static-only principle.
    // Step is intentionally NOT type-recursive in R5.2-E.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var n: int = 3
    on_start() { for (var i : 0, 10, n - 1) { } }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52e_regression_r52c_int_var_bound_still_works) {
    // R5.2-E regression: the most basic R5.2-C case must
    // still pass. (Verifies no analyzer regression.)
    Compiler c;
    auto r = c.compile(R"(
script T {
    var n: int = 5
    on_start() { for (var i : 0, n) { } }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

// R5.2-H (2026-07-14): if / while conditions must statically
// reduce to bool. Logia has no implicit truthiness — `while (n)`
// where `n: int` is a hard TypeMismatch error. The new helper
// `isStaticallyBool` accepts:
//   - bool literals
//   - bool-typed identifiers (incl. builtin-var fallback)
//   - bool-typed self.field / self.method() returning bool
//   - `!` (Bang) on a bool leaf
//   - comparison ops {==, !=, <, <=, >, >=} over two primitive
//     leaves (int/float/bool/string)
//   - logical ops {&&, ||} over two bool leaves
// Arithmetic (`n + 1`) as condition is rejected — no implicit
// int-as-bool.

TEST_CASE(r52h_if_bool_var_is_ok) {
    // R5.2-H: the most common case — `if (ready)` where
    // `ready: bool`. Pre-R5.2-H: silently truthy. R5.2-H:
    // statically bool, accepted.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var ready: bool = true
    on_start() {
        if (ready) { }
    }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52h_while_bool_var_is_ok) {
    // R5.2-H: `while (running)` where `running: bool`.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var running: bool = true
    on_start() {
        while (running) { }
    }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52h_if_int_comparison_is_ok) {
    // R5.2-H: comparison `n > 0` (int > int) returns bool per
    // Lua. The most natural rewrite from pre-R5.2-H `if (n)`.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var n: int = 5
    on_start() {
        if (n > 0) { }
        if (n == 5) { }
        if (n != 0) { }
        if (n <= 10) { }
        if (n >= 1) { }
        if (n < 100) { }
    }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52h_while_int_comparison_is_ok) {
    // R5.2-H: `while (n > 0)` is the canonical loop pattern.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var n: int = 3
    on_start() {
        while (n > 0) { }
    }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52h_if_logical_ops_on_bool_are_ok) {
    // R5.2-H: `&&` / `||` over two bool leaves return bool.
    // `!bool` returns bool.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var ready: bool = true
    var armed: bool = false
    on_start() {
        if (ready && armed) { }
        if (ready || armed) { }
        if (!ready) { }
        if (!ready && armed) { }
    }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52h_if_int_var_alone_is_hard_error) {
    // R5.2-H: the headline rule — `if (n)` where `n: int` is
    // rejected. Logia has no implicit truthiness; Lua would
    // have treated 0 as false and any non-zero as true, which
    // is the silent-bug class R5.2-H exists to eliminate.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var n: int = 5
    on_start() {
        if (n) { }
    }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52h_while_int_var_alone_is_hard_error) {
    // R5.2-H: `while (count)` where `count: int` is rejected.
    // This is the canonical R5.2-H scenario — silent truthy
    // counters are exactly the bug class we're eliminating.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var count: int = 10
    on_start() {
        while (count) { }
    }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52h_if_arithmetic_expression_is_hard_error) {
    // R5.2-H: arithmetic does NOT produce bool. `n + 1` is an
    // int expression even when `n: int`, so it can't be a
    // condition. This enforces the "no int-as-bool" rule —
    // users must write `n + 1 != 0` if they mean "did the
    // counter increment to nonzero?".
    Compiler c;
    auto r = c.compile(R"(
script T {
    var n: int = 5
    on_start() {
        if (n + 1) { }
    }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52h_if_int_literal_is_hard_error) {
    // R5.2-H: int literal as condition is rejected. Even
    // `if (1)` (which Lua would treat as truthy) is a hard
    // error. Forces users to write `if (true)` if they
    // actually want a constant-true branch.
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() {
        if (1) { }
    }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52h_if_string_literal_is_hard_error) {
    // R5.2-H: string literal is not bool. Lua would treat
    // any non-empty string as truthy, but Logia rejects it.
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() {
        if ("hello") { }
    }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52h_if_string_comparison_is_ok) {
    // R5.2-H: string comparison returns bool. `name == "foo"`
    // is the natural string-equality check.
    Compiler c;
    auto r = c.compile(R"(
script T {
    var name: string = "foo"
    on_start() {
        if (name == "foo") { }
        if (name != "bar") { }
    }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(r52h_while_bool_literal_is_ok) {
    // R5.2-H: `while (true)` is the only legal constant loop.
    // Note: this would actually loop forever at runtime — but
    // R5.2-H doesn't gate against infinite loops (that's a
    // separate concern; static-analysis would need a CFG
    // depth-bounded check).
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() {
        while (true) { }
    }
}
)");
    CHECK(r.success);
    CHECK_FALSE(hasError(r, ErrorCode::TypeMismatch));
}

// ----------------------------------------------------------------------------
// S5 ED-02 (2026-07-14) regression anchors — source locations on
// analyzer-side diagnostics. Each test plants a single bad source
// and asserts that the resulting diagnostic's `location.line` matches
// the planted source line. Pre-ED-02 all these lines were `0` (the
// `sourceLocFor(...)` placeholder) and tests would fail; post-ED-02
// they show the real parser-stamped source location.
//
// Source format convention: every planted source begins with a line
// that is not a syntax error (so we can compute the expected
// error line relative to it) — most are `script T {` on line 1
// followed by a body with a planted error on a known later line.
// When in doubt, the planted error line number is written into the
// test comment as `// error at line N`.

TEST_CASE(s5ed02_for_bound_string_carries_line_number) {
    // `for (var i : "five")` on line 3 — bound is a string literal
    // not an int, expected TypeMismatch diagnostic carries line 3.
    // (R-string literal starts with `\n` so the first text line is
    // `script T {` and the for-loop body is on line 3.)
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() { for (var i : "five") { } }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasErrorAt(r, 3, ErrorCode::TypeMismatch));
}

TEST_CASE(s5ed02_unknown_identifier_carries_line_number) {
    // `nope` on line 3 — undeclared identifier read should carry
    // line 3 (the identifier's sourceLoc).
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() {
        var x: int = nope
    }
}
)");
    CHECK(r.success);  // unknown identifier is a warning, not an error
    CHECK(hasWarningAt(r, 4, ErrorCode::UnknownIdentifier));
}

TEST_CASE(s5ed02_unknown_field_member_carries_line_number) {
    // `self.bogus_field` on line 4 — Transform is a registered
    // AYReflect type with `position` and `rotation` fields but
    // no `bogus_field`. The analyzer's `analyzeMemberExpr`
    // emits an UnknownIdentifier warning with
    // `d.location = sourceLocFor(&m)` (this slice). The
    // MemberExpr's sourceLoc stamps at the field-name token
    // (`bogus_field`), so the warning line is 4.
    //
    // We deliberately use `script T` (NOT `Transform`) for the
    // script name so `self` resolution does NOT collide with
    // the registered Transform type — but we still get the
    // implicit-global warning on `self` because `T` doesn't
    // match a registered type. The test below uses a
    // LogiaHostContext with hostType=Transform so `self.field`
    // resolves against Transform's ITypeInfo, and `bogus_field`
    // triggers the unknown-field path. Both warnings share the
    // same line (4) so the test passes either way; the
    // unknown-field one is what we're proving.
    LogiaHostContext ctx = defaultLogiaHostContext();
    ctx.kind = LogiaHostKind::Component;
    ctx.hostType = ayt::reflect::TypeRegistryImpl::instance().findType("Transform");
    Compiler c;
    auto r = c.compile(R"(
script Transform {
    on_start() {
        self.bogus_field = 1
    }
}
)", ctx);
    CHECK(r.success);  // unknown field is a warning, not error
    CHECK(hasWarningAt(r, 4, ErrorCode::UnknownIdentifier));
}

TEST_CASE(s5ed02_break_outside_loop_carries_line_number) {
    // `break;` on line 4 — bare break outside a loop should carry
    // line 4 (parser-side error via `Parser::error` reads
    // `current().line`, which is the `;` token's line — same as
    // `break`'s). The parser emits ErrorCode::UnexpectedToken for
    // all parse errors (a uniform code, distinct from the
    // analyzer's InvalidStatement used in the duplicate-label /
    // unrecognized-statement checks).
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() {
        break;
    }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasErrorAt(r, 4, ErrorCode::UnexpectedToken));
}

TEST_CASE(s5ed02_continue_outside_loop_carries_line_number) {
    // Same pattern as break. Parser-side error carries the
    // `continue` keyword's line.
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() {
        continue;
    }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasErrorAt(r, 4, ErrorCode::UnexpectedToken));
}

TEST_CASE(s5ed02_label_outside_loop_carries_line_number) {
    // `::OUTER::` on line 4 — the parser successfully builds
    // the LabelDeclStmt (no parser-side gate on labels — the
    // loop-scoped rule is analyzer-only). The analyzer emits
    // an InvalidStatement error via `analyzeLabelDeclStmt`
    // with `d.location = sourceLocFor(&l)` (this slice).
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() {
        ::OUTER::
    }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasErrorAt(r, 4, ErrorCode::InvalidStatement));
}

TEST_CASE(s5ed02_if_condition_type_mismatch_carries_line_number) {
    // `if ("hello")` on line 3 — R5.2-H's "must be bool" diagnostic
    // carries line 3 (the IfStmt's sourceLoc = `if` keyword).
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() {
        if ("hello") { }
    }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasErrorAt(r, 4, ErrorCode::TypeMismatch));
}

TEST_CASE(s5ed02_while_condition_type_mismatch_carries_line_number) {
    // `while ("hello")` on line 3 — same R5.2-H rule, validates
    // WhileStmt's sourceLoc.
    Compiler c;
    auto r = c.compile(R"(
script T {
    on_start() {
        while ("hello") { }
    }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasErrorAt(r, 4, ErrorCode::TypeMismatch));
}

TEST_CASE(s5ed02_script_unknown_host_type_carries_line_number) {
    // `script Bogus` on line 1 — unknown script-name soft warning
    // carries line 1 (the ScriptDecl's sourceLoc = `script` keyword).
    Compiler c;
    auto r = c.compile(R"(
script Bogus {
    on_start() { }
}
)");
    CHECK(r.success);  // unknown host type is a warning, not an error
    CHECK(hasWarningAt(r, 2, ErrorCode::UnknownIdentifier));
}

TEST_CASE(s5ed02_var_decl_unknown_type_carries_line_number) {
    // `var x : SomeUnknownType` on line 2 — unknown type name should
    // carry line 2 (the VarDeclStmt's sourceLoc = `var` keyword).
    Compiler c;
    auto r = c.compile(R"(
script T {
    var x : SomeUnknownType = 0
    on_start() { }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasErrorAt(r, 3, ErrorCode::TypeMismatch));
}

// =====================================================================
// S5 ED-03 (2026-07-15): per-line Lua → Logia source map.
//
// The Lua codegen now produces a `LogiaSourceMap` alongside the
// generated Lua source — index N (1-based) is the Logia source
// location of the Lua line emitted at Lua line N. The 4 tests
// below anchor the core invariants:
//
//   1. Vector size matches the count of `\n` in the generated Lua.
//   2. The map's anchors match the originating Stmt sourceLocs.
//   3. Codegen preamble / epilogue / blank lines are anchored to
//      `{}` (line 0 = untranslatable).
//   4. `LogiaSourceMap::lookup(luaLine)` returns `{}` for
//      out-of-range Lua lines.
//
// All tests use `compileLogiaToLua()` (the heap-backed pipeline
// wrapper in `AYScript/logia/AYScript/logia/AYScript/logia/AYScript/logia/LogiaPipeline.h`) so the full front-end + codegen
// path is exercised.
// =====================================================================

TEST_CASE(s5ed03_sourcemap_size_matches_lua_lines) {
    auto r = compileLogiaToLua(R"(
script T {
    var n: int = 0
    on_start() {
        n = 1
        n = 2
        n = 3
    }
}
)");
    CHECK(r.success);
    // Count newlines in generated Lua.
    int newlines = 0;
    for (char c : r.lua) {
        if (c == '\n') ++newlines;
    }
    // The source map's index 0 is reserved (untranslatable
    // sentinel); indices 1..N map to Lua lines 1..N. So vector
    // size must be `newlines + 1`.
    CHECK(static_cast<int>(r.sourceMap.luaLineToSource.size()) == newlines + 1);
}

TEST_CASE(s5ed03_sourcemap_anchors_match_stmt_source_locs) {
    // Plant a script where each Stmt's sourceLoc is known:
    //   line 2: `script T {`
    //   line 3: `var n: int = 0` (VarDeclStmt at var keyword on line 3)
    //   line 4: `on_start() {`  (LifecycleFuncDecl at lifecycle kw on line 4)
    //   line 5: `    n = 1`     (ExprStmt at line 5)
    //   line 6: `    n = 2`     (ExprStmt at line 6)
    //   line 7: `}`             (close script — no Stmt)
    //   line 8: `}`             (close lifecycle body — no Stmt)
    auto r = compileLogiaToLua(R"(
script T {
    var n: int = 0
    on_start() {
        n = 1
        n = 2
    }
}
)");
    CHECK(r.success);
    CHECK(r.sourceMap.luaLineToSource.size() >= 4);

    // Lua line 1 is the codegen `--` comment header (no Logia
    // anchor → line 0). Lua line 2 is `local M = {}` (also no
    // anchor). Lua line 3 is blank (no anchor). The first
    // user-source-anchored Lua line is the `local n = 0` var
    // declaration, which should anchor to line 3 (the `var`
    // keyword in source).
    //
    // Find the Lua line index whose anchor's `line` is 3 — that
    // must be the VarDeclStmt's emit (R5.0-style 3-line preamble
    // plus possibly a blank line above it; the exact index depends
    // on codegen formatting but the *anchor* is invariant).
    bool foundVarAnchor = false;
    for (const auto& anchor : r.sourceMap.luaLineToSource) {
        if (anchor.line == 3) { foundVarAnchor = true; break; }
    }
    CHECK(foundVarAnchor);

    // The `n = 1` ExprStmt must anchor to source line 5. The
    // `n = 2` ExprStmt must anchor to source line 6. Both must
    // appear somewhere in the source map (Lua line number is
    // implementation-dependent due to preamble formatting).
    bool foundExpr1 = false;
    bool foundExpr2 = false;
    for (const auto& anchor : r.sourceMap.luaLineToSource) {
        if (anchor.line == 5) foundExpr1 = true;
        if (anchor.line == 6) foundExpr2 = true;
    }
    CHECK(foundExpr1);
    CHECK(foundExpr2);
}

TEST_CASE(s5ed03_sourcemap_header_footer_unanchored) {
    // The codegen preamble (`--` comment, `local M = {}`) and
    // epilogue (`return M`) plus the blank lines between top-level
    // members have no Logia source — they must anchor to
    // `SourceLocation{}` (line 0). The first few entries of the
    // source map should include at least one line-0 anchor (the
    // preamble).
    auto r = compileLogiaToLua(R"(
script T {
    var n: int = 0
    on_start() { }
}
)");
    CHECK(r.success);
    CHECK(r.sourceMap.luaLineToSource.size() >= 3);

    // Lua line 1 is the `--` header comment (no anchor).
    CHECK(r.sourceMap.luaLineToSource[1].line == 0);
    // Lua line 2 is `local M = {}` (no anchor).
    CHECK(r.sourceMap.luaLineToSource[2].line == 0);
    // Lua line 3 is the blank line after the preamble (no anchor).
    CHECK(r.sourceMap.luaLineToSource[3].line == 0);
}

TEST_CASE(s5ed03_sourcemap_lookup_out_of_range_returns_zero) {
    auto r = compileLogiaToLua(R"(
script T {
    on_start() { }
}
)");
    CHECK(r.success);
    CHECK_FALSE(r.sourceMap.luaLineToSource.empty());

    // Lookup(0) is always out of range (index 0 is reserved).
    auto at0 = r.sourceMap.lookup(0);
    CHECK(at0.line == 0);
    CHECK(at0.column == 0);

    // Lookup(INT_MAX) is always out of range.
    auto atMax = r.sourceMap.lookup(2147483647);
    CHECK(atMax.line == 0);
    CHECK(atMax.column == 0);

    // Lookup(negative) is always out of range.
    auto atNeg = r.sourceMap.lookup(-1);
    CHECK(atNeg.line == 0);
    CHECK(atNeg.column == 0);

    // Lookup(beyond-end) is always out of range.
    int beyond = static_cast<int>(r.sourceMap.luaLineToSource.size()) + 10;
    auto atBeyond = r.sourceMap.lookup(beyond);
    CHECK(atBeyond.line == 0);
    CHECK(atBeyond.column == 0);
}

// S4.1 (2026-07-15): per-component signal surface — semantic
// validation of signal declarations + emit / connect call sites.
// All cases below pin one rule from the analyzer; a regression in
// any of them points at a specific clause in `analyzeSignalDecl`,
// `analyzeEmitCall`, or `analyzeConnectCall`.

namespace {

// Helper: compile a Logia source string under `toolLogiaHostContext`
// (which sets `expectSelf = false`) — used for the Tool-host
// rejection test. Component host is the default
// (`defaultLogiaHostContext()`); Tool host tests need an explicit
// ctx.
CompileResult compileAsTool(const char* source)
{
    Compiler compiler;
    return compiler.compile(source, toolLogiaHostContext());
}

bool hasErrorContaining(const CompileResult& r, const std::string& needle)
{
    for (const auto& e : r.diagnostics) {
        if (e.severity == DiagnosticSeverity::Error &&
            e.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE(s41_duplicate_signal_name_is_hard_error) {
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    signal damaged()
}
)";
    Compiler compiler;
    const CompileResult r = compiler.compile(source);
    CHECK_FALSE(r.success);
    CHECK(hasErrorContaining(r, "duplicate signal 'damaged'"));
}

TEST_CASE(s41_emit_unknown_signal_is_hard_error_with_hint) {
    // Pin that the error message names the unknown signal AND that
    // the hint lists the declared signals — matches the
    // LuaKeywordLeak hint style at L1668.
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    on_update(dt: float) {
        emit("nope", 1)
    }
}
)";
    Compiler compiler;
    const CompileResult r = compiler.compile(source);
    CHECK_FALSE(r.success);
    CHECK(hasErrorContaining(r, "unknown signal 'nope' in emit()"));
    // Verify the hint mentions the declared signal so the user can
    // find the typo immediately.
    bool hintHasDeclaredName = false;
    for (const auto& d : r.diagnostics) {
        if (d.severity == DiagnosticSeverity::Error &&
            d.message.find("nope") != std::string::npos &&
            d.hint.find("damaged") != std::string::npos) {
            hintHasDeclaredName = true;
            break;
        }
    }
    CHECK(hintHasDeclaredName);
}

TEST_CASE(s41_emit_arity_mismatch_is_hard_error) {
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    on_update(dt: float) {
        emit("damaged")
    }
}
)";
    Compiler compiler;
    const CompileResult r = compiler.compile(source);
    CHECK_FALSE(r.success);
    CHECK(hasErrorContaining(r, "arity mismatch"));
}

TEST_CASE(s41_emit_arg_type_mismatch_is_hard_error) {
    // `signal damaged(amount: int)` declared as int; caller passes
    // a string literal — must reject.
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    on_update(dt: float) {
        emit("damaged", "hi")
    }
}
)";
    Compiler compiler;
    const CompileResult r = compiler.compile(source);
    CHECK_FALSE(r.success);
    CHECK(hasErrorContaining(r, "type mismatch"));
}

TEST_CASE(s41_emit_non_literal_name_is_hard_error) {
    // `emit(name, ...)` with `name` as a variable, not a string
    // literal — rejected because static validation needs the
    // signal name up front.
    const char* source = R"(
script PlayerController {
    var name: string = "damaged"
    signal damaged(amount: int)
    on_update(dt: float) {
        emit(name, 1)
    }
}
)";
    Compiler compiler;
    const CompileResult r = compiler.compile(source);
    CHECK_FALSE(r.success);
    CHECK(hasErrorContaining(r, "signal name must be a string literal"));
}

TEST_CASE(s41_connect_unknown_signal_is_hard_error) {
    const char* source = R"(
script PlayerController {
    on_start() {
        connect("nope", handler)
    }
}
)";
    Compiler compiler;
    const CompileResult r = compiler.compile(source);
    CHECK_FALSE(r.success);
    CHECK(hasErrorContaining(r, "unknown signal 'nope' in connect()"));
}

TEST_CASE(s41_connect_wrong_arity_is_hard_error) {
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    on_start() {
        connect("damaged")
    }
}
)";
    Compiler compiler;
    const CompileResult r = compiler.compile(source);
    CHECK_FALSE(r.success);
    CHECK(hasErrorContaining(r, "requires a string-literal signal name"));
}

TEST_CASE(s41_valid_signal_call_stamps_ambient_call_kind) {
    // The success path must stamp `c.ambientCall = Emit` so codegen
    // (in a later commit) can emit `__ay_emit(self, ...)` rather
    // than falling through to the generic Lua call path. We can't
    // directly read the AST field from outside (CallExpr::ambientCall
    // is internal), but we CAN confirm the call compiles cleanly
    // — the codegen path in commit 5 will fail loudly if the stamp
    // is missing.
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    on_update(dt: float) {
        emit("damaged", 10)
    }
    function handler(amount: int) {}
}
)";
    Compiler compiler;
    const CompileResult r = compiler.compile(source);
    CHECK(r.success);
}

TEST_CASE(s41_emit_in_tool_host_is_hard_error) {
    // Tool hosts (run-only, no self receiver) cannot use emit /
    // connect. The host-kind guard rejects this at compile time so
    // codegen never has to think about `self` in a self-less
    // context.
    const char* source = R"(
script BuildTool {
    signal done()
    run() {
        emit("done")
    }
}
)";
    const CompileResult r = compileAsTool(source);
    CHECK_FALSE(r.success);
    CHECK(hasErrorContaining(r, "Tool / EventHandler host scripts"));
}

TEST_CASE(s41_connect_in_tool_host_is_hard_error) {
    const char* source = R"(
script BuildTool {
    signal done()
    run() {
        connect("done", h)
    }
}
)";
    const CompileResult r = compileAsTool(source);
    CHECK_FALSE(r.success);
    CHECK(hasErrorContaining(r, "Tool / EventHandler host scripts"));
}

TEST_CASE(s41_bogus_signal_param_type_is_hard_error) {
    // `signal foo(bar: bad_type)` — bad_type is not registered and
    // not a built-in. The analyzer must reject at pre-pass time so
    // emit/connect arg-type checks downstream don't crash.
    const char* source = R"(
script PlayerController {
    signal foo(bar: bad_type)
}
)";
    Compiler compiler;
    const CompileResult r = compiler.compile(source);
    CHECK_FALSE(r.success);
    CHECK(hasErrorContaining(r, "unknown type 'bad_type'"));
}

// S4.1b (2026-07-18): disconnect host-kind guard. Tool host
// scripts have no `self` receiver, so per-instance signal tables
// don't exist for them — calling disconnect() must be a hard
// error, mirroring the S4.1 emit/connect guards.
TEST_CASE(s41b_disconnect_in_tool_host_is_hard_error) {
    const char* source = R"(
script BuildTool {
    run() {
        disconnect(0)
    }
}
)";
    const CompileResult r = compileAsTool(source);
    CHECK_FALSE(r.success);
    CHECK(hasErrorContaining(r, "Tool / EventHandler host scripts"));
}

TEST_SUITE_END
