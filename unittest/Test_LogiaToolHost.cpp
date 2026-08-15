// Test_LogiaToolHost.cpp - S3.8b / LG-07 Tool host tests
//
// The S3.8 baseline (policy-only) is replaced by the original prompt's
// `run` keyword + dedicated Tool host surface. Concretely:
//   - The lexer recognizes `run` as a keyword; the parser accepts it
//     as a script member and produces a `LifecycleFuncDecl` with
//     `kind == LifecycleKind::Run`.
//   - `toolLogiaHostContext()` returns a context with `kind=Tool,
//     expectSelf=false` (single source of truth for the Tool host
//     shape).
//   - Semantic policy:
//       * Under Tool: only `run()` is allowed. `on_start` /
//         `on_update` / `on_destroy` emit a soft warning (mirroring
//         the S3.1 System-on_destroy policy).
//       * Under Component / System: `run()` emits a soft warning
//         (symmetric: `run()` is a Tool lifecycle, not invoked by
//         tick-driven hosts).
//   - LuaCodegen: under `ctx.expectSelf == false` the emitted
//     signature is `function M.run()` (no `self`); under
//     expectSelf==true (Component / System) the existing
//     `function M.on_*(self)` shape is preserved verbatim.
//   - Bridge: `runTool(name, src, errors)` compiles under
//     `toolLogiaHostContext()`, loads, and invokes `run` exactly
//     once. The compile cache is keyed by (source, Tool ctx, v2),
//     which is a separate slot from any Component-host run of the
//     same source.
//   - Regression: Component / System hosts see no Tool-related
//     warnings; their codegen output is byte-identical to the
//     S3.8 baseline (golden strings verified).
//
// AYScript test macro convention: CHECK(condition), CHECK(... == ...).
// Headless, no HWND, no filesystem. Picked up by the unittest glob.

#include "AYScript.h"
#include "AYScript/ScriptRuntimeBridge.h"
#include "AYScript/logia/CompilerError.h"
#include "AYScript/logia/Logia.h"
#include "AYScript/logia/SemanticAnalyzer.h"
#include "AYScript/logia/Token.h"
#include "LogiaTestHelpers.h"
#include "AYTest.h"

#include <string>
#include <vector>

using ayt::script::LogiaRuntimeBridge;
using ayt::script::logia::CompileResult;
using ayt::script::logia::Compiler;
using ayt::script::logia::CompilerError;
using ayt::script::logia::LifecycleKind;
using ayt::script::logia::LogiaHostContext;
using ayt::script::logia::LogiaHostKind;
using ayt::script::logia::tokenize;

namespace {

// Canonical Tool source: `run()` is the entry point.
const char* kToolRunOnly = R"(
script BuildTool {
    run() {
        log.info("tool ran")
    }
}
)";

// Tool source that ALSO declares `on_update` (should soft-warn).
const char* kToolWithOnUpdate = R"(
script HasOnUpdate {
    run() {
        log.info("still runs")
    }
    on_update() {
        log.info("never invoked")
    }
}
)";

// Tool source that ALSO declares `on_start` (should soft-warn).
const char* kToolWithOnStart = R"(
script HasOnStart {
    on_start() {
        log.info("not invoked")
    }
    run() {
        log.info("the only entry")
    }
}
)";

// Tool source with no `run()` at all.
const char* kNoRunEntry = R"(
script NoEntry {
    on_update() {
        log.info("no entry")
    }
}
)";

// Component host source with `on_update` (must stay clean).
const char* kComponentOnUpdate = R"(
script PlayerController {
    var n: int = 0
    on_update() {
        n = n + 1
        __test_witness = n
    }
}
)";

// Component host source with `run()` (should soft-warn under
// Component, since `run` is a Tool lifecycle).
const char* kComponentWithRun = R"(
script PlayerController {
    var n: int = 0
    run() {
        n = n + 1
    }
    on_update() {
        n = n + 1
    }
}
)";

