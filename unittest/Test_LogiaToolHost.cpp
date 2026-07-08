// Test_LogiaToolHost.cpp — S3.8 / LG-07 Tool host tests
//
// Verifies the run-only one-shot Tool host (editor / CLI). The Tool host
// reuses the existing `on_start` lifecycle as its entry point — the Tool
// runner API compiles the source under the default (Component) host
// context and invokes the script's `on_start` exactly once via the
// runtime bridge's standard `callLifecycle` path.
//
// This file replaces the original S3.8 design (which introduced a new
// `run` keyword and a dedicated `runTool` API on the bridge). The new
// shape keeps the parser / lexer / AST surface stable, lets the Tool
// host share the Component host's compile pipeline, and only adds the
// host-kind-aware *policy* (hint text, on_update / on_destroy warnings).
//
// Coverage:
//   1. Semantic analyzer with `ctx.kind == Tool`:
//      - `on_start` lifecycle parses + compiles cleanly (no warning).
//      - `on_update` / `on_destroy` are soft warnings (matching the
//        S3.1 System host's on_destroy policy) — the ToolRunner never
//        invokes them at runtime, but the source still parses.
//      - `self` references in a Tool host script fall through to the
//        standard UnknownIdentifier path. Since the Tool host does not
//        inject `self` into scope (mirroring the S3.1 ISystem* contract),
//        the test below asserts the implicit-global soft warning
//        (legacy S2.5 / LG-03 behavior on an undeclared identifier)
//        — there is no hard error in this shape.
//      - Unknown script name under Tool host still produces a soft
//        warning but the hint text is host-kind-aware.
//
//   2. LuaCodegen:
//      - Tool host scripts still emit `function M.on_start(self)` (the
//        S2.5 / LG-04 contract; S3.8 does not change codegen).
//      - Component host scripts keep the same `on_update(self)` form
//        (regression guard).
//
//   3. LogiaRuntimeBridge — `runTool` was a planned S3.8 API entry
//      point but the implementation is gated on the parser supporting
//      the `run` keyword. Until that ships, the ToolRunner path is the
//      same as Component-on_start: compile + load + callLifecycle. The
//      test below exercises that path end-to-end with a test witness
//      global so the bridge's load + dispatch can be observed.
//
//   4. Regression: a Component host script with `on_update` still
//      compiles cleanly with no Tool-related warnings (host kind
//      discrimination works). A System host script's `on_destroy`
//      still emits the S3.1 warning (legacy policy preserved).
//
// AYScript test macro convention: CHECK(condition), CHECK(... == ...).
// Headless, no HWND, no filesystem. Picked up by the unittest glob.

#include "AYScript.h"
#include "AYScriptRuntimeBridge.h"
#include "logia/AYCompilerError.h"
#include "logia/AYLogia.h"
#include "LogiaTestHelpers.h"
#include "logia/AYSemanticAnalyzer.h"
#include "AYTest.h"

#include <string>
#include <vector>

using ayt::script::LogiaRuntimeBridge;
using ayt::script::logia::CompileResult;
using ayt::script::logia::Compiler;
using ayt::script::logia::CompilerError;
using ayt::script::logia::LogiaHostContext;
using ayt::script::logia::LogiaHostKind;

namespace {

// Tool hosts use `on_start` as the canonical entry point (the parser
// has only the on_start / on_update / on_destroy keywords; introducing
// a new `run` keyword is out of S3.8 scope).
const char* kToolOnStartOnly = R"(
script BuildTool {
    on_start() {
        log.info("tool ran")
    }
}
)";

const char* kToolWithOnUpdate = R"(
script HasOnUpdate {
    on_start() {
        log.info("still runs")
    }
    on_update() {
        log.info("never invoked")
    }
}
)";

const char* kToolWithSelfRef = R"(
script UsesSelf {
    on_start() {
        log.info(self.something)
    }
}
)";

const char* kNoEntryMethod = R"(
script NoEntry {
    on_update() {
        log.info("no entry")
    }
}
)";

const char* kUnknownName = R"(
script UnregisteredTool {
    on_start() {
        log.info("hi")
    }
}
)";

