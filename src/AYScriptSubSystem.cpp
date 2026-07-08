// AYScriptSubSystem.cpp - AYGameLoop subsystem for the Logia runtime

#include "AYScriptSubSystem.h"

#include "AYScriptBridgeAdapter.h"
#include "AYLogger.h"

// S3.1 (LG-04) and S3.4: drive Logia scripts (System + Component hosts)
// from the per-tick subsystem. World owns ISystem and Entity lists; we
// dispatch.
#include <AYWorld.h>
#include <AYEntityImpl.h>
#include <components/AYScriptComponent.h>

namespace ayt::script
{

namespace
{

// S3.1 (LG-04): walk the world's systems and invoke the Logia
// on_update for any system whose name matches a loaded Logia script.
// `systemPtr` is passed as lightuserdata `self` — System host scripts
// can read AYReflect fields on it (S3.1 keeps S2.5's
// self-as-lightuserdata convention).
void tickLogiaSystems(LogiaRuntimeBridge& bridge, float dt)
{
    auto& world = ayt::entity::World::instance();
    const size_t n = world.systemCount();
    for (size_t i = 0; i < n; ++i) {
        const char* name = world.getSystemNameAt(i);
        if (!name || !*name) continue;
        if (!bridge.hasScript(name)) continue;
        ayt::entity::ISystem* sys = world.findSystemByName(name);
        if (!sys) continue;  // raced with shutdown — skip
        bridge.callLifecycle(name, "on_update", static_cast<void*>(sys), &dt);
    }
}

} // namespace

ScriptSubSystem::ScriptSubSystem()
{
    _descriptor.name = "ayt.script.runtime";
    // S3.1 (LG-04): World is required for the System host tick path.
    _descriptor.dependencies = {"ayt.log", "ayt.entity"};
    _descriptor.basePriority = 100;
    _descriptor.timeType = ayt::game::SubSystemDescriptor::TimeType::Scaled;
}

ScriptSubSystem::~ScriptSubSystem() = default;

const ayt::game::SubSystemDescriptor& ScriptSubSystem::getDescriptor() const
{
    return _descriptor;
}

bool ScriptSubSystem::initialize()
{
    if (_initialized) return true;
    if (!_bridge.initialize()) {
        ayt::log::error("ScriptSubSystem: bridge.initialize() failed");
        return false;
    }
    // S3.4: create the adapter now that the bridge is live.
    if (!_adapter) {
        _adapter.reset(new LogiaScriptBridgeAdapter(&_bridge));
    }
    _initialized = true;
    ayt::log::info("ScriptSubSystem: ready (sol2 + Lua runtime online; "
                   "Component + System host tick paths active)");
    return true;
}

void ScriptSubSystem::update(float deltaTime)
{
    // S3.5: publish the per-tick scaled delta to the bridge BEFORE
    // any lifecycle dispatch. The `time.delta` / `time.total`
    // ambient table reads from the accumulator populated here —
    // ScriptSubSystem is the canonical source so all hosts (System
    // + Component) see the same authoritative value.
    _bridge.tickAmbient(deltaTime);
    // S3.1 (LG-04): drive Logia System-host scripts.
    tickLogiaSystems(_bridge, deltaTime);
    // S3.4: drive Logia Component-host scripts via the Entity loop.
    tickComponentHosts(deltaTime);
}

void ScriptSubSystem::fixedUpdate(float fixedDeltaTime)
{
    // S3.5: fixedUpdate also feeds the ambient accumulator so a
    // host that opts into fixed ticks (via SubSystemDescriptor)
    // sees the fixed dt in `time.delta` instead of the variable
    // one. Component-host Lifecycle dispatched from
    // tickComponentHosts inherits the same accumulator.
    _bridge.tickAmbient(fixedDeltaTime);
    // Same dispatch as update() — Logia System hosts opt into the
    // fixed tick via their host-side ISystem::setPriority / descriptor
    // wiring. Component host ticks get the fixed dt too.
    tickLogiaSystems(_bridge, fixedDeltaTime);
    tickComponentHosts(fixedDeltaTime);
}

void ScriptSubSystem::shutdown()
{
    if (!_initialized) return;
    _bridge.shutdown();
    _adapter.reset();
    _initialized = false;
    ayt::log::info("ScriptSubSystem: shutdown");
}

// ============================================================================
// S3.4 — Component host binding
// ============================================================================

bool ScriptSubSystem::bindAndLoad(ayt::entity::ScriptComponent& component,
                                  const std::string& scriptSource,
                                  std::vector<logia::CompilerError>& errors)
{
    if (!_initialized) {
        ayt::log::error("ScriptSubSystem::bindAndLoad called before initialize()");
        return false;
    }
    const char* name = component.getScriptName();
    if (name == nullptr || *name == '\0') {
        ayt::log::error("ScriptSubSystem::bindAndLoad: component scriptName is empty");
        return false;
    }
    // Step 1: compile + load the .logia source.
    if (!_bridge.loadScript(name, scriptSource, errors)) {
        // Bridge keeps diagnostic; component remains unbound.
        return false;
    }
    // Step 2: install adapter on the component. After this the
    // component's onAttach/onUpdate/onDetach lifecycle routes
    // through this subsystem. The adapter's `asScriptBridge()`
    // returns the AYEntity IScriptBridge* view — the public
    // AYScript header keeps the inheritance inside the cpp
    // (pimpl-style) to avoid dragging AYEntity into AYScript's
    // compile-time surface.
    component.setBridge(_adapter ? _adapter->asScriptBridge() : nullptr);
    return true;
}

void ScriptSubSystem::bindComponent(ayt::entity::ScriptComponent& component)
{
    if (!_adapter) return;
    component.setBridge(_adapter->asScriptBridge());
}

void ScriptSubSystem::tickComponentHosts(float deltaTime)
{
    if (!_initialized) return;
    auto& world = ayt::entity::World::instance();
    auto entities = world.getAllEntities();
    for (auto* e : entities) {
        if (e && e->isValid()) {
            e->onUpdate(deltaTime);
        }
    }
}

} // namespace ayt::script