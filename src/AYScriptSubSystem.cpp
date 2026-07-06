// AYScriptSubSystem.cpp - AYGameLoop subsystem for the Logia runtime

#include "AYScriptSubSystem.h"

#include "AYLogger.h"

namespace ayt::script
{

ScriptSubSystem::ScriptSubSystem()
{
    _descriptor.name = "ayt.script.runtime";
    // S3 will add AYInput/AYEntity/AYTime as dependencies once wired up.
    _descriptor.dependencies = {"ayt.log"};
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
    ayt::log::info("ScriptSubSystem: ready (sol2 + Lua runtime online)");
    return true;
}

void ScriptSubSystem::update(float /*deltaTime*/)
{
    // No-op for S1. Real dispatch comes in S3 once ScriptComponent is
    // integrated with ECS query iteration.
}

void ScriptSubSystem::fixedUpdate(float /*fixedDeltaTime*/)
{
    // Same as update() — S3 territory.
}

void ScriptSubSystem::shutdown()
{
    if (!_initialized) return;
    _bridge.shutdown();
    _initialized = false;
    ayt::log::info("ScriptSubSystem: shutdown");
}

} // namespace ayt::script