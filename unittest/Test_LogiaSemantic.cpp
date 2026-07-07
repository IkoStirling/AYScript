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

TEST_SUITE_END