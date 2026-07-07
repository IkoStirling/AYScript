// AYScriptBridgeAdapter.cpp

#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include "AYScriptBridgeAdapter.h"
#include "AYScriptRuntimeBridge.h"

// The adapter needs the full ScriptComponent definition so it can
// read .getScriptName() to forward to the runtime bridge (which keys
// its chunk cache by name). AYEntity's AYScriptComponent.h has a
// side-effect macro (AY_COMPONENT) that registers ScriptComponent
// with `World::registerComponentType<T>`. AYEntity's IAYEntity.h
// only forward-declares World — so we include AYWorld.h here for
// the full definition. AYEntity.lib already contains the actual
// registrar (AYEntityReflection.cpp); the per-TU static we emit here
// is benign because the registrar is idempotent and AYEntity's own
// static init runs first. In S3 we plan to split AYScriptComponent.h
// so this layering cleanup is no longer needed.
#include <AYWorld.h>
#include <components/AYScriptComponent.h>

#include <cstring>

namespace ayt::script
{

LogiaScriptBridgeAdapter::LogiaScriptBridgeAdapter(LogiaRuntimeBridge* bridge)
    : _bridge(bridge)
{
}

const char* LogiaScriptBridgeAdapter::toLogiaName(const char* m)
{
    if (m == nullptr) return nullptr;
    if (std::strcmp(m, "onStart")   == 0) return "on_start";
    if (std::strcmp(m, "onUpdate")  == 0) return "on_update";
    if (std::strcmp(m, "onDestroy") == 0) return "on_destroy";
    return m; // unknown — bridge will return false (script not found)
}

bool LogiaScriptBridgeAdapter::call(const char* method, void* arg1, void* arg2)
{
    if (_bridge == nullptr || method == nullptr) return false;

    // AYEntity's IScriptBridge contract: `arg1` is the ScriptComponent*.
    // We forward it directly to the runtime bridge as `receiver`. The
    // runtime bridge passes it through as Lua lightuserdata, so the
    // script's `self` parameter receives the actual component.
    auto* receiver = static_cast<ayt::entity::ScriptComponent*>(arg1);

    // The script name lives on the ScriptComponent; the bridge keys
    // its chunk cache by it. Read it here so we know which module to
    // invoke. (ScriptComponent::getScriptName returns "" when unset,
    // in which case the bridge correctly returns false.)
    const char* scriptName = receiver ? receiver->getScriptName() : "";

    return _bridge->callLifecycle(scriptName, toLogiaName(method),
                                  /*receiver*/ arg1, arg2);
}

bool LogiaScriptBridgeAdapter::hasScript(const char* scriptName) const
{
    if (_bridge == nullptr) return false;
    return _bridge->hasScript(scriptName ? scriptName : "");
}

} // namespace ayt::script