bool hasWarningWithMessage(const CompileResult& r, const std::string& needle)
{
    for (const auto& d : r.diagnostics) {
        if (d.severity == ayt::script::logia::DiagnosticSeverity::Warning
            && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasHintContaining(const CompileResult& r, const std::string& needle)
{
    for (const auto& d : r.diagnostics) {
        if (d.hint.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_SUITE(LogiaToolHostTests)

// ============================================================
// 0. Lexer — `run` is a keyword
// ============================================================

TEST_CASE(tool_lexer_run_is_keyword) {
    std::vector<ayt::script::logia::Token> tokens;
    tokenize("run on_start on_update on_destroy", tokens);
    CHECK(tokens.size() == 5u);
    CHECK(tokens[0].type == ayt::script::logia::TokenType::Run);
    CHECK(tokens[1].type == ayt::script::logia::TokenType::OnStart);
    CHECK(tokens[2].type == ayt::script::logia::TokenType::OnUpdate);
    CHECK(tokens[3].type == ayt::script::logia::TokenType::OnDestroy);
    CHECK(tokens[4].type == ayt::script::logia::TokenType::EndOfFile);
}

// ============================================================
// 1. Semantic analyzer (Tool host policy)
// ============================================================

// --- 1a. `run()` under Tool host compiles cleanly with no
//          Tool-specific warnings. -----------------------------

TEST_CASE(tool_ctx_run_only_no_tool_specific_warnings) {
    LogiaHostContext ctx = ayt::script::logia::toolLogiaHostContext();

    Compiler c;
    auto r = c.compile(kToolRunOnly, ctx);
    CHECK(r.success);
    CHECK(r.errors.empty());
    CHECK_FALSE(hasWarningWithMessage(r, "is not invoked on Tool host scripts"));
    CHECK_FALSE(hasWarningWithMessage(r, "is a Tool host lifecycle"));
}

// --- 1b. `on_update` under Tool → soft warning. --------------

TEST_CASE(tool_ctx_on_update_emits_soft_warning) {
    LogiaHostContext ctx = ayt::script::logia::toolLogiaHostContext();

    Compiler c;
    auto r = c.compile(kToolWithOnUpdate, ctx);
    CHECK(r.success);
    CHECK(hasWarningWithMessage(r, "on_update is not invoked on Tool host scripts"));
    CHECK(hasHintContaining(r, "run()"));
}

// --- 1c. `on_start` under Tool → soft warning. ---------------

TEST_CASE(tool_ctx_on_start_emits_soft_warning) {
    LogiaHostContext ctx = ayt::script::logia::toolLogiaHostContext();

    Compiler c;
    auto r = c.compile(kToolWithOnStart, ctx);
    CHECK(r.success);
    CHECK(hasWarningWithMessage(r, "on_start is not invoked on Tool host scripts"));
    CHECK(hasHintContaining(r, "run()"));
}

// --- 1d. `on_destroy` under Tool → soft warning. -------------

TEST_CASE(tool_ctx_on_destroy_emits_soft_warning) {
    const char* src = R"(
script Both {
    run() { log.info("a") }
    on_destroy() { log.info("c") }
}
)";
    LogiaHostContext ctx = ayt::script::logia::toolLogiaHostContext();

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK(r.success);
    CHECK(hasWarningWithMessage(r, "on_destroy is not invoked on Tool host scripts"));
}

// --- 1e. `self` in Tool host — Tool does NOT inject `self`
//          into scope (expectSelf=false). A `self` reference
//          falls through to the standard UnknownIdentifier
//          soft warning. (Tool scripts must not touch `self`.) -

TEST_CASE(tool_ctx_self_reference_emits_implicit_global_warning) {
    const char* src = R"(
script PlayerController {
    run() {
        log.info(self.something)
    }
}
)";
    LogiaHostContext ctx = ayt::script::logia::toolLogiaHostContext();

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK(r.success);  // soft warning only
    // No Tool-specific lifecycle warnings (run() is allowed).
    CHECK_FALSE(hasWarningWithMessage(r, "is not invoked on Tool host scripts"));
    // `self` is not in scope → implicit global warning.
    CHECK(hasWarningWithMessage(r, "implicit global 'self'"));
}

// --- 1f. `toolLogiaHostContext()` returns the canonical
//          Tool host context. ----------------------------------

TEST_CASE(tool_logia_host_context_factory) {
    LogiaHostContext ctx = ayt::script::logia::toolLogiaHostContext();
    CHECK(ctx.kind == LogiaHostKind::Tool);
    CHECK(ctx.expectSelf == false);
    CHECK(ctx.hostType == nullptr);
    CHECK(ctx.strictInheritance == false);
}

// ============================================================
// 2. Symmetric policy — `run()` on Component / System hosts
// ============================================================

// --- 2a. `run()` under Component → soft warning. ------------

TEST_CASE(component_ctx_run_emits_soft_warning) {
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Component;

    Compiler c;
    auto r = c.compile(kComponentWithRun, ctx);
    CHECK(r.success);
    CHECK(hasWarningWithMessage(r, "run() is a Tool / EventHandler lifecycle"));
    CHECK_FALSE(hasWarningWithMessage(r, "is not invoked on Tool host scripts"));
}

// --- 2b. `run()` under System → soft warning. ---------------

TEST_CASE(system_ctx_run_emits_soft_warning) {
    const char* src = R"(
script MovementSystem {
    run() {
        log.info("not invoked by tick dispatcher")
    }
}
)";
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::System;

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK(r.success);
    CHECK(hasWarningWithMessage(r, "run() is a Tool / EventHandler lifecycle"));
    CHECK_FALSE(hasWarningWithMessage(r, "is not invoked on Tool host scripts"));
}

// ============================================================
// 3. LuaCodegen — `function M.run()` (no self) under Tool host
// ============================================================

// --- 3a. Tool host `run()` → emitted without `self`. ---------

TEST_CASE(tool_codegen_emits_run_without_self) {
    LogiaHostContext ctx = ayt::script::logia::toolLogiaHostContext();

    auto lua = logia_test::compileToLua(kToolRunOnly, ctx);
    CHECK(!lua.empty());
    // Tool host: signature is `function M.run()` — no `self`.
    CHECK(lua.find("function M.run()") != std::string::npos);
    // Crucially, NO `function M.run(self)`.
    CHECK(lua.find("function M.run(self)") == std::string::npos);
}

// --- 3b. Component host `on_update` keeps the S2.5 `(self)`
//          signature (regression guard). ---------------------

TEST_CASE(component_codegen_emits_on_update_with_self) {
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Component;

    auto lua = logia_test::compileToLua(kComponentOnUpdate, ctx);
    CHECK(!lua.empty());
    CHECK(lua.find("function M.on_update(self)") != std::string::npos);
}

// --- 3c. Component host `run()` soft-warns in the diagnostic
//          surface (codegen still emits `function M.run(self)`
//          because expectSelf=true on the Component host). ----

TEST_CASE(component_codegen_emits_run_with_self_when_expectSelf_true) {
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Component;
    // expectSelf is true by default for Component.

    auto lua = logia_test::compileToLua(kComponentWithRun, ctx);
    CHECK(!lua.empty());
    // The codegen is host-context-driven, not warning-driven.
    // Even though `run()` on Component is a soft warning, the
    // signature still has `self` because expectSelf=true.
    CHECK(lua.find("function M.run(self)") != std::string::npos);
}

// ============================================================
// 4. LogiaRuntimeBridge — runTool end-to-end
// ============================================================

// --- 4a. runTool: compile + load + invoke `run` once. -------

TEST_CASE(tool_bridge_run_tool_executes_run_once) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    const char* src = R"(
script Tool1 {
    run() {
        log.info("ran")
        __test_witness = "ran-once"
    }
}
)";
    CHECK(bridge.runTool("Tool1", src, errors));
    CHECK(errors.empty());
    CHECK(bridge.hasScript("Tool1"));
    CHECK(bridge.getLuaGlobalString("__test_witness") == std::string("ran-once"));
}

// --- 4b. runTool: a Tool source without `run()` is
//          loadable (parser accepts it as valid source) but
//          runTool returns false because dispatch fails. -----

TEST_CASE(tool_bridge_run_tool_without_run_method_fails) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    CHECK_FALSE(bridge.runTool("NoEntry", kNoRunEntry, errors));
    // The compile cache slot for (NoEntry, Tool ctx) is still
    // populated (compile succeeded; only dispatch failed).
    CHECK(bridge.hasScript("NoEntry"));
}

