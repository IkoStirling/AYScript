#pragma once
// AYScriptSubSystem.h - AYGameLoop subsystem for the Logia runtime (S1)

#include <IAYGameLoop.h>

#include "AYScriptRuntimeBridge.h"

#include <memory>

namespace ayt::script
{

// ScriptSubSystem
//
// First concrete ISubSystem in the engine — establishes the pattern for
// future subsystems (e.g. RenderSubSystem, AudioSubSystem).
//
// S1 scope:
//   - Owns a LogiaRuntimeBridge
//   - initialize(): bridge initialize + log "ayt.script.runtime ready"
//   - update() / fixedUpdate(): no-op (real per-frame dispatch waits for
//     ECS + ScriptComponent integration in S3)
//   - shutdown(): bridge shutdown
//
// Intentionally NOT auto-registered via REGISTER_SUBSYSTEM in this TU —
// the host process must explicitly call
// IGameLoop::instance().registerSubSystem(new ScriptSubSystem())
// after AYGameLoop's IGameLoop::instance() is alive. Static-init
// registration would NPE because IGameLoop::instance() is a singleton
// whose construction depends on host order.
class ScriptSubSystem : public ayt::game::ISubSystem {
public:
    ScriptSubSystem();
    ~ScriptSubSystem() override;

    ScriptSubSystem(const ScriptSubSystem&) = delete;
    ScriptSubSystem& operator=(const ScriptSubSystem&) = delete;

    const char* getName() const override { return "ayt.script.runtime"; }
    const ayt::game::SubSystemDescriptor& getDescriptor() const override;

    bool initialize() override;
    void update(float deltaTime) override;
    void fixedUpdate(float fixedDeltaTime) override;
    void shutdown() override;

    LogiaRuntimeBridge& bridge() { return _bridge; }
    const LogiaRuntimeBridge& bridge() const { return _bridge; }

private:
    LogiaRuntimeBridge _bridge;
    ayt::game::SubSystemDescriptor _descriptor;
    bool _initialized = false;
};

} // namespace ayt::script