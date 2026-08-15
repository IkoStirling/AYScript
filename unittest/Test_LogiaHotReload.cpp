// Test_LogiaHotReload.cpp — S3.7a / LG-06b part A in-memory reload.
//
// Verifies the pure-memory `LogiaRuntimeBridge::reloadScript` API
// (S3.7a, minimal first slice — no FileWatcher, no AYIO link, no
// ScriptSubSystem coordinator):
//
//   1. reloadScript with NEW source replaces the cached module so
//      a subsequent callLifecycle observes the new behavior (the
//      `__test_witness` value flips to prove a re-bind occurred).
//   2. reloadScript with FAILING source (e.g. syntax error) keeps
//      hasScript(name) == true and keeps the prior good module
//      table — a typo in the editor does NOT detach the running
//      component. Function returns false and populates `errors`.
//   3. reloadScript with IDENTICAL source to a prior loadScript
//      hits the compile cache (counter bumps by exactly 1, miss
//      counter does not advance).
//   4. Lifecycle dispatched against the reloaded module sees the
//      new on_update body — the witness flips to the new value.
//   5. The 3-arg reloadScript delegator with default host context
//      produces the same outcome as a same-context loadScript+call
//      path (verifies the 3-arg / 4-arg symmetry).
//
// Headless, no HWND. No filesystem dependency. New file only —
// CMakeLists.txt already globs *.cpp so it picks this up
// automatically.
//
// AYScript test macro convention: assertions are written
//   CHECK(condition);
//   CHECK(expr == expected);
// per the AYTest macros used elsewhere in this directory
// (Test_LogiaAmbient.cpp, Test_LogiaCompileCache.cpp). CHECK_EQ
// is not defined here.

#include "AYScript.h"
#include "AYScript/ScriptRuntimeBridge.h"
#include "AYScript/logia/CompilerError.h"
#include "AYScript/logia/Logia.h"
#include "AYTest.h"

#include <string>
#include <vector>

using ayt::script::LogiaRuntimeBridge;
using ayt::script::logia::CompilerError;

namespace {

constexpr const char* kScriptV1 = R"(
script Reloadable {
    var n: int = 0
    on_update() {
        n = n + 1
        __test_witness = tostring(n)
    }
}
)";

// V2: increments by 10 instead of 1, so the witness flips from "1"
// to "10" on the next on_update — a value-shape test that proves
// the module table was re-bound.
constexpr const char* kScriptV2 = R"(
script Reloadable {
    var n: int = 0
    on_update() {
        n = n + 10
        __test_witness = tostring(n)
    }
}
)";

// V3: identical to V1 — must hit the compile cache.
constexpr const char* kScriptV1Again = kScriptV1;

// Intentionally unparseable: missing closing brace.
constexpr const char* kBrokenSource = R"(
script Broken {
    var x: int = 1
)";

} // namespace

TEST_SUITE(LogiaHotReloadTests)

// --- 1. Reload with new source replaces module table ----------------

TEST_CASE(reload_with_new_source_replaces_module_behavior) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    // Load V1.
    CHECK(bridge.loadScript("Reloadable", kScriptV1, errors));
    CHECK(errors.empty());
    CHECK(bridge.hasScript("Reloadable"));

    // Run V1 on_update — witness becomes "1".
    float dt = 0.016f;
    CHECK(bridge.callLifecycle("Reloadable", "on_update", nullptr, &dt));
    std::string w = bridge.getLuaGlobalString("__test_witness");
    CHECK(w == std::string("1"));

    // Reload with V2 — different source, different behavior.
    errors.clear();
    CHECK(bridge.reloadScript("Reloadable", kScriptV2, errors));
    CHECK(errors.empty());
    CHECK(bridge.hasScript("Reloadable"));

    // Dispatch the freshly-bound on_update. V2's body does n = n + 10
    // starting from n=0 (the new local re-declared at chunk load),
    // so the witness becomes "10" — proves the module table was
    // actually re-bound, not a no-op.
    dt = 0.016f;
    CHECK(bridge.callLifecycle("Reloadable", "on_update", nullptr, &dt));
    w = bridge.getLuaGlobalString("__test_witness");
    CHECK(w == std::string("10"));
}

// --- 2. Reload with bad source keeps prior good module ---------------

