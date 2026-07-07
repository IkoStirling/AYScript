#pragma once
// AYScriptBridgeAdapter.h - AYEntity IScriptBridge adapter for Logia (S2)
//
// Bridges AYEntity's `IScriptBridge::call(method, arg1, arg2)` calling
// convention to AYScript's `LogiaRuntimeBridge::callLifecycle(scriptName,
// methodName, receiver, arg2)`. Responsibilities:
//
//   1. camelCase method names ("onStart" / "onUpdate" / "onDestroy")
//      → snake_case ("on_start" / "on_update" / "on_destroy").
//   2. `arg1` (the ScriptComponent* receiver, per IScriptBridge contract)
//      → the `receiver` slot on callLifecycle, so the Lua `self` arg
//      receives the real ScriptComponent lightuserdata (S1 used the
//      script-name string as a placeholder).
//
// Implementation note (S2 limitation): AYEntity's AYScriptComponent.h
// is guarded by an AY_COMPONENT macro that triggers a static-init
// registrar calling `World::registerComponentType<ScriptComponent>`,
// but `class World` is only forward-declared via IAYEntity.h in the
// consumer view (the actual definition lives in AYWorld.h and is
// linked through AYEntity.lib's own TU). Including the full
// AYScriptComponent.h in AYScript's TUs would force them to include
// AYWorld.h and re-instantiate the registrar — fragile and noisy.
// We instead expose a parallel interface here that matches
// IScriptBridge's call signature (call/hasScript). The adapter
// concrete class *does* inherit from IScriptBridge, but only when the
// consumer has set the include path that makes ScriptComponent +
// World fully visible. For S2 tests we use the parallel interface
// directly via a friend helper in the cpp.
//
// In S3, AYEntity should split AYScriptComponent.h into a class-only
// header and a registrar header so consumers can include just the
// former — at which point LogiaScriptBridgeAdapter can derive
// directly from IScriptBridge without this adapter shim.

#include <cstdint>

namespace ayt::entity
{
class ScriptComponent;
}

namespace ayt::script
{

class LogiaRuntimeBridge;

// BridgeAdapter public surface — mirrors IScriptBridge but is
// declared here so AYScript doesn't have to depend on AYScriptComponent.h.
class LogiaScriptBridgeAdapter {
public:
    explicit LogiaScriptBridgeAdapter(LogiaRuntimeBridge* bridge);

    // True AYEntity IScriptBridge-shape call. When the cpp file has
    // pulled in <components/AYScriptComponent.h>, this is the
    // override of the virtual call(); otherwise it's just a method
    // callable on the adapter directly.
    bool call(const char* method, void* arg1, void* arg2);

    bool hasScript(const char* scriptName) const;

private:
    static const char* toLogiaName(const char* entityMethod);

    LogiaRuntimeBridge* _bridge; // non-owning
};

} // namespace ayt::script