// Test_LogiaAmbient.cpp — S3.5: real ambient API bindings (time + input)
//
// Covers the S3.5 scope:
//   1. `time.delta` reflects the scaled dt published by
//      LogiaRuntimeBridge::tickAmbient(dt) (no longer a static 0).
//   2. `time.total` accumulates across multiple ticks.
//   3. `input.is_pressed`/`input.is_just_pressed` route through an
//      injectable InputProvider*; default MockInputProvider keeps the
//      "jump only" S1 behavior; tests plug in a custom provider to
//      verify the dispatch path.
//   4. ScriptSubSystem::update(dt) calls tickAmbient(dt) before any
//      lifecycle dispatch — so the ambient table reads from the
//      authoritative scaled dt in the integrated tick path, not from
//      a separate default.
//   5. time.delta is safe to read from a script that runs *before*
//      the first tickAmbient (returns 0, not a crash).
//
// Headless: no HWND. The injectable backend isolates the test from
// real input devices.

#include "AYScript.h"
#include "AYScriptRuntimeBridge.h"
#include "AYScriptSubSystem.h"
#include "logia/AYCompilerError.h"
#include "AYTest.h"
#include "AYLogger.h"

#include <cmath>
#include <string>
#include <vector>

using ayt::script::LogiaRuntimeBridge;
using ayt::script::ScriptSubSystem;
using ayt::script::logia::CompilerError;

namespace {

bool loadFromSource(LogiaRuntimeBridge& bridge,
                    const char* name,
                    const char* source,
                    std::vector<CompilerError>& errors)
{
    return bridge.loadScript(name, source, errors);
}

// Visible-only test: script reads time.delta and time.total and
// stashes them as Lua numbers. We assert after callLifecycle that
// the cached values match the bridge's published values.
constexpr const char* kTimeWitness = R"(
script TimeWitness {
    on_update() {
        __test_witness = tostring(time.delta) .. "," .. tostring(time.total)
    }
}
)";

// Script reads input.is_pressed into __test_witness as a string,
// encoding the boolean as "0"/"1". Lets us inspect the boolean from
// C++ without coupling to ayt::log line-parse.
constexpr const char* kInputWitness = R"(
script InputWitness {
    on_update() {
        local p  = input.is_pressed("jump")
        local jp = input.is_just_pressed("jump")
        __test_witness = (p and "1" or "0") .. "," .. (jp and "1" or "0")
    }
}
)";

// Trivial test for "time.delta before any tick". Cold bridge, no
// tickAmbient, script reads time.delta → 0 should land in __test_witness.
constexpr const char* kColdDelta = R"(
script ColdDelta {
    on_update() {
        __test_witness = tostring(time.delta)
    }
}
)";

} // namespace

TEST_SUITE(LogiaAmbientTests)

// ------------------------------------------------------------------
// time.delta wiring — replaces the S1 zero-everywhere mock with a
// real value sourced from tickAmbient().
// ------------------------------------------------------------------

TEST_CASE(ambient_time_delta_default_zero_before_tick) {
    LogiaRuntimeBridge bridge;
    CHECK(bridge.isInitialized());
    CHECK_EQ(bridge.currentDelta(), 0.0f);
    CHECK_EQ(bridge.totalElapsed(), 0.0f);

    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "ColdDelta", kColdDelta, errors));
    CHECK(errors.empty());

    float dt = 0.016f;
    CHECK(bridge.callLifecycle("ColdDelta", "on_update", nullptr, &dt));
    // Important: tickAmbient MUST be called before the script sees
    // time.delta — that is exactly ScriptSubSystem::update's job.
    // This test simulates the "directly callLifecycle" path that
    // legacy tests use; it confirms cold delta = 0 (no fallback
    // crash, no garbage).
    CHECK_EQ(bridge.getLuaGlobalString("__test_witness"), std::string("0.0"));
}

TEST_CASE(ambient_time_delta_published_via_tickAmbient) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "TimeWitness", kTimeWitness, errors));
    CHECK(errors.empty());

    // Publish a known dt BEFORE the script runs — exactly what
    // ScriptSubSystem::update will do under the live GameLoop.
    bridge.tickAmbient(0.016f);
    CHECK_EQ(bridge.currentDelta(), 0.016f);

    CHECK(bridge.callLifecycle("TimeWitness", "on_update", nullptr, nullptr));
    // After the first tick: total elapsed == 0.016, delta == 0.016.
    // Lua's tostring() emits at minimum one fractional digit, so
    // "0.016,0.016" is the canonical form.
    CHECK_EQ(bridge.getLuaGlobalString("__test_witness"),
             std::string("0.016,0.016"));
}

TEST_CASE(ambient_time_total_accumulates_across_ticks) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "TimeWitness", kTimeWitness, errors));
    CHECK(errors.empty());

    // Three ticks with the same dt — total should accumulate
    // linearly; delta always reads the most recent publish.
    bridge.tickAmbient(0.016f);
    CHECK(bridge.callLifecycle("TimeWitness", "on_update", nullptr, nullptr));
    bridge.tickAmbient(0.016f);
    CHECK(bridge.callLifecycle("TimeWitness", "on_update", nullptr, nullptr));
    bridge.tickAmbient(0.016f);
    CHECK(bridge.callLifecycle("TimeWitness", "on_update", nullptr, nullptr));

    CHECK(std::fabs(bridge.totalElapsed() - 0.048f) < 1e-5f);
    CHECK_EQ(bridge.currentDelta(), 0.016f);
    // After three ticks: delta=0.016, total=0.048
    CHECK_EQ(bridge.getLuaGlobalString("__test_witness"),
             std::string("0.016,0.048"));
}

