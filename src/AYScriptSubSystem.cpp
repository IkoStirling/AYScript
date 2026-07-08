// AYScriptSubSystem.cpp - AYGameLoop subsystem for the Logia runtime

#include "AYScriptSubSystem.h"

#include "AYLogger.h"

// S3.1 (LG-04): drive Logia System-host scripts from the per-tick
// subsystem. World owns the ISystem instances; we just dispatch.
#include <AYWorld.h>

namespace ayt::script
{

namespace
{

// Walk the world's systems and invoke the Logia on_update for any system
// whose name matches a loaded Logia script. The `systemPtr` is passed
// as lightuserdata `self` — System host scripts can read AYReflect
// fields on it (S3.1 keeps S2.5's self-as-lightuserdata convention).
// The receiver pointer is informational only in LG-04; future S3.x
// will route self.field through a usertype binding (see design.md
// §5.3 / Phase S3 backlog).
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
    _initialized = true;
    ayt::log::info("ScriptSubSystem: ready (sol2 + Lua runtime online; "
                   "System host tick path active)");
    return true;
}

void ScriptSubSystem::update(float deltaTime)
{
    // S3.1 (LG-04): drive Logia System-host scripts. World must be
    // initialized by the host before this point (handled by ordering
    // on the GameLoop descriptor).
    tickLogiaSystems(_bridge, deltaTime);
}

void ScriptSubSystem::fixedUpdate(float fixedDeltaTime)
{
    // Same dispatch as update() — Logia System hosts opt into the fixed
    // tick via their host-side ISystem::setPriority / descriptor wiring.
    tickLogiaSystems(_bridge, fixedDeltaTime);
}

void ScriptSubSystem::shutdown()
{
    if (!_initialized) return;
    _bridge.shutdown();
    _initialized = false;
    ayt::log::info("ScriptSubSystem: shutdown");
}

} // namespace ayt::script