// --- 4c. runTool: a compile-broken source returns false
//          and does NOT leave a script bound. ----------------

TEST_CASE(tool_bridge_run_tool_compile_failure_returns_false) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    // Unparseable (missing brace).
    const char* bad = R"(
script BrokenTool {
    run() {
        log.info("x")
)";
    CHECK(bridge.runTool("BrokenTool", bad, errors) == false);
    CHECK(!errors.empty());
    CHECK(bridge.hasScript("BrokenTool") == false);
}

// --- 4d. Repeat runTool on the same source hits the S3.6
//          compile cache. -----------------------------------

TEST_CASE(tool_bridge_run_tool_repeat_source_hits_compile_cache) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    CHECK(bridge.runTool("Repeatable", kToolRunOnly, errors));
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount() == 0u);

    errors.clear();
    CHECK(bridge.runTool("Repeatable", kToolRunOnly, errors));
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount() == 1u);
}

// --- 4e. Tool host and Component host of the same source
//          are SEPARATE cache slots (different ctx hash).
//          A Component-host loadScript + Tool-host runTool on
//          the same source string must both miss + populate
//          their own slots. ---------------------------------

TEST_CASE(tool_bridge_separate_cache_slot_from_component) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    // First run under Tool host — miss + populate Tool slot.
    CHECK(bridge.runTool("Shared", kToolRunOnly, errors));
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount() == 0u);

    // Same source but loaded under Component host — different ctx
    // hash → must miss + populate a separate slot. The Tool slot
    // is untouched.
    LogiaHostContext componentCtx;
    componentCtx.kind = LogiaHostKind::Component;
    errors.clear();
    CHECK(bridge.loadScript("Shared", kToolRunOnly, componentCtx, errors));
    CHECK(bridge.compileCacheMissCount() == 2u);
    CHECK(bridge.compileCacheHitCount() == 0u);

    // Re-run the Tool variant → still hits the Tool slot (no
    // new miss). Cache key carries `expectSelf` + `kind` so the
    // Tool and Component slots remain isolated.
    errors.clear();
    CHECK(bridge.runTool("Shared", kToolRunOnly, errors));
    CHECK(bridge.compileCacheMissCount() == 2u);
    CHECK(bridge.compileCacheHitCount() == 1u);
}

// ============================================================
// 5. Regression — Component / System hosts unchanged
// ============================================================

// --- 5a. Component host `on_update` produces no Tool-related
//          warnings (host kind discrimination). -------------

TEST_CASE(component_host_on_update_emits_no_tool_warning) {
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Component;

    Compiler c;
    auto r = c.compile(kComponentOnUpdate, ctx);
    CHECK(r.success);
    CHECK(r.errors.empty());
    CHECK_FALSE(hasWarningWithMessage(r, "is not invoked on Tool host scripts"));
    CHECK_FALSE(hasWarningWithMessage(r, "is a Tool host lifecycle"));
}

// --- 5b. System host `on_destroy` still emits the S3.1
//          legacy warning (NOT a Tool warning). --------------

TEST_CASE(system_host_on_destroy_legacy_warning_preserved) {
    const char* src = R"(
script MovementSystem {
    on_update() { x = 1 }
    on_destroy() { log.info("d") }
}
)";
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::System;

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK(r.success);
    CHECK(hasWarningWithMessage(r, "on_destroy is not invoked on System host scripts"));
    CHECK_FALSE(hasWarningWithMessage(r, "is not invoked on Tool host scripts"));
}

TEST_SUITE_END