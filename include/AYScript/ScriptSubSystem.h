#pragma once
// AYScript/ScriptSubSystem.h - AYGameLoop subsystem for the Logia runtime (S3.4)

#include <AYGameLoop/IGameLoop.h>

#include "AYScript/ScriptRuntimeBridge.h"

#include <memory>
#include <string>
#include <vector>

namespace ayt::entity { class ScriptComponent; }
namespace ayt::entity { class Entity; }

namespace ayt::script
{

class LogiaScriptBridgeAdapter;

// ScriptSubSystem
//
// First concrete ISubSystem in the engine — establishes the pattern for
// future subsystems (e.g. RenderSubSystem, AudioSubSystem).
//
// S3.4 scope:
//   - Owns a LogiaRuntimeBridge
//   - Owns a LogiaScriptBridgeAdapter (sibling to the bridge) that
//     implements AYEntity's IScriptBridge. ScriptSubSystem hands the
//     adapter pointer to AYEntity's ScriptComponent instances via
//     bindComponent(); from that moment the component's onAttach /
//     onUpdate / onDetach lifecycle routes through the Logia runtime
//     and (S3.3) `self.<primitiveField>` writes mutate AY_PROPERTY fields.
//   - initialize(): bridge initialize + adapter instantiation.
//   - update() / fixedUpdate():
//       * S3.1 (LG-04) drives Logia System-host scripts. For each
//         ISystem in World whose getName() matches a loaded Logia
//         script, calls `_bridge.callLifecycle(name, "on_update",
//         systemPtr, &dt)`.
//       * S3.4 additionally walks World::getAllEntities() and calls
//         `entity->onUpdate(dt)` on each **only when** the "Entity"
//         subsystem is not registered. When EntitySubSystem is present
//         it already drives World::update; skipping the duplicate walk
//         avoids double-ticking ScriptComponents in full host builds.
//         Headless tests without "Entity" still use this path.
//   - shutdown(): bridge shutdown + reset adapter registry.
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

    // S3.4 — Component host binding + script loading.
    //
    // bindAndLoad() takes a caller-supplied ScriptComponent that
    // already has a scriptName set (via setScriptName("Foo")). It
    // (a) loads the script source into the bridge keyed by name and
    // (b) installs the adapter as the component's IScriptBridge so
    // the component's onStart/onUpdate/onDestroy lifecycle routes
    // into the Logia runtime.
    //
    // After this call returns true, `entity.onUpdate(dt)` (driven
    // either by World::update or by tests calling
    // ScriptSubSystem::tickComponentHosts(dt) directly) will run
    // the Logia script with `self` = the ScriptComponent pointer —
    // so self.<primitiveField> reads/writes mutate AY_PROPERTY.
    //
    // Returns false if the source failed to compile (the bridge
    // stores the diagnostics in `errors`).
    bool bindAndLoad(ayt::entity::ScriptComponent& component,
                     const std::string& scriptSource,
                     std::vector<logia::CompilerError>& errors);

    // Lower-level hook — set the adapter on a component that the
    // caller has already loaded into the bridge by hand. Useful
    // for stress tests that exercise path conventions
    // independently of source loading.
    void bindComponent(ayt::entity::ScriptComponent& component);

    // S3.4 — drive every entity's onUpdate path when the "Entity"
    // subsystem is absent (headless unittest / partial host). When
    // EntitySubSystem is registered, World::update already ticks
    // entities and this function returns immediately.
    void tickComponentHosts(float deltaTime);

    // Adapter handle so tests can drive the adapter path
    // independently (e.g. when bypassing the World loop). Not
    // declared override — IScriptBridge is owned by AYEntity; this
    // pointer is just the same object AYEntity will see.
    LogiaScriptBridgeAdapter* adapter() { return _adapter.get(); }

    // === S3.7b — hot reload coordinator (AYIO FileWatcher) ===

    // Enable/disable filesystem watch + automatic reloadScript on
    // .logia changes. Default off. When enabled, every path
    // registered via watchScriptPath / bindAndLoadFromFile is
    // watched; pollAndApplyReloads runs at the start of update().
    void setHotReloadEnabled(bool enabled);
    [[nodiscard]] bool isHotReloadEnabled() const;

    // Register a on-disk .logia path for hot reload under
    // `scriptName`. `filePath` is normalized via ayt::io::path.
    // Returns false if the path is invalid for OS watch setup.
    bool watchScriptPath(const std::string& scriptName,
                         const std::string& filePath);

    bool watchScriptPath(const std::string& scriptName,
                         const std::string& filePath,
                         const logia::LogiaHostContext& ctx);

    bool unwatchScriptPath(const std::string& filePath);

    // Read `filePath`, bindAndLoad, then register the path for
    // hot reload (Component host / default LogiaHostContext).
    bool bindAndLoadFromFile(ayt::entity::ScriptComponent& component,
                             const std::string& filePath,
                             std::vector<logia::CompilerError>& errors);

    // Number of successful reloadScript calls applied by the watcher
    // coordinator (observability for tests).
    [[nodiscard]] std::size_t hotReloadApplyCount() const;

private:
    void stopHotReload();
    void pollAndApplyReloads();

    struct HotReloadState;
    std::unique_ptr<HotReloadState> _hotReload;

    LogiaRuntimeBridge _bridge;
    // Owning — non-copyable because the adapter holds a raw
    // pointer to _bridge above. Single ownership keeps the
    // destruction order trivial (adapter dies first).
    std::unique_ptr<LogiaScriptBridgeAdapter> _adapter;

    ayt::game::SubSystemDescriptor _descriptor;
    bool _initialized = false;
};

} // namespace ayt::script