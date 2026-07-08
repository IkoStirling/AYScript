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

#include "IAYReflect.h"

#include <string>

using namespace ayt::script::logia;

namespace {

bool hasError(const CompileResult& r, ErrorCode code)
{
    for (const auto& d : r.diagnostics) {
        if (d.severity == DiagnosticSeverity::Error && d.errorCode == code) {
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

TEST_SUITE_END