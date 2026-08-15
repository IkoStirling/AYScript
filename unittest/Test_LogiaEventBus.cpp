// Test_LogiaEventBus.cpp — INT-04: ambient event.* ↔ EventBus aliases
//
// Covers design §14.5.1.5 test matrix rows:
//   - alias round-trip (Lua emit → C++ listener; C++ emit → Lua listener)
//   - unknown alias soft fail (no crash)
//   - unsubscribe stops delivery
// Does NOT route S4.1 signal/emit/connect through the bus (those stay
// in Test_LogiaRuntime / Test_LogiaSemantic).

#include "AYScriptRuntimeBridge.h"
#include "AYLogiaEventBridge.h"
#include "logia/AYCompilerError.h"
#include "AYTest.h"

#include <ayevent/EventBus.h>
#include <ayevent/Events/ScriptTestEvents.h>
#include <ayevent/Events/WindowEvents.h>

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