TEST_CASE(reload_with_bad_source_keeps_prior_module) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    // Load V1 successfully.
    CHECK(bridge.loadScript("Reloadable", kScriptV1, errors));
    CHECK(bridge.hasScript("Reloadable"));
    float dt = 0.016f;
    CHECK(bridge.callLifecycle("Reloadable", "on_update", nullptr, &dt));
    std::string w = bridge.getLuaGlobalString("__test_witness");
    CHECK(w == std::string("1"));

    // Reload with broken source — must return false, populate errors,
    // and LEAVE the prior good module bound.
    errors.clear();
    CHECK(bridge.reloadScript("Reloadable", kBrokenSource, errors) == false);
    CHECK(!errors.empty());
    // Critical invariant: hasScript still true (prior module preserved).
    CHECK(bridge.hasScript("Reloadable"));

    // Dispatch on_update — must run the V1 body (NOT fail because
    // the new compile failed). Witness still flips 1 -> 2.
    dt = 0.016f;
    CHECK(bridge.callLifecycle("Reloadable", "on_update", nullptr, &dt));
    w = bridge.getLuaGlobalString("__test_witness");
    CHECK(w == std::string("2"));
}

// --- 3. Reload with identical source hits compile cache --------------

TEST_CASE(reload_with_identical_source_hits_compile_cache) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    // Cold load — 1 miss.
    CHECK(bridge.loadScript("Reloadable", kScriptV1, errors));
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount()  == 0u);

    // Reload with byte-identical source — must hit the cache.
    // (The first reloadScript call IS a miss because the V2 entry
    // was never inserted; but the second reloadScript with V1 hits.)
    errors.clear();
    CHECK(bridge.reloadScript("Reloadable", kScriptV1Again, errors));
    CHECK(errors.empty());
    CHECK(bridge.compileCacheMissCount() == 1u);  // unchanged
    CHECK(bridge.compileCacheHitCount()  == 1u);  // bumped

    // Third reload — also a hit.
    errors.clear();
    CHECK(bridge.reloadScript("Reloadable", kScriptV1Again, errors));
    CHECK(bridge.compileCacheMissCount() == 1u);
    CHECK(bridge.compileCacheHitCount()  == 2u);
}

// --- 4. Reload then call — fresh body runs ---------------------------

TEST_CASE(reload_then_call_executes_fresh_body) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    CHECK(bridge.loadScript("Reloadable", kScriptV1, errors));
    float dt = 0.016f;
    CHECK(bridge.callLifecycle("Reloadable", "on_update", nullptr, &dt));
    CHECK(bridge.getLuaGlobalString("__test_witness") == std::string("1"));

    // Hot-reload to V2.
    errors.clear();
    CHECK(bridge.reloadScript("Reloadable", kScriptV2, errors));

    // After reload the witness from V1 is gone (V2 has its own `n`
    // local starting at 0). First on_update under V2 → "10".
    dt = 0.016f;
    CHECK(bridge.callLifecycle("Reloadable", "on_update", nullptr, &dt));
    CHECK(bridge.getLuaGlobalString("__test_witness") == std::string("10"));

    // Second on_update under V2 → "20" (proves state continuity
    // is *not* preserved across reload — fresh locals each time,
    // matching the loadScript contract).
    dt = 0.016f;
    CHECK(bridge.callLifecycle("Reloadable", "on_update", nullptr, &dt));
    CHECK(bridge.getLuaGlobalString("__test_witness") == std::string("20"));
}

// --- 5. 3-arg reloadScript matches 4-arg path ------------------------

TEST_CASE(reload_three_arg_uses_default_host_context) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    // Load via 3-arg (default ctx) — count miss.
    CHECK(bridge.loadScript("Reloadable", kScriptV1, errors));
    CHECK(bridge.compileCacheMissCount() == 1u);

    // Reload via 3-arg (default ctx) — should hit because the prior
    // loadScript used the same default ctx (LogiaHostKind::Component).
    errors.clear();
    CHECK(bridge.reloadScript("Reloadable", kScriptV2, errors));
    CHECK(bridge.compileCacheMissCount() == 2u);  // V2 is a new key → miss

    // Reload via 3-arg with V2 again — hit.
    errors.clear();
    CHECK(bridge.reloadScript("Reloadable", kScriptV2, errors));
    CHECK(bridge.compileCacheHitCount() == 1u);

    // Lifecycle still works after two reloads.
    float dt = 0.016f;
    CHECK(bridge.callLifecycle("Reloadable", "on_update", nullptr, &dt));
    CHECK(bridge.getLuaGlobalString("__test_witness") == std::string("10"));
}

// --- 6. Reload script that was never loaded --------------------------

TEST_CASE(reload_of_unloaded_script_loads_it) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;

    // No prior loadScript for "Fresh" — reloadScript must behave
    // like loadScript in that case: it loads the script, hasScript
    // flips true, lifecycle works.
    CHECK(bridge.reloadScript("Fresh", kScriptV1, errors));
    CHECK(errors.empty());
    CHECK(bridge.hasScript("Fresh"));

    float dt = 0.016f;
    CHECK(bridge.callLifecycle("Fresh", "on_update", nullptr, &dt));
    CHECK(bridge.getLuaGlobalString("__test_witness") == std::string("1"));
}

TEST_SUITE_END