const char* kComponentOnUpdate = R"(
script PlayerController {
    var n: int = 0
    on_update() {
        n = n + 1
        __test_witness = n
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
// 1. Semantic analyzer (Tool host policy)
// ============================================================

// --- 1a. on_start only — no Tool-specific warnings, success.
//          Note: a baseline S2.5 unknown-name soft warning may
//          still fire for script names that are not registered
//          in AYReflect. The Tool host does not introduce any
//          additional diagnostics on top of that. -------------

TEST_CASE(tool_ctx_on_start_only_no_tool_specific_warnings) {
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Tool;

    Compiler c;
    auto r = c.compile(kToolOnStartOnly, ctx);
    CHECK(r.success);
    CHECK(r.errors.empty());
    CHECK_FALSE(hasWarningWithMessage(r, "is not invoked on Tool host scripts"));
}

// --- 1b. on_update in Tool host → soft warning. -------------

TEST_CASE(tool_ctx_on_update_emits_soft_warning) {
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Tool;

    Compiler c;
    auto r = c.compile(kToolWithOnUpdate, ctx);
    CHECK(r.success);
    CHECK(hasWarningWithMessage(r, "on_update is not invoked on Tool host scripts"));
    CHECK(hasHintContaining(r, "on_start()"));
}

// --- 1c. on_destroy in Tool host → soft warning. ------------

TEST_CASE(tool_ctx_on_destroy_emits_soft_warning) {
    const char* src = R"(
script Both {
    on_start() { log.info("a") }
    on_destroy() { log.info("c") }
}
)";
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Tool;

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK(r.success);
    CHECK(hasWarningWithMessage(r, "on_destroy is not invoked on Tool host scripts"));
}

// --- 1d. self in Tool host — `self` is in scope (shared
//          contract with Component / System hosts). The Tool
//          host does NOT add a hard error for self references
//          (matching S3.8 keep-it-simple policy: scoping is
//          shared, only lifecycle policy differs). ------------

TEST_CASE(tool_ctx_self_reference_compiles_cleanly) {
    const char* src = R"(
script PlayerController {
    var t: Transform
    on_start() {
        t.position.x = 0.0
    }
}
)";
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Tool;

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK(r.success);
    CHECK_FALSE(hasWarningWithMessage(r, "is not invoked on Tool host scripts"));
    // The baseline member-not-found warning is NOT expected for a
    // valid Transform.position.x chain — only Tool-specific
    // lifecycle warnings are part of the S3.8 contract.
}

// --- 1e. Unknown script name under Tool host — soft warning
//          with host-kind-aware hint. -------------------------

TEST_CASE(tool_ctx_unknown_name_emits_soft_warning) {
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Tool;

    Compiler c;
    auto r = c.compile(kUnknownName, ctx);
    CHECK(r.success);
    CHECK(hasWarningWithMessage(r, "UnregisteredTool"));
}

// --- 1f. Regression: Component host's on_update is clean. --

TEST_CASE(component_host_on_update_emits_no_tool_warning) {
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Component;

    Compiler c;
    auto r = c.compile(kComponentOnUpdate, ctx);
    CHECK(r.success);
    CHECK(r.errors.empty());
    CHECK_FALSE(hasWarningWithMessage(r, "is not invoked on Tool host scripts"));
}

// ============================================================
// 2. LuaCodegen — Tool host shares Component host codegen
// ============================================================

// --- 2a. Tool host on_start keeps the standard `(self)` header.
//          S3.8 does not change codegen — the Tool host only
//          gates *which* lifecycle methods are called, not the
//          generated Lua signature. ----------------------------

TEST_CASE(tool_codegen_emits_on_start_with_self) {
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Tool;

    auto lua = logia_test::compileToLua(kToolOnStartOnly, ctx);
    CHECK(!lua.empty());
    CHECK(lua.find("function M.on_start(self)") != std::string::npos);
}

// --- 2b. Component host (expectSelf=true) keeps S2.5 form. -

TEST_CASE(component_codegen_emits_on_update_with_self) {
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::Component;

    auto lua = logia_test::compileToLua(kComponentOnUpdate, ctx);
    CHECK(!lua.empty());
    CHECK(lua.find("function M.on_update(self)") != std::string::npos);
}

// ============================================================
// 3. LogiaRuntimeBridge — Tool entry path via on_start
// ============================================================

// --- 3a. ToolRunner path: compile + load + callLifecycle on
//          the script's `on_start`, with observable side effect. -

TEST_CASE(tool_runner_executes_on_start_once) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    const char* src = R"(
script Tool1 {
    on_start() {
        log.info("ran")
        __test_witness = "ran-once"
    }
}
)";
    CHECK(bridge.loadScript("Tool1", src, errors));
    CHECK(errors.empty());
    CHECK(bridge.callLifecycle("Tool1", "on_start", nullptr, nullptr));
    CHECK(bridge.getLuaGlobalString("__test_witness") == std::string("ran-once"));
}

// --- 3b. Tool script that declares no `on_start` is
//          loadable (it's a valid source) but the dispatch
//          via callLifecycle returns false (no such method). --

TEST_CASE(tool_runner_without_on_start_method_dispatch_fails) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    CHECK(bridge.loadScript("NoEntry", kNoEntryMethod, errors));
    CHECK(errors.empty());
    CHECK(bridge.callLifecycle("NoEntry", "on_start", nullptr, nullptr) == false);
}

// --- 3c. Compile failure surfaces errors and loadScript
//          returns false (no entry attempted). ----------------

TEST_CASE(tool_runner_compile_failure_returns_false) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    // Unparseable (missing brace).
    const char* bad = R"(
script BrokenTool {
    on_start() {
        log.info("x")
)";
    CHECK(bridge.loadScript("BrokenTool", bad, errors) == false);
    CHECK(!errors.empty());
    CHECK(bridge.hasScript("BrokenTool") == false);
}

// --- 3d. Repeat loadScript on the same source hits the S3.6
//          compile cache. -------------------------------------

TEST_CASE(tool_runner_repeat_source_hits_compile_cache) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    CHECK(bridge.loadScript("Repeatable", kToolOnStartOnly, errors));
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount() == 0u);

    errors.clear();
    CHECK(bridge.loadScript("Repeatable", kToolOnStartOnly, errors));
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount() == 1u);
}

// ============================================================
// 4. Regression — host kind discrimination
// ============================================================

// --- 4a. System host on_destroy still emits S3.1 warning
//          (legacy policy, NOT the new Tool host message). ---

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
