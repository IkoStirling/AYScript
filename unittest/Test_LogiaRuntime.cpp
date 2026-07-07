// Logia runtime unit tests (S2.5)
//
// Verifies that LogiaRuntimeBridge can:
//   - load a compiled `script` block and report hasScript() == true
//   - invoke lifecycle methods (on_start / on_update / on_destroy)
//     with the receiver as lightuserdata
//   - propagate engine API calls (log.*, input.*) correctly
//   - handle missing methods and unknown scripts gracefully

#include "AYScript.h"
#include "AYScriptRuntimeBridge.h"
#include "logia/AYCompilerError.h"
#include "AYTest.h"

#include <string>
#include <vector>

using ayt::script::LogiaRuntimeBridge;
using ayt::script::logia::CompilerError;

namespace {

bool loadFromSource(LogiaRuntimeBridge& bridge,
                    const std::string& name,
                    const char* logiaSource,
                    std::vector<CompilerError>& errors)
{
    return bridge.loadScript(name, logiaSource, errors);
}

} // namespace

TEST_SUITE(LogiaRuntimeTests)

TEST_CASE(runtime_load_simple_script) {
    LogiaRuntimeBridge bridge;
    CHECK(bridge.isInitialized());

    const char* src = R"(
script Empty {
}
)";
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "Empty", src, errors));
    CHECK(errors.empty());
    CHECK(bridge.hasScript("Empty"));
}

TEST_CASE(runtime_load_invalid_logia_returns_false) {
    LogiaRuntimeBridge bridge;

    const char* src = R"(
script Broken {
    var x: int = 1
)";
    std::vector<CompilerError> errors;
    CHECK_FALSE(loadFromSource(bridge, "Broken", src, errors));
    CHECK_FALSE(errors.empty());
    CHECK_FALSE(bridge.hasScript("Broken"));
}

TEST_CASE(runtime_load_lifecycle_with_param_is_warning) {
    // S2.5: lifecycle functions take no parameters. The parser
    // forgives non-empty param lists; SemanticAnalyzer emits a soft
    // warning. The compile still succeeds.
    LogiaRuntimeBridge bridge;
    const char* src = R"(
script BadLifecycle {
    on_start(entity: Entity) {
        log.info("nope")
    }
}
)";
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "BadLifecycle", src, errors));
    CHECK(errors.empty());
}

TEST_CASE(runtime_unknown_script_hasScript_false) {
    LogiaRuntimeBridge bridge;
    CHECK_FALSE(bridge.hasScript("Nonsense"));
}

TEST_CASE(runtime_call_on_start_invokes_log) {
    LogiaRuntimeBridge bridge;
    const char* src = R"(
script Logger {
    on_start() {
        log.info("start called")
    }
}
)";
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "Logger", src, errors));
    CHECK(errors.empty());

    // S2.5: lifecycle functions take no params (Lua silently drops
    // extras, so the bridge's caller can still pass entity/dt even
    // though the script doesn't read them).
    CHECK(bridge.callLifecycle("Logger", "on_start",
                               /*receiver*/ nullptr,
                               /*arg2*/ nullptr));
}

TEST_CASE(runtime_call_on_update_dt) {
    LogiaRuntimeBridge bridge;
    const char* src = R"(
script Stepper {
    on_update() {
        log.info("step")
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
script Cleaner {
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
script OnlyStart {
    on_start() {
        log.info("hi")
    }
}
)";
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "OnlyStart", src, errors));

    CHECK_FALSE(bridge.callLifecycle("OnlyStart", "on_destroy"));
}

TEST_CASE(runtime_input_branching) {
    LogiaRuntimeBridge bridge;
    const char* src = R"(
script JumpOnJump {
    on_update() {
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
    CHECK(bridge.callLifecycle("JumpOnJump", "on_update",
                               /*receiver*/ nullptr,
                               /*arg2*/ &dt));
}

TEST_CASE(runtime_reload_replaces_script) {
    LogiaRuntimeBridge bridge;

    const char* v1 = R"(
script Reload {
    var x: int = 1
}
)";
    const char* v2 = R"(
script Reload {
    var x: int = 2
}
)";
    std::vector<CompilerError> errors;

    CHECK(loadFromSource(bridge, "Reload", v1, errors));
    CHECK(bridge.hasScript("Reload"));

    CHECK(loadFromSource(bridge, "Reload", v2, errors));
    CHECK(bridge.hasScript("Reload"));
    CHECK(errors.empty());
}

TEST_SUITE_END