// Test_LogiaEventBus.cpp — INT-04: ambient event.* ↔ EventBus aliases
//
// Covers design §14.5.1.5 test matrix rows:
//   - alias round-trip (Lua emit → C++ listener; C++ emit → Lua listener)
//   - unknown alias soft fail (no crash)
//   - unsubscribe stops delivery
// Does NOT route S4.1 signal/emit/connect through the bus (those stay
// in Test_LogiaRuntime / Test_LogiaSemantic).

#include "AYScript/ScriptRuntimeBridge.h"
#include "AYScript/LogiaEventBridge.h"
#include "AYScript/logia/CompilerError.h"
#include "AYTest.h"

#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/SceneEvents.h>
#include <AYEventSystem/Events/ScriptTestEvents.h>
#include <AYEventSystem/Events/WindowEvents.h>

#include <string>
#include <vector>

using ayt::script::LogiaRuntimeBridge;
using ayt::script::logia::CompilerError;
using ayt::event::EventBus;
using ayt::event::ScriptTestPingEvent;
using ayt::event::WindowResizeEvent;

namespace {

void resetSingletonBus()
{
    // INT-04 install registers aliases on EventBus::instance(). Tests
    // share that singleton with other suites — clear listeners so
    // leftovers do not leak across cases. Alias map is left intact
    // (idempotent re-register on next bridge ctor).
    EventBus::instance().unsubscribeAll();
    EventBus::instance().resetCounters();
}

bool loadOk(LogiaRuntimeBridge& bridge, const char* name, const char* src)
{
    std::vector<CompilerError> errors;
    const bool ok = bridge.loadScript(name, src, errors);
    return ok && errors.empty();
}

} // namespace

TEST_SUITE(Logia_INT04_EventBus)

TEST_CASE(int04_lua_emit_reaches_cpp_listener)
{
    resetSingletonBus();

    int gotW = 0;
    int gotH = 0;
    EventBus::instance().subscribeByAlias<WindowResizeEvent>(
        "window_resize",
        [&](const WindowResizeEvent& e) {
            gotW = e.width;
            gotH = e.height;
        });

    LogiaRuntimeBridge bridge;
    constexpr const char* kSrc = R"(
script Emitter {
    on_start() {
        event.emit("window_resize", 1280, 720)
    }
}
)";
    CHECK(loadOk(bridge, "Emitter", kSrc));
    CHECK(bridge.callLifecycle("Emitter", "on_start"));
    CHECK_INT_EQ(gotW, 1280);
    CHECK_INT_EQ(gotH, 720);

    resetSingletonBus();
}

TEST_CASE(int04_lua_emit_table_payload)
{
    resetSingletonBus();

    int gotW = 0;
    int gotH = 0;
    EventBus::instance().subscribeByAlias<WindowResizeEvent>(
        "window_resize",
        [&](const WindowResizeEvent& e) {
            gotW = e.width;
            gotH = e.height;
        });

    LogiaRuntimeBridge bridge;
    constexpr const char* kSrc = R"(
script EmitterTable {
    on_start() {
        event.emit("window_resize", {width = 800, height = 600})
    }
}
)";
    CHECK(loadOk(bridge, "EmitterTable", kSrc));
    CHECK(bridge.callLifecycle("EmitterTable", "on_start"));
    CHECK_INT_EQ(gotW, 800);
    CHECK_INT_EQ(gotH, 600);

    resetSingletonBus();
}

TEST_CASE(int04_cpp_emit_reaches_lua_listener)
{
    resetSingletonBus();

    LogiaRuntimeBridge bridge;
    constexpr const char* kSrc = R"(
script Listener {
    function on_ping(v: int) {
        __witness_ping = v
    }
    on_start() {
        event.subscribe("script_test_ping", on_ping)
    }
}
)";
    CHECK(loadOk(bridge, "Listener", kSrc));
    CHECK(bridge.callLifecycle("Listener", "on_start"));

    CHECK(EventBus::instance().emitByAlias<ScriptTestPingEvent>(
        "script_test_ping", ScriptTestPingEvent{42}));

    double witness = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_ping", witness));
    CHECK_INT_EQ(static_cast<int>(witness), 42);

    resetSingletonBus();
}

TEST_CASE(int04_unsubscribe_stops_delivery)
{
    resetSingletonBus();

    LogiaRuntimeBridge bridge;
    constexpr const char* kSrc = R"(
script Unsub {
    function on_ping(v: int) {
        __witness_count = __witness_count + v
    }
    on_start() {
        __witness_count = 0
        var id: int = event.subscribe("script_test_ping", on_ping)
        event.emit("script_test_ping", 1)
        event.unsubscribe(id)
        event.emit("script_test_ping", 10)
    }
}
)";
    CHECK(loadOk(bridge, "Unsub", kSrc));
    CHECK(bridge.callLifecycle("Unsub", "on_start"));

    double count = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_count", count));
    // First emit delivered (+1); second after unsubscribe must not (+10).
    CHECK_INT_EQ(static_cast<int>(count), 1);

    resetSingletonBus();
}

TEST_CASE(int04_unknown_alias_soft_fail)
{
    resetSingletonBus();

    LogiaRuntimeBridge bridge;
    constexpr const char* kSrc = R"(
script SoftFail {
    function on_noop(v: int) {
        __witness_should_not = v
    }
    on_start() {
        __witness_emit_ok = event.emit("never_registered_alias_xyz", 1) and 1 or 0
        var id: int = event.subscribe("never_registered_alias_xyz", on_noop)
        __witness_sub_id = id
    }
}
)";
    CHECK(loadOk(bridge, "SoftFail", kSrc));
    CHECK(bridge.callLifecycle("SoftFail", "on_start"));

    double emitOk = -1.0;
    double subId = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_emit_ok", emitOk));
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_sub_id", subId));
    CHECK_INT_EQ(static_cast<int>(emitOk), 0);
    CHECK_INT_EQ(static_cast<int>(subId), 0);

    double shouldNot = 0.0;
    // Global may be missing — that is success (handler never ran).
    if (bridge.tryGetLuaGlobalNumber("__witness_should_not", shouldNot)) {
        CHECK_INT_EQ(static_cast<int>(shouldNot), 0);
    }

    resetSingletonBus();
}

