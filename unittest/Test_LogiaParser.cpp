// Logia parser unit tests (S2.5)
//
// Verifies that the parser produces a correct ScriptDecl tree for the
// S2.5 syntax: `script Name { ... }`, `var` (no `export`), lifecycle
// functions with no parameters, `self.field` member access.

#include "AYScript.h"
#include "AYTest.h"

using namespace ayt::script::logia;

namespace {

const char* kPlayerController = R"(
script PlayerController {
    var tick_counter: int = 0

    on_start() {
        tick_counter = 0
    }

    on_update(dt: float) {
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

VarDeclStmt* findVarDecl(const ScriptDecl& script, const char* name)
{
    for (const auto& member : script.members) {
        if (auto* varDecl = dynamic_cast<VarDeclStmt*>(member.get())) {
            if (varDecl->name == name) return varDecl;
        }
    }
    return nullptr;
}

LifecycleFuncDecl* findLifecycle(const ScriptDecl& script, LifecycleKind kind)
{
    for (const auto& member : script.members) {
        if (auto* lifecycle = dynamic_cast<LifecycleFuncDecl*>(member.get())) {
            if (lifecycle->kind == kind) return lifecycle;
        }
    }
    return nullptr;
}

} // namespace

TEST_SUITE(LogiaParserTests)

TEST_CASE(parse_player_controller) {
    Compiler compiler;
    const CompileResult result = compiler.compile(kPlayerController);

    CHECK(result.success);
    CHECK(result.errors.empty());
    CHECK(result.program != nullptr);
    CHECK(result.program->scripts.size() == 1u);

    const ScriptDecl& script = *result.program->scripts[0];
    CHECK(script.name == "PlayerController");

    VarDeclStmt* tick = findVarDecl(script, "tick_counter");
    CHECK(tick != nullptr);
    CHECK(tick->typeName == "int");

    LifecycleFuncDecl* onStart = findLifecycle(script, LifecycleKind::OnStart);
    CHECK(onStart != nullptr);
    CHECK(onStart->params.empty());  // S2.5: no params
    CHECK_FALSE(onStart->body.empty());

    LifecycleFuncDecl* onUpdate = findLifecycle(script, LifecycleKind::OnUpdate);
    CHECK(onUpdate != nullptr);
    // S2.5: source still shows `on_update(dt: float)` for clarity
    // (semantic analyzer soft-warns), but the parser records the
    // legacy param. The codegen ignores params and emits `(self)`.
    CHECK(onUpdate->params.size() == 1u);
    CHECK(onUpdate->params[0].name == "dt");

    LifecycleFuncDecl* onDestroy = findLifecycle(script, LifecycleKind::OnDestroy);
    CHECK(onDestroy != nullptr);
    CHECK(onDestroy->params.empty());
}

TEST_CASE(parse_if_without_parens) {
    const char* source = R"(
script T {
    on_update(dt: float) {
        if flag {
            x = 1
        }
    }
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK(result.success);

    const LifecycleFuncDecl* onUpdate =
        findLifecycle(*result.program->scripts[0], LifecycleKind::OnUpdate);
    CHECK(onUpdate != nullptr);
    CHECK(onUpdate->body.size() == 1u);

    const auto* ifStmt = dynamic_cast<const IfStmt*>(onUpdate->body[0].get());
    CHECK(ifStmt != nullptr);
    CHECK(ifStmt->condition != nullptr);
    CHECK_FALSE(ifStmt->thenBranch.empty());
}

TEST_CASE(parse_r52a_do_block) {
    // R5.2-A (2026-07-13): `do { <stmts> } end` parses to a BlockStmt
    // whose body is a vector with the expected size. Mirrors
    // parse_if_without_parens — the parser's responsibility is the
    // AST shape; codegen + runtime semantics are exercised by the
    // LogiaReflectRuntime suite.
    const char* source = R"(
script T {
    on_start() {
        do {
            var a: int = 1
            a = a + 1
        } end
    }
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK(result.success);
    CHECK(result.errors.empty());

    const LifecycleFuncDecl* onStart =
        findLifecycle(*result.program->scripts[0], LifecycleKind::OnStart);
    CHECK(onStart != nullptr);
    CHECK(onStart->body.size() == 1u);

    const auto* bs = dynamic_cast<const BlockStmt*>(onStart->body[0].get());
    CHECK(bs != nullptr);
    CHECK(bs->body.size() == 2u);
}

TEST_CASE(parse_r52a_do_block_at_top_level_rejected) {
    // R5.2-A: `do { ... } end` is a STATEMENT, valid only where
    // statements are allowed (inside lifecycle bodies / function
    // bodies / control-flow branches). A bare `do` at script-block
    // scope is rejected — parseMember has no `do` dispatch, so a
    // bare `do` keyword falls through to the "Expected script
    // member" error path.
    const char* source = R"(
script T {
    do { var a: int = 1 } end
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK_FALSE(result.success);
    CHECK_FALSE(result.errors.empty());
}

TEST_SUITE_END