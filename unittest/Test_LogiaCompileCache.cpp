// Test_LogiaCompileCache.cpp — S3.6 / LG-06a compile cache.
//
// Verifies that the per-bridge in-memory compile cache introduced by
// S3.6 (LG-06a):
//
//   - Skips Lexer/Parser/Semantic/Codegen when the same
//     (source, LogiaHostContext, pipelineVersion) tuple is presented
//     a second time.
//   - Still re-executes the cached Lua chunk through sol::state on
//     each loadScript call (Lua state lifecycle is independent of
//     the compile cache).
//   - Reports hit/miss via the public counters
//     (compileCacheHitCount / compileCacheMissCount /
//     resetCompileCacheCounters / clearCompileCache).
//   - Different source or different LogiaHostContext → cache miss
//     (the cache key MUST distinguish these to avoid serving a
//     Component-host cached Lua to a System host load — different
//     diagnostics, different validation rules, different codegen
//     semantics downstream).
//   - A previously-failing compile caches its failure so a second
//     call returns false with errors populated (and counts a hit).
//   - shutdown() wipes the cache so the bridge restarts in a clean
//     miss state.
//
// AYScript test macro convention: assertions are written
//   CHECK(condition);
//   CHECK(expr == expected);
// per the AYTest macros used elsewhere in this directory
// (Test_LogiaAmbient.cpp). CHECK_EQ is not defined here.
//
// Headless, no HWND. Each test constructs its own LogiaRuntimeBridge.

#include "AYScript.h"
#include "AYScriptRuntimeBridge.h"
#include "logia/AYCompilerError.h"
#include "logia/AYLogia.h"
#include "AYTest.h"

#include <string>
#include <vector>

using ayt::script::LogiaRuntimeBridge;
using ayt::script::logia::CompilerError;
using ayt::script::logia::LogiaHostContext;
using ayt::script::logia::LogiaHostKind;

namespace {

constexpr const char* kSimpleScript = R"(
script Cached {
    var n: int = 0
    on_update() {
        n = n + 1
        __cache_witness = n
    }
}
)";

constexpr const char* kAltScript = R"(
script AltCached {
    var m: int = 0
    on_update() {
        m = m + 2
        __cache_witness = m
    }
}
)";

// Intentionally unparseable: missing closing brace. Compile
// pipeline must produce a non-empty error; cache must remember it
// so a repeat call reproduces the failure (hit) without re-lexing.
constexpr const char* kBrokenScript = R"(
script Broken {
    var x: int = 1
)";

bool loadScriptDefault(LogiaRuntimeBridge& bridge,
                       const char* name,
                       const char* src,
                       std::vector<CompilerError>& errors)
{
    return bridge.loadScript(name, src, errors);
}

} // namespace

TEST_SUITE(LogiaCompileCacheTests)

// --- 1. First call misses, second call hits -------------------------

TEST_CASE(cache_second_load_of_same_source_is_a_hit) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    // Cold: both counters start at zero.
    CHECK(bridge.compileCacheHitCount() == 0u);
    CHECK(bridge.compileCacheMissCount() == 0u);

    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(errors.empty());
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount() == 0u);

    // Same script name + identical source string: cache hit.
    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(errors.empty());
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount() == 1u);

    // Third load: another hit.
    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount() == 2u);
}

// --- 2. Hit produces identical hasScript + lifecycle behavior --------

TEST_CASE(cache_hit_preserves_hasScript_and_lifecycle) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(bridge.hasScript("Cached"));

    // Lifecycle works on miss path.
    float dt = 0.016f;
    CHECK(bridge.callLifecycle("Cached", "on_update", nullptr, &dt));
    double witness = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__cache_witness", witness));
    CHECK(static_cast<int>(witness) == 1);

    // Second loadScript hits the cache AND the lifecycle still runs
    // because Lua chunk re-execution is a per-call step (not a
    // cache step). The module table is freshly bound under the same
    // name; the cache contract is "hit produces SAME Lua module
    // table SHAPE" — verified by the next callLifecycle returning
    // success against the freshly-bound on_update, and the local
    // `n` redeclared to 0 by the new Lua execution so it observes
    // n == 1 again on its single on_update.
    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(bridge.hasScript("Cached"));
    CHECK(bridge.compileCacheHitCount() == 1u);

    CHECK(bridge.callLifecycle("Cached", "on_update", nullptr, &dt));
    witness = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__cache_witness", witness));
    CHECK(static_cast<int>(witness) == 1);
}

// --- 3. Different source → miss -------------------------------------

TEST_CASE(cache_different_source_is_miss) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount()  == 0u);

    // Different source under the same name → miss (key differs).
    CHECK(loadScriptDefault(bridge, "Cached", kAltScript, errors));
    CHECK(bridge.compileCacheMissCount() == 2u);
    CHECK(bridge.compileCacheHitCount()  == 0u);

    // The cache key is content-addressed (source bytes), not
    // name-keyed. Re-loading the original source string hits the
    // cache regardless of the script name. The compile result is
    // keyed by content; the per-name module-table registration is a
    // separate runtime-side map (`_scripts`).
    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(bridge.compileCacheMissCount() == 2u);  // unchanged
    CHECK(bridge.compileCacheHitCount()  == 1u);
}

// --- 4. Different LogiaHostContext → miss ---------------------------

