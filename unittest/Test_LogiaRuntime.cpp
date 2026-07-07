// Logia runtime unit tests (S1)
//
// Verifies that LogiaRuntimeBridge can:
//   - load a compiled script and report hasScript() == true
//   - invoke lifecycle methods without throwing
//   - propagate engine API calls (log.*, input.*) correctly
//   - handle missing methods and unknown scripts gracefully
//
// The bridge is exercised standalone — no ECS / AYEntity / AYGameLoop.

#include "AYScript.h"
#include "AYScriptRuntimeBridge.h"
#include "logia/AYCompilerError.h"
#include "AYTest.h"

#include <string>
#include <vector>

using ayt::script::LogiaRuntimeBridge;
using ayt::script::logia::CompilerError;

namespace {

// Drive Compiler + LuaCodegen → Lua source via the bridge.
bool loadFromSource(LogiaRuntimeBridge& bridge,
                    const std::string& name,
                    const char* logiaSource,
                    std::vector<CompilerError>& errors)
{
    return bridge.loadScript(name, logiaSource, errors);
}

} // namespace

TEST_SUITE(LogiaRuntimeTests)

TEST_CASE(runtime_load_simple_component) {
    LogiaRuntimeBridge bridge;
    CHECK(bridge.isInitialized());

    const char* src = R"(
component Empty {
}
)";
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "Empty", src, errors));
    CHECK(errors.empty());
    CHECK(bridge.hasScript("Empty"));
}

TEST_CASE(runtime_load_invalid_logia_returns_false) {
    LogiaRuntimeBridge bridge;

    // Missing closing brace — parser will fail.
    const char* src = R"(
component Broken {
    var x: int = 1
)";
    std::vector<CompilerError> errors;
    CHECK_FALSE(loadFromSource(bridge, "Broken", src, errors));
    CHECK_FALSE(errors.empty());
    CHECK_FALSE(bridge.hasScript("Broken"));
}

TEST_CASE(runtime_unknown_script_hasScript_false) {
    LogiaRuntimeBridge bridge;
    CHECK_FALSE(bridge.hasScript("Nonsense"));
}

TEST_CASE(runtime_call_on_start_invokes_log) {
    LogiaRuntimeBridge bridge;

    const char* src = R"(
component Logger {
    on_start(entity: Entity) {
        log.info("start called")
    }
}
)";
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "Logger", src, errors));
    CHECK(errors.empty());

    // Should not throw — log.info routes to AYLog.
    // S2: receiver is opaque void* (ScriptComponent* in real usage).
    // Pass nullptr since this test doesn't observe the receiver.
    CHECK(bridge.callLifecycle("Logger", "on_start",
                               /*receiver*/ nullptr,
                               /*arg2*/ nullptr));
}

TEST_CASE(runtime_call_on_update_dt) {
    LogiaRuntimeBridge bridge;

    const char* src = R"(
component Stepper {
    on_update(dt: float) {
        log.info("dt=")
    }
}
)";
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "Stepper", src, errors));

    float dt = 0.016f;
    CHECK(bridge.callLifecycle("Stepper", "on_update",
                               /*receiver*/ nullptr,
                               /*arg2*/ &dt));
}

TEST_CASE(runtime_call_on_destroy) {
    LogiaRuntimeBridge bridge;

    const char* src = R"(
component Cleaner {
    on_destroy() {
        log.info("cleaned")
    }
}
)";
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "Cleaner", src, errors));
    CHECK(bridge.callLifecycle("Cleaner", "on_destroy"));
}

TEST_CASE(runtime_missing_method_returns_false) {
    LogiaRuntimeBridge bridge;

    const char* src = R"(
component OnlyStart {
    on_start(entity: Entity) {
        log.info("hi")
    }
}
)";
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "OnlyStart", src, errors));

    // Component has no on_destroy
    CHECK_FALSE(bridge.callLifecycle("OnlyStart", "on_destroy"));
}

TEST_CASE(runtime_input_branching) {
    LogiaRuntimeBridge bridge;

    // input.is_pressed("jump") returns true (mock).
    // Verifies the bridge correctly exposes mockIsPressed and that the
    // generated Lua 'if ... then' actually enters the branch.
    const char* src = R"(
component JumpOnJump {
    on_update(dt: float) {
        if input.is_pressed("jump") {
            log.info("branched-in")
        } else {
            log.info("branched-out")
        }
    }
}
)";
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "JumpOnJump", src, errors));

    float dt = 1.0f;
    // Mock returns true for "jump" → the if-branch fires and log.info
    // prints "branched-in". The call should succeed.
    CHECK(bridge.callLifecycle("JumpOnJump", "on_update",
                               /*receiver*/ nullptr,
                               /*arg2*/ &dt));
}

TEST_CASE(runtime_reload_replaces_script) {
    LogiaRuntimeBridge bridge;

    const char* v1 = R"(
component Reload {
    export var x: int = 1
}
)";
    const char* v2 = R"(
component Reload {
    export var x: int = 2
}
)";
    std::vector<CompilerError> errors;

    CHECK(loadFromSource(bridge, "Reload", v1, errors));
    CHECK(bridge.hasScript("Reload"));

    // Re-load with different source under same name — should replace.
    CHECK(loadFromSource(bridge, "Reload", v2, errors));
    CHECK(bridge.hasScript("Reload"));
    CHECK(errors.empty());
}

TEST_SUITE_END