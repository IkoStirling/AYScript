// Logia parser unit tests (S2.5)
//
// Verifies that the parser produces a correct ScriptDecl tree for the
// S2.5 syntax: `script Name { ... }`, `var` (no `export`), lifecycle
// functions with no parameters, `self.field` member access.

#include "AYScript.h"
#include "AYScript/logia/Parser.h"
#include "AYScript/logia/Lexer.h"
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
    // Lifecycle parameters are retained so semantic analysis can scope
    // them and codegen can emit `(self, dt)` for the runtime bridge.
    CHECK(onUpdate->params.size() == 1u);
    CHECK(onUpdate->params[0].name == "dt");

    LifecycleFuncDecl* onDestroy = findLifecycle(script, LifecycleKind::OnDestroy);
    CHECK(onDestroy != nullptr);
    CHECK(onDestroy->params.empty());
}

TEST_CASE(parse_if_without_parens) {
    // R5.2-H (2026-07-14): the original `if flag { x = 1 }`
    // used a bare undeclared identifier as the condition —
    // pre-R5.2-H Lua's truthiness made this compile and run
    // (with implicit-global warning). R5.2-H now requires
    // conditions to statically reduce to bool, so an
    // undeclared `flag` is a hard error. The test's intent
    // is to validate the **AST shape** of an `if` statement
    // without parentheses — the parens-less form is a
    // R5.0.1 / R5.2-A parser feature, not a sema feature.
    // We rewrite the source to use a bool-typed local so
    // R5.2-H accepts it; the AST-shape checks (condition
    // populated, thenBranch non-empty) remain unchanged.
    const char* source = R"(
script T {
    on_update(dt: float) {
        var flag: bool = true
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
    CHECK(onUpdate->body.size() == 2u);

    const auto* ifStmt = dynamic_cast<const IfStmt*>(onUpdate->body[1].get());
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

// ============================================================
// R5.2-B (2026-07-14): label declarations, break :L, continue :L
// ============================================================

namespace {

// Walk a `body` vector and return the first node whose dynamic type
// matches T. Returns nullptr if not found. Used by the R5.2-B
// parser tests to fetch a BreakStmt / ContinueStmt / LabelDeclStmt
// out of an arbitrary body vector without writing per-test
// dynamic_cast loops.
template <typename T>
T* findFirstInBody(const std::vector<StmtPtr>& body)
{
    for (const auto& s : body) {
        if (auto* p = dynamic_cast<T*>(s.get())) return p;
    }
    return nullptr;
}

} // namespace

TEST_CASE(parse_r52b_label_decl) {
    // R5.2-B: `::FOUND::` parses to a LabelDeclStmt with name
    // "FOUND". Mirrors the R5.2-A parser-test pattern.
    const char* source = R"(
script T {
    on_start() {
        for (var i : 1) {
            ::FOUND::
            break
        }
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

    const auto* fs = dynamic_cast<const ForStmt*>(onStart->body[0].get());
    CHECK(fs != nullptr);

    const auto* ld = findFirstInBody<LabelDeclStmt>(fs->body);
    CHECK(ld != nullptr);
    CHECK(ld->name == "FOUND");
}

TEST_CASE(parse_r52b_break_with_label) {
    // R5.2-B: `break :FOUND` produces a BreakStmt with the label
    // field set to "FOUND".
    const char* source = R"(
script T {
    on_start() {
        for (var i : 1) {
            ::FOUND::
            break :FOUND
        }
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

    const auto* fs = dynamic_cast<const ForStmt*>(onStart->body[0].get());
    CHECK(fs != nullptr);

    const auto* bs = findFirstInBody<BreakStmt>(fs->body);
    CHECK(bs != nullptr);
    CHECK(bs->label == "FOUND");
}

TEST_CASE(parse_r52b_continue_with_label_rejected) {
    // R5.2-B (2026-07-14): `continue :L` is NOT supported in this
    // slice. The parser rejects it with a hard error. Only
    // `break :L` and bare `continue` are valid.
    const char* source = R"(
script T {
    on_start() {
        for (var i : 1) {
            continue :FOUND
        }
    }
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK_FALSE(result.success);
}

TEST_CASE(parse_r52b_undefined_label_is_error) {
    // R5.2-B: `break :NEVERDECLARED` references a label that is
    // not declared in any enclosing loop. The analyzer emits a
    // hard error. Verify compile fails and the message names the
    // missing label.
    const char* source = R"(
script T {
    on_start() {
        for (var i : 1) {
            break :NEVERDECLARED
        }
    }
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK_FALSE(result.success);
    bool found = false;
    for (const auto& e : result.errors) {
        if (e.message.find("NEVERDECLARED") != std::string::npos) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE(parse_r52b_label_outside_loop_is_error) {
    // R5.2-B: `::FOO::` at function-body scope (not inside any
    // loop) is a hard error per the loop-scoped rule. The
    // analyzer's "_labelStack.size() == 1" check (size 1 = the
    // outer function-body frame) catches it.
    const char* source = R"(
script T {
    on_start() {
        ::FOO::
    }
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK_FALSE(result.success);
}

TEST_CASE(parse_r52b_duplicate_label_is_error) {
    // R5.2-B: two `::FOUND::` in the same for-body is a hard
    // error on the second declaration.
    const char* source = R"(
script T {
    on_start() {
        for (var i : 1) {
            ::FOUND::
            ::FOUND::
        }
    }
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK_FALSE(result.success);
    bool found = false;
    for (const auto& e : result.errors) {
        if (e.message.find("duplicate") != std::string::npos) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

// S4.1 (2026-07-15): per-component signal declarations are accepted
// at script-block scope. Companion call sites `emit(...)` and
// `connect(...)` are ordinary CallExprs — the parser does NOT need
// to know they are special; analyzer shape-recognition does that.
// These tests pin the parser-side surface.

TEST_CASE(s41_parse_signal_zero_params) {
    const char* source = R"(
script PlayerController {
    signal died()
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    // S4.1 surface is parser-side accept-only. Semantic validation
    // (signal name resolution, type checks) is the analyzer's job in
    // a later commit; the parser here just has to produce a
    // SignalDeclStmt with the right name and empty param list.
    CHECK(result.success);
    CHECK(result.program && result.program->scripts.size() == 1u);
    const auto& members = result.program->scripts[0]->members;
    CHECK(members.size() == 1u);
    auto* sig = dynamic_cast<SignalDeclStmt*>(members[0].get());
    CHECK(sig != nullptr);
    CHECK(sig->name == "died");
    CHECK(sig->params.empty());
    // S5 ED-02: sourceLoc stamps at the signal name position.
    CHECK(sig->sourceLoc.line >= 2);
}

TEST_CASE(s41_parse_signal_with_typed_params) {
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int, source: string)
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK(result.success);
    const auto& members = result.program->scripts[0]->members;
    CHECK(members.size() == 1u);
    auto* sig = dynamic_cast<SignalDeclStmt*>(members[0].get());
    CHECK(sig != nullptr);
    CHECK(sig->name == "damaged");
    CHECK(sig->params.size() == 2u);
    CHECK(sig->params[0].name == "amount");
    CHECK(sig->params[0].typeName == "int");
    CHECK(sig->params[1].name == "source");
    CHECK(sig->params[1].typeName == "string");
}

TEST_CASE(s41_parse_multiple_signal_decls) {
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    signal died()
    signal healed(amount: int)
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK(result.success);
    const auto& members = result.program->scripts[0]->members;
    CHECK(members.size() == 3u);
    CHECK(dynamic_cast<SignalDeclStmt*>(members[0].get()) != nullptr);
    CHECK(dynamic_cast<SignalDeclStmt*>(members[1].get()) != nullptr);
    CHECK(dynamic_cast<SignalDeclStmt*>(members[2].get()) != nullptr);
}

TEST_CASE(s41_parse_signal_optional_trailing_semicolon) {
    // Mirrors `var x: int = 5;` style — trailing `;` is optional.
    const char* source = R"(
script P {
    signal hit();
    signal heal(amount: int);
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK(result.success);
    const auto& members = result.program->scripts[0]->members;
    CHECK(members.size() == 2u);
    CHECK(dynamic_cast<SignalDeclStmt*>(members[0].get()) != nullptr);
    CHECK(dynamic_cast<SignalDeclStmt*>(members[1].get()) != nullptr);
}

TEST_CASE(s41_parse_signal_in_lifecycle_body_is_error) {
    // Same restriction as `function NAME(...)` inside a lifecycle
    // body — the gate in parseStatement rejects this with a
    // tailored message.
    const char* source = R"(
script T {
    on_start() {
        signal doomed()
    }
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK_FALSE(result.success);
    bool found = false;
    for (const auto& e : result.errors) {
        if (e.message.find("signal declarations only allowed as script members")
            != std::string::npos) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE(s41_parse_signal_emit_and_connect_are_call_exprs) {
    // `emit` and `connect` are NOT lexer-reserved; the parser treats
    // them as ordinary Identifier callees inside a CallExpr. The
    // analyzer stamps `ambientCall` on them in commit 4, but
    // the AST shape here is a regular CallExpr(IdentifierExpr("emit"),
    // [...]).
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    on_start() {
        connect("damaged", on_damaged)
    }
    on_update(dt: float) {
        if input.is_pressed("hit") {
            emit("damaged", 10)
        }
    }
    function on_damaged(amount: int) {
        log.info("hit")
    }
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK(result.success);
    CHECK(result.program && result.program->scripts.size() == 1u);
    // Find the on_update body and look for the emit CallExpr. Pin
    // that the callee is an IdentifierExpr named "emit" (NOT
    // member-of-self — analyzer shape comes later).
    const auto& members = result.program->scripts[0]->members;
    LifecycleFuncDecl* updateFn = nullptr;
    for (const auto& m : members) {
        auto* lc = dynamic_cast<LifecycleFuncDecl*>(m.get());
        if (lc && lc->kind == LifecycleKind::OnUpdate) {
            updateFn = lc;
            break;
        }
    }
    CHECK(updateFn != nullptr);
    CHECK_FALSE(updateFn->body.empty());
    auto* ifStmt = dynamic_cast<IfStmt*>(updateFn->body[0].get());
    CHECK(ifStmt != nullptr);
    CHECK_FALSE(ifStmt->thenBranch.empty());
    auto* emitStmt = dynamic_cast<ExprStmt*>(ifStmt->thenBranch[0].get());
    CHECK(emitStmt != nullptr);
    auto* call = dynamic_cast<CallExpr*>(emitStmt->expr.get());
    CHECK(call != nullptr);
    auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
    CHECK(calleeId != nullptr);
    CHECK(calleeId->name == "emit");
    CHECK(call->args.size() == 2u);
}

// S4.1b (2026-07-18): pin that `disconnect(id)` is parsed as a
// plain `CallExpr` whose callee is `IdentifierExpr("disconnect")`
// with exactly 1 argument — same shape-recognition pattern as
// `emit` / `connect`. The analyzer stamps `ambientCall =
// AmbientCallKind::Disconnect`; codegen lowers to
// `__ay_disconnect(self, id)`.
TEST_CASE(s41b_parse_disconnect_is_call_expr) {
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    var hit_handler: int = 0
    on_destroy() {
        disconnect(hit_handler)
    }
    function on_damaged(amount: int) { }
}
)";
    std::vector<Token> tokens;
    tokenize(source, tokens);
    ayt::script::logia::Parser parser(tokens);
    auto program = parser.parse();
    CHECK(program != nullptr);
    CHECK(!parser.hasErrors());
    CHECK(program->scripts.size() == 1u);
    // members[0] = signal, [1] = var, [2] = on_destroy, [3] = function
    auto* onDestroy = dynamic_cast<LifecycleFuncDecl*>(
        program->scripts[0]->members[2].get());
    CHECK(onDestroy != nullptr);
    CHECK(onDestroy->body.size() >= 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(onDestroy->body[0].get());
    CHECK(exprStmt != nullptr);
    auto* call = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    CHECK(call != nullptr);
    auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
    CHECK(calleeId != nullptr);
    CHECK(calleeId->name == "disconnect");
    CHECK(call->args.size() == 1u);
    // The id arg should be an IdentifierExpr referencing the
    // `var hit_handler` declared above.
    auto* idArg = dynamic_cast<IdentifierExpr*>(call->args[0].get());
    CHECK(idArg != nullptr);
    CHECK(idArg->name == "hit_handler");
}

TEST_SUITE_END