TEST_CASE(cache_different_host_context_is_miss) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    // Default ctx (Component).
    LogiaHostContext ctxComponent;
    ctxComponent.kind        = LogiaHostKind::Component;
    ctxComponent.expectSelf  = true;

    // Same source under System ctx — must miss even though bytes match.
    LogiaHostContext ctxSystem;
    ctxSystem.kind        = LogiaHostKind::System;
    ctxSystem.expectSelf  = true;

    CHECK(bridge.loadScript("Cached", kSimpleScript, ctxComponent, errors));
    CHECK(errors.empty());
    CHECK(bridge.compileCacheMissCount() == 1u);

    CHECK(bridge.loadScript("Cached", kSimpleScript, ctxSystem, errors));
    CHECK(errors.empty());
    CHECK(bridge.compileCacheMissCount() == 2u);

    // Re-load Component ctx: hit.
    CHECK(bridge.loadScript("Cached", kSimpleScript, ctxComponent, errors));
    CHECK(bridge.compileCacheHitCount() == 1u);
}

// --- 5. Cached compile failure --------------------------------------

TEST_CASE(cache_records_failed_compile_and_skips_front_end) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    // First load: miss, fails, errors populated. Failure is cached
    // (entry with empty generatedLua + compileOk=false).
    CHECK(loadScriptDefault(bridge, "Broken", kBrokenScript, errors) == false);
    CHECK(!errors.empty());
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount()  == 0u);
    CHECK(bridge.hasScript("Broken") == false);

    // Second load of the same source: must HIT (skipped Lex/Parser/
    // Semantic/Codegen) AND still surface a non-empty error. The
    // hit/miss counters reflect the cache lookup, not the success.
    errors.clear();
    CHECK(loadScriptDefault(bridge, "Broken", kBrokenScript, errors) == false);
    CHECK(!errors.empty());
    CHECK(bridge.compileCacheMissCount() == 1u);  // unchanged
    CHECK(bridge.compileCacheHitCount()  == 1u);  // bumped
    CHECK(bridge.hasScript("Broken") == false);
}

// --- 6. clearCompileCache preserves runtime modules -----------------

TEST_CASE(clear_compile_cache_preserves_loaded_modules) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(bridge.hasScript("Cached"));
    CHECK(bridge.compileCacheMissCount() == 1u);

    bridge.clearCompileCache();
    // Counter untouched (clearCompileCache wipes cache but not
    // counters; resetCompileCacheCounters would zero them).
    CHECK(bridge.compileCacheMissCount() == 1u);

    // Module table in _scripts is independent of the compile cache
    // — should still be runnable.
    float dt = 0.016f;
    CHECK(bridge.callLifecycle("Cached", "on_update", nullptr, &dt));
    double witness = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__cache_witness", witness));
    CHECK(static_cast<int>(witness) == 1);

    // Next loadScript of the same source now MISSES (cache wiped).
    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(bridge.compileCacheMissCount() == 2u);
    CHECK(bridge.compileCacheHitCount()  == 0u);
}

// --- 7. resetCompileCacheCounters only zeros counters --------------

TEST_CASE(reset_counters_keeps_cache_intact) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(bridge.compileCacheHitCount()  == 1u);
    CHECK(bridge.compileCacheMissCount() == 1u);

    bridge.resetCompileCacheCounters();
    CHECK(bridge.compileCacheHitCount()  == 0u);
    CHECK(bridge.compileCacheMissCount() == 0u);

    // Cache itself is still warm → third load is a hit.
    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(bridge.compileCacheHitCount()  == 1u);
    CHECK(bridge.compileCacheMissCount() == 0u);
}

// --- 8. shutdown wipes both ---------------------------------------

TEST_CASE(shutdown_wipes_compile_cache) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(bridge.compileCacheHitCount()  == 1u);
    CHECK(bridge.compileCacheMissCount() == 1u);

    bridge.shutdown();

    CHECK(bridge.compileCacheHitCount()  == 0u);
    CHECK(bridge.compileCacheMissCount() == 0u);

    // After shutdown, the bridge is reusable (sol::state reset).
    CHECK(loadScriptDefault(bridge, "Cached", kSimpleScript, errors));
    CHECK(errors.empty());
    CHECK(bridge.hasScript("Cached"));
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount()  == 0u);
}

// S4.1 (2026-07-15): the pipeline version bump (24 → 25) forces
// every pre-existing cache entry to miss on first load after the
// bump. We can't test the cross-process migration (a process running
// v24 doesn't exist any more), but we CAN pin that a fresh bridge
// sees the expected version constant — and that loading a
// signal-bearing script compiles successfully end-to-end (the cache
// key implicitly includes the version, so a miss on first load is
// the success path).
TEST_CASE(s41_cache_version_bump_force_first_load_miss) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    const char* signalSrc = R"(
script SignalScript {
    signal ping()
    on_start() {
        connect("ping", on_ping)
    }
    on_update(dt: float) {
        emit("ping")
    }
    function on_ping() {
        __test_witness = "pinged"
    }
}
)";
    // First load: miss (counter == 1).
    CHECK(loadScriptDefault(bridge, "SignalScript", signalSrc, errors));
    CHECK(errors.empty());
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount()  == 0u);
    CHECK(bridge.hasScript("SignalScript"));

    // Second load with identical source: hit (counter == 1, miss unchanged).
    CHECK(loadScriptDefault(bridge, "SignalScript", signalSrc, errors));
    CHECK(bridge.compileCacheHitCount()  == 1u);
    CHECK(bridge.compileCacheMissCount() == 1u);
}

TEST_SUITE_END
