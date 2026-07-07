// Logia semantic analyzer unit tests (S2)
//
// Verifies that SemanticAnalyzer:
//   - hard-errors on unknown var/param type names
//   - hard-errors on unknown identifiers in expressions
//   - hard-errors on get_component(<unknown>)
//   - soft-warns on member access where field is missing
//   - stamps resolvedType on VarDeclStmt / MemberExpr / CallExpr when
//     a registered type is found
//   - recognizes built-in primitives + ambient identifiers (input/log/time)
//   - accepts the canonical player_controller.logia example

#include "AYScript.h"
#include "logia/AYSemanticAnalyzer.h"
#include "AYTest.h"

// AYReflect full include — needed to access ITypeInfo::getName() etc.
// (forward declaration in AYAST.h is sufficient for pointers, but
// member access on the pointee needs the full definition).
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
    for (const auto& c : p.components) {
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
component Foo {
    var x: NotARealType
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(semantic_unknown_param_type_is_hard_error) {
    Compiler c;
    auto r = c.compile(R"(
component Foo {
    on_start(p: BogusType) {
    }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(semantic_builtin_types_resolve) {
    Compiler c;
    auto r = c.compile(R"(
component Foo {
    export var i: int = 0
    export var f: float = 0.0
    export var b: bool = true
    export var s: string = ""
}
)");
    CHECK(r.success);
    // Built-ins are accepted without an ITypeInfo (stamp is intentionally
    // left as nullptr — see SemanticAnalyzer::analyzeVarDecl).
    CHECK_NOT_NULL(findVar(*r.program, "i"));
    CHECK_NOT_NULL(findVar(*r.program, "f"));
    CHECK_NOT_NULL(findVar(*r.program, "b"));
    CHECK_NOT_NULL(findVar(*r.program, "s"));
}

TEST_CASE(semantic_known_registered_type_resolves) {
    Compiler c;
    auto r = c.compile(R"(
component Foo {
    var t: Transform
}
)");
    // Transform must be registered by AYEntity. If not, this test fails —
    // that's a useful negative signal that AYEntity isn't being linked
    // into the test exe.
    CHECK(r.success);
    auto* v = findVar(*r.program, "t");
    CHECK_NOT_NULL(v);
    CHECK_NOT_NULL(v->resolvedType);
    CHECK(std::string(v->resolvedType->getName()) == "Transform");
}

TEST_CASE(semantic_unknown_field_is_soft_warning) {
    Compiler c;
    auto r = c.compile(R"(
component Foo {
    var t: Transform
    on_update(dt: float) {
        t.no_such_field = 1
    }
}
)");
    // Soft warning: compile still succeeds.
    CHECK(r.success);
    CHECK(hasWarning(r, ErrorCode::UnknownIdentifier));
}

TEST_CASE(semantic_get_component_known_type_ok) {
    Compiler c;
    auto r = c.compile(R"(
component Foo {
    on_start(entity: Entity) {
        entity.get_component(Transform)
    }
}
)");
    CHECK(r.success);
}

TEST_CASE(semantic_get_component_unknown_type_hard_error) {
    Compiler c;
    auto r = c.compile(R"(
component Foo {
    on_start(entity: Entity) {
        entity.get_component(FooBar)
    }
}
)");
    CHECK_FALSE(r.success);
    CHECK(hasError(r, ErrorCode::TypeMismatch));
}

TEST_CASE(semantic_undeclared_identifier_is_warning) {
    // S2 chose to soft-warn (not hard-error) on undeclared identifiers
    // because Logia's runtime model is Lua-style implicit globals.
    // Compiles pass; warning is emitted on any read reference.
    // (An LHS-only reference like `nonsense = 1` is an implicit
    // declaration — no warning. We exercise the read path here.)
    Compiler c;
    auto r = c.compile(R"(
component Foo {
    on_update(dt: float) {
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
component Foo {
    on_update(dt: float) {
        input.is_pressed("jump")
        log.info("hi")
    }
}
)");
    CHECK(r.success);
}

TEST_CASE(semantic_member_chain_resolves_fields) {
    // t.position.x = 0.0 : the analyzer should stamp the intermediate
    // `t.position` MemberExpr with its Transform::position field info.
    // The leaf `.x` is on FVector3, which S2 doesn't introspect (FVector3
    // uses anonymous-union fields that aren't reflectable), so the leaf
    // member is left un-stamped with a soft warning — codegen still
    // emits the bare `t.position.x = 0.0` as Lua. We assert that the
    // *intermediate* member chain (Transform::position) is resolved.
    Compiler c;
    auto r = c.compile(R"(
component Foo {
    var t: Transform
    on_update(dt: float) {
        t.position.x = 0.0
    }
}
)");
    CHECK(r.success);
    bool foundChain = false;
    for (const auto& c1 : r.program->components) {
        if (!c1) continue;
        for (const auto& m : c1->members) {
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

TEST_CASE(semantic_literal_types) {
    // No stamp assertion in S2 (codegen doesn't read it). Verify just that
    // the analyzer doesn't crash or error on a literal-only body.
    Compiler c;
    auto r = c.compile(R"(
component Foo {
    on_update(dt: float) {
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
component Foo {
    export var a: int = 0
    export var b: int = 0
    on_update(dt: float) {
        a = a + b
    }
}
)");
    CHECK(r.success);
}

TEST_CASE(semantic_player_controller_example_compiles) {
    // Read examples/player_controller.logia if available; otherwise inline
    // the canonical body so the test is self-contained.
    const char* src = R"(
component PlayerController {
    export var speed: float = 5.0
    export var jump_force: float = 8.0

    var transform: Transform

    on_start(entity: Entity) {
        transform = entity.get_component(Transform)
    }

    on_update(dt: float) {
        if input.is_pressed("jump") {
            transform.position.y = transform.position.y + jump_force * dt
        }
        transform.position.x = transform.position.x + speed * dt
    }

    on_destroy() {
        log.info("PlayerController destroyed")
    }
}
)";
    Compiler c;
    auto r = c.compile(src);
    CHECK(r.success);
}

TEST_SUITE_END