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
#include "LogiaTestHelpers.h"
#include "AYTest.h"
#include "AYLog/Logger.h"

// M1: FVector2 reflect registration probe pulls the registry directly
// rather than relying on SemanticAnalyzer ctor side-effect.
#include "AYReflect.h"

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
    logia_test::resetWorldForTest(world);
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

    logia_test::shutdownScriptHost(world, sys.get());
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
    // INT-03 (2026-07-15): pure-virtual defaults added by the
    // InputProvider extension. Tests that exercise only bool
    // predicates continue to use the legacy fields above; new
    // axis/just_released tests live in Test_LogiaDeviceInput.cpp.
    // Returns safe defaults (0.0 / false) so legacy tests don't
    // need to set them explicitly.
    float getAxisValue(const std::string& /*key*/) const override {
        return 0.0f;
    }
    bool isJustReleased(const std::string& /*key*/) const override {
        return false;
    }
    // M1 (2026-07-15): scripted 2-axis injection point. Tests set
    // vec2xReturn / vec2yReturn / vec2Bound before invoking the
    // Logia side; the override records the queried key so tests can
    // assert dispatch reached here. Default-bound=false so legacy
    // tests that never call the 2-axis path don't need to set it.
    bool vec2Bound = false;
    double vec2xReturn = 0.0;
    double vec2yReturn = 0.0;
    mutable std::string lastVec2Key;
    mutable int vec2Queries = 0;

    bool getAxisValue2D(const std::string& key,
                        double& outX, double& outY) const override {
        ++vec2Queries;
        lastVec2Key = key;
        if (!vec2Bound) {
            outX = 0.0;
            outY = 0.0;
            return false;
        }
        outX = vec2xReturn;
        outY = vec2yReturn;
        return true;
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

// ------------------------------------------------------------------
// M1 (2026-07-15): vec2 helpers + FVector2 reflect registration
// ------------------------------------------------------------------
//
// The helpers accept either keyed `{x=number, y=number}` or array-
// style `{number, number}` Lua tables; missing/non-numeric fields
// fall back to safe defaults (length → 0, normalized → passthrough).
// `input.vec2("move")` is exercised through the device-provider
// path in Test_LogiaDeviceInput.cpp; here we focus on the helpers
// and their tolerance for shape variance.

namespace {

constexpr const char* kVec2LengthWitness = R"(
script Vec2LengthWitness {
    on_update() {
        __vec2_witness = tostring(vec2.length({x=3, y=4}))
    }
}
)";

constexpr const char* kVec2NormalizedWitness = R"(
script Vec2NormalizedWitness {
    on_update() {
        var v: FVector2 = vec2.normalized({x=3, y=4})
        __vec2_norm_x = tostring(v.x)
        __vec2_norm_y = tostring(v.y)
    }
}
)";

constexpr const char* kVec2ArrayStyleWitness = R"(
script Vec2ArrayStyleWitness {
    on_update() {
        __vec2_array_style = tostring(vec2.length({3, 4}))
    }
}
)";

constexpr const char* kVec2DefaultWitness = R"(
script Vec2DefaultWitness {
    on_update() {
        __vec2_default = tostring(vec2.length({}))
    }
}
)";

} // namespace

TEST_CASE(vec2_length_basic_345_returns_5) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "Vec2LengthWitness",
                         kVec2LengthWitness, errors));
    CHECK(errors.empty());

    CHECK(bridge.callLifecycle("Vec2LengthWitness", "on_update",
                               nullptr, nullptr));
    CHECK(bridge.getLuaGlobalString("__vec2_witness") ==
          std::string("5.0"));
}