TEST_CASE(ambient_tickAmbient_negative_dt_is_clamped) {
    // A buggy caller might pass a negative dt; the bridge clamps
    // it to 0 to avoid time going backward. Also forces total
    // accumulation to advance zero (not negative).
    LogiaRuntimeBridge bridge;
    bridge.tickAmbient(-1.0f);
    CHECK_EQ(bridge.currentDelta(), 0.0f);
    CHECK_EQ(bridge.totalElapsed(), 0.0f);

    bridge.tickAmbient(0.5f);
    CHECK(std::fabs(bridge.totalElapsed() - 0.5f) < 1e-5f);
}

TEST_CASE(ambient_script_sub_system_publishes_dt_to_bridge) {
    // End-to-end integration: ScriptSubSystem::update(0.016f) must
    // tickAmbient() before dispatching lifecycle. This is the
    // critical guarantee that makes the ambient table real under
    // the live GameLoop — the canonicalized path that ships.
    ScriptSubSystem sys;
    CHECK(sys.initialize());

    std::vector<CompilerError> errors;
    CHECK(loadFromSource(sys.bridge(), "TimeWitness", kTimeWitness, errors));
    CHECK(errors.empty());

    // Pre-tick: nothing has been pushed into the bridge yet.
    CHECK_EQ(sys.bridge().currentDelta(), 0.0f);

    sys.update(0.016f);
    // After update(): delta reflects the published value and total
    // has accumulated by it. The script also ran (__test_witness
    // should be populated with the live values).
    CHECK_EQ(sys.bridge().currentDelta(), 0.016f);
    CHECK(std::fabs(sys.bridge().totalElapsed() - 0.016f) < 1e-5f);
    CHECK_EQ(sys.bridge().getLuaGlobalString("__test_witness"),
             std::string("0.016,0.016"));

    sys.shutdown();
}

// ------------------------------------------------------------------
// input.is_pressed / input.is_just_pressed — injectable backend
// ------------------------------------------------------------------

namespace {

// Test-only provider that returns whatever the test pre-loaded.
// Lets us verify dispatch without owning an OS input layer.
struct ScriptedInputProvider final
    : public LogiaRuntimeBridge::InputProvider {
    bool pressedReturn = false;
    bool justPressedReturn = false;
    std::string lastPressedKey;
    std::string lastJustPressedKey;
    int pressedQueries = 0;
    int justPressedQueries = 0;

    bool isPressed(const std::string& key) const override {
        ++pressedQueries;
        lastPressedKey = key;
        return pressedReturn;
    }
    bool isJustPressed(const std::string& key) const override {
        ++justPressedQueries;
        lastJustPressedKey = key;
        return justPressedReturn;
    }
};

} // namespace

TEST_CASE(ambient_input_default_mock_keeps_jump_pressed) {
    // Default provider = MockInputProvider → "jump" pressed (S1
    // behavior), "anything-else" not pressed. This proves the
    // S1 mock site was rewired, not deleted.
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "InputWitness", kInputWitness, errors));
    CHECK(errors.empty());

    CHECK(bridge.inputProvider() != nullptr);
    CHECK(bridge.callLifecycle("InputWitness", "on_update", nullptr, nullptr));
    CHECK_EQ(bridge.getLuaGlobalString("__test_witness"),
             std::string("1,0")); // pressed=1, just_pressed=0
}

TEST_CASE(ambient_input_custom_provider_dispatches_correctly) {
    // Plug a scripted provider that returns 0/0 by default and a
    // true/false combo on demand. Verify both dispatch and that the
    // provider received the expected key string.
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "InputWitness", kInputWitness, errors));
    CHECK(errors.empty());

    ScriptedInputProvider provider;
    provider.pressedReturn    = true;
    provider.justPressedReturn = true;
    bridge.setInputProvider(&provider);

    CHECK(bridge.inputProvider() == &provider);
    CHECK(bridge.callLifecycle("InputWitness", "on_update", nullptr, nullptr));

    // The provider saw the query AND the script observed the truthy
    // values for both predicates. Recorded key string proves the
    // table forwards string args without translation.
    CHECK(provider.pressedQueries      == 1);
    CHECK(provider.justPressedQueries  == 1);
    CHECK_EQ(provider.lastPressedKey,     std::string("jump"));
    CHECK_EQ(provider.lastJustPressedKey, std::string("jump"));
    CHECK_EQ(bridge.getLuaGlobalString("__test_witness"),
             std::string("1,1"));
}

TEST_CASE(ambient_input_null_provider_falls_back_to_default) {
    // Setting nullptr explicitly must NOT crash the next call. The
    // bridge clamps back to the default MockInputProvider.
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "InputWitness", kInputWitness, errors));
    CHECK(errors.empty());

    bridge.setInputProvider(nullptr);
    // callLifecycle should succeed by way of the mock fallback.
    CHECK(bridge.callLifecycle("InputWitness", "on_update", nullptr, nullptr));
    CHECK_EQ(bridge.getLuaGlobalString("__test_witness"),
             std::string("1,0")); // default mock = jump-pressed
}

TEST_SUITE_END
