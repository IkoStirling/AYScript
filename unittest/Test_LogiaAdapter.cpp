// Logia ↔ AYEntity IScriptBridge adapter unit tests (S2)
//
// Verifies that LogiaScriptBridgeAdapter:
//   - translates AYEntity's camelCase ("onStart" / "onUpdate" /
//     "onDestroy") to Logia's snake_case ("on_start" / etc.) and
//     forwards to the runtime bridge
//   - forwards hasScript correctly
//
// S2 limitation: AYEntity's AYScriptComponent.h triggers a static-init
// registrar (AY_COMPONENT macro) that requires a fully-defined
// World class. Including the full ScriptComponent definition in
// AYScript's translation units is fragile (the class is in
// AYScriptComponent.h, the registrar needs AYWorld.h, and including
// both produces forward-declaration conflicts). LogiaScriptBridgeAdapter
// is therefore NOT a subclass of IScriptBridge in S2 — instead it
// exposes the same call/hasScript surface as a regular class. The
// caller is expected to forward from AYEntity's ScriptComponent
// lifecycle (onAttach/onUpdate/onDetach) to `adapter.call()` with
// the appropriate `method` and `arg1` (ScriptComponent*) values.
// When S3 splits AYScriptComponent.h into class-only and registrar
// headers, the adapter will derive from IScriptBridge directly and
// this shim layer goes away.

#include "AYScript.h"
#include "AYScriptRuntimeBridge.h"
#include "AYScriptBridgeAdapter.h"
#include "AYTest.h"

// Full ScriptComponent + AYWorld definitions. The adapter cpp needs
// these to read getScriptName() and pass ScriptComponent* as receiver;
// the test mirrors that.
#include <AYWorld.h>
#include <components/AYScriptComponent.h>

#include <string>
#include <vector>

using ayt::entity::ScriptComponent;
using ayt::script::LogiaRuntimeBridge;
using ayt::script::LogiaScriptBridgeAdapter;
using ayt::script::logia::CompilerError;

namespace {

bool loadFromSource(LogiaRuntimeBridge& bridge,
                    const std::string& name,
                    const char* src)
{
    std::vector<CompilerError> errors;
    bool ok = bridge.loadScript(name, src, errors);
    return ok && errors.empty();
}

} // namespace

TEST_SUITE(LogiaAdapterTests)

// Helper: invoke the adapter exactly as AYEntity's ScriptComponent
// lifecycle methods would, by directly calling `adapter.call` with
// the same (method, arg1=this, arg2) tuple. We can't go through
// `comp.setBridge(&adapter)` in S2 because LogiaScriptBridgeAdapter
// does not (yet) derive from IScriptBridge — see header for rationale.
namespace {
struct AdapterCall {
    LogiaRuntimeBridge* bridge;
    LogiaScriptBridgeAdapter* adapter;
    ScriptComponent* comp;

    void onAttach(ayt::entity::Entity* entity) {
        adapter->call("onStart", comp, entity);
    }
    void onUpdate(float dt) {
        adapter->call("onUpdate", comp, &dt);
    }
    void onDetach() {
        adapter->call("onDestroy", comp, nullptr);
    }
};
} // namespace

TEST_CASE(adapter_maps_onStart_to_on_start) {
    LogiaRuntimeBridge bridge;
    LogiaScriptBridgeAdapter adapter(&bridge);

    // S2.5: `script` keyword; lifecycle functions take no params.
    const char* src = R"(
script Logger {
    on_start() {
        __test_witness = "on_start_called"
    }
}
)";
    CHECK(loadFromSource(bridge, "Logger", src));

    ScriptComponent comp;
    comp.setScriptName("Logger");
    AdapterCall ac{&bridge, &adapter, &comp};
    int dummyEntityStorage = 0;
    ac.onAttach(reinterpret_cast<ayt::entity::Entity*>(&dummyEntityStorage));

    CHECK(bridge.getLuaGlobalString("__test_witness") == "on_start_called");
}

TEST_CASE(adapter_maps_onUpdate_to_on_update_passes_dt) {
    LogiaRuntimeBridge bridge;
    LogiaScriptBridgeAdapter adapter(&bridge);

    const char* src = R"(
script Stepper {
    on_update() {
        __test_witness = "on_update_called"
    }
}
)";
    CHECK(loadFromSource(bridge, "Stepper", src));

    ScriptComponent comp;
    comp.setScriptName("Stepper");
    AdapterCall ac{&bridge, &adapter, &comp};
    ac.onUpdate(2.5f);
    CHECK(bridge.getLuaGlobalString("__test_witness") == "on_update_called");
}

TEST_CASE(adapter_maps_onDestroy_to_on_destroy) {
    LogiaRuntimeBridge bridge;
    LogiaScriptBridgeAdapter adapter(&bridge);

    const char* src = R"(
script Cleaner {
    on_destroy() {
        __test_witness = "destroyed"
    }
}
)";
    CHECK(loadFromSource(bridge, "Cleaner", src));

    ScriptComponent comp;
    comp.setScriptName("Cleaner");
    AdapterCall ac{&bridge, &adapter, &comp};
    ac.onDetach();
    CHECK(bridge.getLuaGlobalString("__test_witness") == "destroyed");
}

TEST_CASE(adapter_passes_script_component_as_self) {
    LogiaRuntimeBridge bridge;
    LogiaScriptBridgeAdapter adapter(&bridge);

    const char* src = R"(
script SelfProbe {
    on_start() {
        __test_witness = type(self)
    }
}
)";
    CHECK(loadFromSource(bridge, "SelfProbe", src));

    ScriptComponent comp;
    comp.setScriptName("SelfProbe");
    AdapterCall ac{&bridge, &adapter, &comp};
    int dummy = 0;
    ac.onAttach(reinterpret_cast<ayt::entity::Entity*>(&dummy));
    CHECK(bridge.getLuaGlobalString("__test_witness") == "userdata");
}

TEST_CASE(adapter_unknown_method_returns_false) {
    LogiaRuntimeBridge bridge;
    LogiaScriptBridgeAdapter adapter(&bridge);

    const char* src = R"(
script NoMethods {
}
)";
    CHECK(loadFromSource(bridge, "NoMethods", src));

    ScriptComponent comp;
    comp.setScriptName("NoMethods");
    CHECK_FALSE(adapter.call("onSomething", &comp, nullptr));
}

TEST_CASE(adapter_hasScript_delegates) {
    LogiaRuntimeBridge bridge;
    LogiaScriptBridgeAdapter adapter(&bridge);

    CHECK_FALSE(adapter.hasScript("Foo"));

    const char* src = R"(
script Foo {
}
)";
    CHECK(loadFromSource(bridge, "Foo", src));
    CHECK(adapter.hasScript("Foo"));
}

TEST_SUITE_END