TEST_CASE(vec2_normalized_basic_returns_unit) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "Vec2NormalizedWitness",
                         kVec2NormalizedWitness, errors));
    CHECK(errors.empty());

    CHECK(bridge.callLifecycle("Vec2NormalizedWitness", "on_update",
                               nullptr, nullptr));
    // vec2.normalized mutates the input table; values are 3/5, 4/5.
    // Float formatting: lua's `tostring(0.6)` -> "0.6". Take the
    // prefix to absorb any precision wobble — R4.2 fixture wisdom.
    std::string xs = bridge.getLuaGlobalString("__vec2_norm_x");
    std::string ys = bridge.getLuaGlobalString("__vec2_norm_y");
    CHECK(xs.rfind("0.6", 0) == 0);
    CHECK(ys.rfind("0.8", 0) == 0);
}

TEST_CASE(vec2_normalized_zero_vector_passthrough) {
    // Zero vector must not produce NaN — helper returns the same
    // {x=0, y=0} table untouched.
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "Vec2NormalizedWitness",
                         kVec2NormalizedWitness, errors));
    CHECK(errors.empty());

    // Inject a zero-vector witness by re-loading a one-off script
    // that calls vec2.normalized on {x=0, y=0}. Use a fresh global
    // to avoid coupling to the prior test's witness globals.
    ScriptedInputProvider provider;
    bridge.setInputProvider(&provider);   // any provider works

    // Run via a minimal inline script — the compiler caches by
    // source so we'll just inline a fresh script body below.
    const char* zeroSource = R"(
script Vec2ZeroPassthrough {
    on_update() {
        var v: FVector2 = vec2.normalized({x=0, y=0})
        __zero_norm_x = tostring(v.x)
        __zero_norm_y = tostring(v.y)
    }
}
)";
    std::vector<CompilerError> zeroErr;
    CHECK(loadFromSource(bridge, "Vec2ZeroPassthrough",
                         zeroSource, zeroErr));
    CHECK(zeroErr.empty());
    CHECK(bridge.callLifecycle("Vec2ZeroPassthrough", "on_update",
                               nullptr, nullptr));
    CHECK(bridge.getLuaGlobalString("__zero_norm_x") ==
          std::string("0"));
    CHECK(bridge.getLuaGlobalString("__zero_norm_y") ==
          std::string("0"));
}

TEST_CASE(vec2_length_array_style_accepts_positional) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "Vec2ArrayStyleWitness",
                         kVec2ArrayStyleWitness, errors));
    CHECK(errors.empty());

    CHECK(bridge.callLifecycle("Vec2ArrayStyleWitness", "on_update",
                               nullptr, nullptr));
    CHECK(bridge.getLuaGlobalString("__vec2_array_style") ==
          std::string("5.0"));
}

TEST_CASE(vec2_length_empty_table_returns_zero_safe) {
    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;
    CHECK(loadFromSource(bridge, "Vec2DefaultWitness",
                         kVec2DefaultWitness, errors));
    CHECK(errors.empty());

    CHECK(bridge.callLifecycle("Vec2DefaultWitness", "on_update",
                               nullptr, nullptr));
    CHECK(bridge.getLuaGlobalString("__vec2_default") ==
          std::string("0.0"));
}

TEST_CASE(fvector2_reflect_registration_present) {
    // Spawning a SemanticAnalyzer triggers ensureAYEntityTypesRegistered,
    // which mirrors FVector3's registration block for FVector2 (x, y
    // Float32 fields). After init, the registry must expose a non-null
    // ITypeInfo for "FVector2" with two primitive fields.
    LogiaRuntimeBridge bridge;   // bridge ctor already triggers registration
    auto* info = ayt::reflect::TypeRegistryImpl::instance().findType("FVector2");
    CHECK(info != nullptr);
    CHECK(info->getFieldCount() == 2u);
    auto* xField = info->findField("x");
    auto* yField = info->findField("y");
    CHECK(xField != nullptr);
    CHECK(yField != nullptr);
    CHECK(std::string(xField->getType()->getName()) == "float");
    CHECK(std::string(yField->getType()->getName()) == "float");
}

TEST_SUITE_END