TEST_CASE(int04_round_trip_script_test_ping)
{
    resetSingletonBus();

    LogiaRuntimeBridge bridge;
    constexpr const char* kSrc = R"(
script RoundTrip {
    function on_ping(v: int) {
        __witness_rt = v
    }
    on_start() {
        event.subscribe("script_test_ping", on_ping)
        event.emit("script_test_ping", 99)
    }
}
)";
    CHECK(loadOk(bridge, "RoundTrip", kSrc));
    CHECK(bridge.callLifecycle("RoundTrip", "on_start"));

    double witness = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_rt", witness));
    CHECK_INT_EQ(static_cast<int>(witness), 99);

    resetSingletonBus();
}

TEST_CASE(int04_device_action_and_task_complete_aliases)
{
    resetSingletonBus();

    LogiaRuntimeBridge bridge;
    constexpr const char* kSrc = R"(
script DeviceTask {
    function on_action(id: int, pressed: bool) {
        __witness_da = id
        __witness_dp = pressed and 1 or 0
    }
    function on_task(tid: int, ok: bool) {
        __witness_tid = tid
        __witness_tok = ok and 1 or 0
    }
    on_start() {
        event.subscribe("device_action", on_action)
        event.subscribe("task_complete", on_task)
        event.emit("device_action", 77, true)
        event.emit("task_complete", 9001, false)
    }
}
)";
    CHECK(loadOk(bridge, "DeviceTask", kSrc));
    CHECK(bridge.callLifecycle("DeviceTask", "on_start"));

    double da = -1, dp = -1, tid = -1, tok = -1;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_da", da));
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_dp", dp));
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_tid", tid));
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_tok", tok));
    CHECK_INT_EQ(static_cast<int>(da), 77);
    CHECK_INT_EQ(static_cast<int>(dp), 1);
    CHECK_INT_EQ(static_cast<int>(tid), 9001);
    CHECK_INT_EQ(static_cast<int>(tok), 0);

    resetSingletonBus();
}

TEST_CASE(int04_scene_begin_play_subscribe_only)
{
    resetSingletonBus();

    LogiaRuntimeBridge bridge;
    constexpr const char* kSrc = R"(
script SceneListener {
    function on_begin() {
        __witness_scene = 1
    }
    on_start() {
        event.subscribe("scene_begin_play", on_begin)
        __witness_emit = event.emit("scene_begin_play") and 1 or 0
    }
}
)";
    CHECK(loadOk(bridge, "SceneListener", kSrc));
    CHECK(bridge.callLifecycle("SceneListener", "on_start"));

    double emitOk = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_emit", emitOk));
    CHECK_INT_EQ(static_cast<int>(emitOk), 0);

    // C++ producer posts SceneBeginPlayEvent → Lua lightuserdata callback.
    int sentinel = 42;
    CHECK(EventBus::instance().emitByAlias<ayt::event::SceneBeginPlayEvent>(
        "scene_begin_play",
        ayt::event::SceneBeginPlayEvent{
            reinterpret_cast<ayt::scene::Scene*>(&sentinel)}));

    double hit = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_scene", hit));
    CHECK_INT_EQ(static_cast<int>(hit), 1);

    resetSingletonBus();
}

TEST_CASE(int04_physics_collision_alias)
{
    resetSingletonBus();

    LogiaRuntimeBridge bridge;
    constexpr const char* kSrc = R"(
script PhysListener {
    function on_hit(a: int, b: int, kind: int) {
        __witness_pa = a
        __witness_pb = b
        __witness_pk = kind
    }
    on_start() {
        event.subscribe("physics_collision", on_hit)
        event.emit("physics_collision", 11, 22, 1)
    }
}
)";
    CHECK(loadOk(bridge, "PhysListener", kSrc));
    CHECK(bridge.callLifecycle("PhysListener", "on_start"));

    double a = -1, b = -1, k = -1;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_pa", a));
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_pb", b));
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_pk", k));
    CHECK_INT_EQ(static_cast<int>(a), 11);
    CHECK_INT_EQ(static_cast<int>(b), 22);
    CHECK_INT_EQ(static_cast<int>(k), 1);

    resetSingletonBus();
}

TEST_CASE(int04_s41_signal_not_routed_to_bus)
{
    // Prove S4.1 emit stays domain-local: a bus listener on a typed
    // alias must NOT fire when the script uses bare emit("name", …).
    resetSingletonBus();

    int busHits = 0;
    EventBus::instance().subscribeByAlias<ScriptTestPingEvent>(
        "script_test_ping",
        [&](const ScriptTestPingEvent&) { ++busHits; });

    LogiaRuntimeBridge bridge;
    constexpr const char* kSrc = R"(
script LocalSignal {
    signal ping(v: int)
    function on_local(v: int) {
        __witness_local = v
    }
    on_start() {
        connect("ping", on_local)
        emit("ping", 7)
    }
}
)";
    CHECK(loadOk(bridge, "LocalSignal", kSrc));
    CHECK(bridge.callLifecycle("LocalSignal", "on_start"));

    double local = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_local", local));
    CHECK_INT_EQ(static_cast<int>(local), 7);
    CHECK_INT_EQ(busHits, 0);

    resetSingletonBus();
}

TEST_SUITE_END
