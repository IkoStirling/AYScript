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

#include <IAYEntity.h>
#include <AYWorld.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using ayt::script::LogiaRuntimeBridge;
using ayt::script::ScriptSubSystem;
using ayt::script::logia::CompilerError;

namespace {

bool nearEqual(double a, double b, double eps = 1e-5)
{
    return std::fabs(a - b) < eps;
}

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
// Logia has no `..` string concat — use separate witness globals.
// Assign numeric witness globals (avoid tostring float formatting).
constexpr const char* kTimeWitness = R"(
script TimeWitness {
    on_update() {
        __witness_delta = time.delta
        __witness_total = time.total
    }
}
)";

// Separate globals for pressed / just_pressed (no `..` concat).
constexpr const char* kInputWitness = R"(
script InputWitness {
    on_update() {
        __test_witness_p = input.is_pressed("jump") and "1" or "0"
        __test_witness_j = input.is_just_pressed("jump") and "1" or "0"
    }
}
)";

// Trivial test for "time.delta before any tick". Cold bridge, no
// tickAmbient, script reads time.delta → 0 should land in __test_witness.
constexpr const char* kColdDelta = R"(
script ColdDelta {
    on_update() {
        __witness_delta = time.delta
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
    CHECK(bridge.currentDelta()==0.0f);
    CHECK(bridge.totalElapsed()==0.0f);

    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "ColdDelta", kColdDelta, errors));
    CHECK(errors.empty());

    float dt = 0.016f;
    CHECK(bridge.callLifecycle("ColdDelta", "on_update", nullptr, &dt));
    double witnessDelta = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_delta", witnessDelta));
    CHECK(nearEqual(witnessDelta, 0.0));
}

TEST_CASE(ambient_time_delta_published_via_tickAmbient) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "TimeWitness", kTimeWitness, errors));
    CHECK(errors.empty());

    // Publish a known dt BEFORE the script runs — exactly what
    // ScriptSubSystem::update will do under the live GameLoop.
    bridge.tickAmbient(0.016f);
    CHECK(bridge.currentDelta()==0.016f);

    CHECK(bridge.callLifecycle("TimeWitness", "on_update", nullptr, nullptr));
    double witnessDelta = -1.0;
    double witnessTotal = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_delta", witnessDelta));
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_total", witnessTotal));
    CHECK(nearEqual(witnessDelta, 0.016));
    CHECK(nearEqual(witnessTotal, 0.016));
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
    CHECK(bridge.currentDelta()==0.016f);
    double witnessDelta = -1.0;
    double witnessTotal = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_delta", witnessDelta));
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_total", witnessTotal));
    CHECK(nearEqual(witnessDelta, 0.016));
    CHECK(nearEqual(witnessTotal, 0.048));
}

TEST_CASE(ambient_tickAmbient_negative_dt_is_clamped) {
    // A buggy caller might pass a negative dt; the bridge clamps
    // it to 0 to avoid time going backward. Also forces total
    // accumulation to advance zero (not negative).
    LogiaRuntimeBridge bridge;
    bridge.tickAmbient(-1.0f);
    CHECK(bridge.currentDelta()==0.0f);
    CHECK(bridge.totalElapsed()==0.0f);

    bridge.tickAmbient(0.5f);
    CHECK(std::fabs(bridge.totalElapsed() - 0.5f) < 1e-5f);
}

TEST_CASE(ambient_script_sub_system_publishes_dt_to_bridge) {
    // End-to-end integration: ScriptSubSystem::update(0.016f) must
    // tickAmbient() BEFORE dispatching lifecycle. The canonical
    // integration path: ScriptSubSystem.tickLogiaSystems walks the
    // World and forwards dt into each system whose name matches a
    // loaded Logia script. To prove sys.update really invokes the
    // Logia under the published ambient state, register a minimal
    // ISystem shim named "TimeWitness", load the Logia under the
    // same name, then drive sys.update(0.016f) and read the witness
    // globals back from the bridge.
    struct TimeWitnessSystem : public ayt::entity::ISystem {
        const char* getName() const override { return "TimeWitness"; }
        void onUpdate(float /*dt*/) override {
            // No-op: the Logia on_update is what we want to exercise.
            // The ISystem exists only to give tickLogiaSystems a
            // registered handle whose name matches the Logia.
        }
    };

    auto& world = ayt::entity::World::instance();
    world.shutdown();  // isolate from any prior test (S3.4 fix pattern)
    world.initialize();
    world.registerSystem<TimeWitnessSystem>(/* priority */ 200);

    auto sys = std::make_unique<ScriptSubSystem>();
    CHECK(sys->initialize());

    std::vector<CompilerError> errors;
    CHECK(loadFromSource(sys->bridge(), "TimeWitness", kTimeWitness, errors));
    CHECK(errors.empty());

    // Pre-tick: nothing has been pushed into the bridge yet.
    CHECK(sys->bridge().currentDelta()==0.0f);

    sys->update(0.016f);

    // After update(): bridge ambient state was published by
    // tickAmbient(dt) BEFORE the lifecycle dispatch — and the
    // TimeWitness.on_update ran and stashed its observations into
    // __witness_delta / __witness_total.
    CHECK(sys->bridge().currentDelta()==0.016f);
    CHECK(std::fabs(sys->bridge().totalElapsed() - 0.016f) < 1e-5f);
    double witnessDelta = -1.0;
    double witnessTotal = -1.0;
    CHECK(sys->bridge().tryGetLuaGlobalNumber("__witness_delta", witnessDelta));
    CHECK(sys->bridge().tryGetLuaGlobalNumber("__witness_total", witnessTotal));
    CHECK(nearEqual(witnessDelta, 0.016));
    CHECK(nearEqual(witnessTotal, 0.016));

    sys->shutdown();
    world.shutdown();
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
    mutable std::string lastPressedKey;
    mutable std::string lastJustPressedKey;
    mutable int pressedQueries = 0;
    mutable int justPressedQueries = 0;

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
    CHECK(bridge.getLuaGlobalString("__test_witness_p")==
             std::string("1"));
    CHECK(bridge.getLuaGlobalString("__test_witness_j")==
             std::string("0"));
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
    CHECK(provider.lastPressedKey==     std::string("jump"));
    CHECK(provider.lastJustPressedKey== std::string("jump"));
    CHECK(bridge.getLuaGlobalString("__test_witness_p")==
             std::string("1"));
    CHECK(bridge.getLuaGlobalString("__test_witness_j")==
             std::string("1"));
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
    CHECK(bridge.getLuaGlobalString("__test_witness_p")==
             std::string("1"));
    CHECK(bridge.getLuaGlobalString("__test_witness_j")==
             std::string("0"));
}

TEST_SUITE_END
