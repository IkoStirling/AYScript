// AYScriptSubSystem.cpp - AYGameLoop subsystem for the Logia runtime

#include "AYScript/ScriptSubSystem.h"

#include "AYScript/ScriptBridgeAdapter.h"
#include "AYLog/Logger.h"

#include <AYGameLoop/SubSystemRegistry.h>

#include "AYIO.h"
#include <AYTime/Clock.h>

// S3.1 (LG-04) and S3.4: drive Logia scripts (System + Component hosts)
// from the per-tick subsystem. World owns ISystem and Entity lists; we
// dispatch.
#include <AYEntity/World.h>
#include <AYEntity/EntityImpl.h>
#include <AYEntity/components/ScriptComponent.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace ayt::script
{

namespace
{

constexpr int64_t kHotReloadDebounceMs = 100;

int64_t steadyClockMs()
{
    return static_cast<int64_t>(ayt::time::Clock::performanceNowUs() / 1000u);
}

// S3.1 (LG-04): walk the world's systems and invoke the Logia
// on_update for any system whose name matches a loaded Logia script.
void tickLogiaSystems(LogiaRuntimeBridge& bridge, float dt)
{
    auto& world = ayt::entity::World::instance();
    const size_t n = world.systemCount();
    for (size_t i = 0; i < n; ++i) {
        const char* name = world.getSystemNameAt(i);
        if (!name || !*name) continue;
        if (!bridge.hasScript(name)) continue;
        ayt::entity::ISystem* sys = world.findSystemByName(name);
        if (!sys) continue;
        bridge.callLifecycle(name, "on_update", static_cast<void*>(sys), &dt);
    }
}

} // namespace

// S3.7b — hot reload state (pimpl-style, kept in this TU only).
struct ScriptSubSystem::HotReloadState {
    struct Entry {
        std::string scriptName;
        std::string normalizedPath;
        logia::LogiaHostContext ctx;
        bool pendingReload = false;
        int64_t debounceStartMs = 0;
    };

    std::unique_ptr<ayt::io::FileWatcher> watcher;
    bool enabled = false;
    std::unordered_map<std::string, Entry> byPath;
    std::size_t applyCount = 0;
};

ScriptSubSystem::ScriptSubSystem()
{
    _descriptor.name = "ayt.script.runtime";
    // INT-02 (2026-07-15): keep the legacy, lenient "Device" lifecycle dep
    // so normal clients initialize after AYDevice. Also fix
    // INT-01 R3 silent dep mismatch — "Entity" name matches
    // EntitySubSystem::getName() (no "ayt." prefix). Topo-sort
    // silently ignores unmatched deps today, so this is purely a
    // documentation / future-proofing fix. Explicit phase ordering below
    // Device writes Input.TickFrame at a higher phase priority while Script
    // declares the matching read below, so the phase hazard planner orders
    // Device before Script whenever Device is registered. Do not add a strict
    // runsAfter edge here: editor hosts intentionally supply input through an
    // externally-owned DeviceManager and have no window-owning DeviceSubSystem.
    _descriptor.dependencies = {"ayt.log", "Entity", "Device"};
    _descriptor.basePriority = 100;
    _descriptor.timeType = ayt::game::SubSystemDescriptor::TimeType::Scaled;
    _descriptor.phases = ayt::game::phaseBit(ayt::game::FramePhase::FixedPrePhysics)
                       | ayt::game::phaseBit(ayt::game::FramePhase::Gameplay);
    _descriptor.clock = ayt::game::ClockDomain::Game;
    _descriptor.phasePriority = 100;
    _descriptor.reads = {"Input.TickFrame"};
    _descriptor.writes = {"Simulation.World"};
}

ScriptSubSystem::~ScriptSubSystem()
{
    shutdown();
}

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
    pollAndApplyReloads();
    _bridge.tickAmbient(deltaTime);
    tickLogiaSystems(_bridge, deltaTime);
    tickComponentHosts(deltaTime);
}

void ScriptSubSystem::fixedUpdate(float fixedDeltaTime)
{
    pollAndApplyReloads();
    _bridge.tickAmbient(fixedDeltaTime);
    tickLogiaSystems(_bridge, fixedDeltaTime);
    tickComponentHosts(fixedDeltaTime);
}

void ScriptSubSystem::tick(ayt::game::FramePhase phase,
                           const ayt::game::FrameContext& context)
{
    if (phase == ayt::game::FramePhase::FixedPrePhysics) {
        // Deterministic System hosts run exactly once per simulation tick.
        tickLogiaSystems(_bridge, context.fixedDeltaTime);
        return;
    }

    if (phase == ayt::game::FramePhase::Gameplay) {
        // File IO and ambient/visual scripts are variable-rate work. In a
        // headless host without Entity, this also supplies the component path.
        pollAndApplyReloads();
        _bridge.tickAmbient(context.deltaTime);
        tickComponentHosts(context.deltaTime);
    }
}

void ScriptSubSystem::shutdown()
{
    if (!_initialized && !_hotReload) {
        return;
    }

    stopHotReload();
    _hotReload.reset();

    if (_initialized) {
        // INT-02 (2026-07-15): unhook any injected InputProvider so
        // the bridge falls back to MockInputProvider before we tear
        // down. Host shutdown ordering (GameLoop::clearAll deletes
        // each SubSystem) means the DeviceInputProvider's
        // DeviceManager* can become dangling between EditorApp
        // destruction and SubSystem deletion; unhooking here is the
        // safe contract.
        _bridge.setInputProvider(nullptr);
        _bridge.shutdown();
        _adapter.reset();
        _initialized = false;
        ayt::log::info("ScriptSubSystem: shutdown");
    }
}

void ScriptSubSystem::stopHotReload()
{
    if (!_hotReload) {
        return;
    }
    if (_hotReload->watcher) {
        _hotReload->watcher->clearPending();
        _hotReload->watcher->stop();
        _hotReload->watcher.reset();
    }
    _hotReload->enabled = false;
}

void ScriptSubSystem::setHotReloadEnabled(bool enabled)
{
    if (!_hotReload) {
        _hotReload = std::make_unique<HotReloadState>();
    }
    if (_hotReload->enabled == enabled) {
        return;
    }

    if (!enabled) {
        stopHotReload();
        return;
    }

    _hotReload->watcher = std::make_unique<ayt::io::FileWatcher>();
    for (const auto& kv : _hotReload->byPath) {
        if (!_hotReload->watcher->watch(kv.first, nullptr)) {
            ayt::log::warn("ScriptSubSystem: hot reload watch failed for '%s'",
                           kv.first.c_str());
        }
    }
    _hotReload->watcher->start();
    _hotReload->enabled = true;
}

bool ScriptSubSystem::isHotReloadEnabled() const
{
    return _hotReload && _hotReload->enabled;
}

bool ScriptSubSystem::watchScriptPath(const std::string& scriptName,
                                     const std::string& filePath)
{
    return watchScriptPath(scriptName, filePath,
                           logia::defaultLogiaHostContext());
}

bool ScriptSubSystem::watchScriptPath(const std::string& scriptName,
                                     const std::string& filePath,
                                     const logia::LogiaHostContext& ctx)
{
    if (scriptName.empty() || filePath.empty()) {
        return false;
    }

    const std::string normalized = ayt::io::path::normalize(filePath);
    if (normalized.empty()) {
        return false;
    }

    if (!_hotReload) {
        _hotReload = std::make_unique<HotReloadState>();
    }

    HotReloadState::Entry entry;
    entry.scriptName = scriptName;
    entry.normalizedPath = normalized;
    entry.ctx = ctx;
    _hotReload->byPath[normalized] = std::move(entry);

    if (_hotReload->enabled && _hotReload->watcher) {
        return _hotReload->watcher->watch(normalized, nullptr);
    }
    return true;
}

bool ScriptSubSystem::unwatchScriptPath(const std::string& filePath)
{
    if (!_hotReload) {
        return false;
    }
    const std::string normalized = ayt::io::path::normalize(filePath);
    const auto it = _hotReload->byPath.find(normalized);
    if (it == _hotReload->byPath.end()) {
        return false;
    }
    _hotReload->byPath.erase(it);
    if (_hotReload->watcher) {
        _hotReload->watcher->unwatch(normalized);
    }
    return true;
}

bool ScriptSubSystem::bindAndLoadFromFile(
    ayt::entity::ScriptComponent& component,
    const std::string& filePath,
    std::vector<logia::CompilerError>& errors)
{
    if (!ayt::io::File::exists(filePath)) {
        ayt::log::error("ScriptSubSystem::bindAndLoadFromFile: file not found '%s'",
                        filePath.c_str());
        return false;
    }
    const std::string source = ayt::io::File::readAllText(filePath);
    if (!bindAndLoad(component, source, errors)) {
        return false;
    }
    if (!watchScriptPath(component.getScriptName(), filePath)) {
        ayt::log::warn("ScriptSubSystem::bindAndLoadFromFile: loaded but watch failed for '%s'",
                       filePath.c_str());
    }
    return true;
}

std::size_t ScriptSubSystem::hotReloadApplyCount() const
{
    return _hotReload ? _hotReload->applyCount : 0u;
}

void ScriptSubSystem::pollAndApplyReloads()
{
    if (!_initialized || !_hotReload || !_hotReload->enabled || !_hotReload->watcher) {
        return;
    }
    if (!_hotReload->watcher->isRunning()) {
        return;
    }

    const int64_t nowMs = steadyClockMs();
    std::vector<ayt::io::FileWatchEvent> events;
    _hotReload->watcher->pollPending(events);

    for (const ayt::io::FileWatchEvent& ev : events) {
        const std::string norm = ayt::io::path::normalize(ev.path);
        auto it = _hotReload->byPath.find(norm);
        if (it == _hotReload->byPath.end()) {
            continue;
        }
        if (ev.kind == ayt::io::FileWatchEvent::Kind::Deleted) {
            continue;
        }
        it->second.pendingReload = true;
        it->second.debounceStartMs = nowMs;
    }

    for (auto& kv : _hotReload->byPath) {
        HotReloadState::Entry& entry = kv.second;
        if (!entry.pendingReload) {
            continue;
        }
        if (nowMs - entry.debounceStartMs < kHotReloadDebounceMs) {
            continue;
        }
        entry.pendingReload = false;

        if (!ayt::io::File::exists(entry.normalizedPath)) {
            continue;
        }

        const std::string source =
            ayt::io::File::readAllText(entry.normalizedPath);
        if (source.empty()) {
            continue;
        }

        std::vector<logia::CompilerError> errors;
        if (_bridge.reloadScript(entry.scriptName, source, entry.ctx, errors)) {
            ++_hotReload->applyCount;
        }
    }
}

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
    if (!_bridge.loadScript(name, scriptSource, errors)) {
        return false;
    }
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

    // EntitySubSystem ("Entity") already calls World::update, which
    // walks every entity's onUpdate. Skip here to avoid double-ticking
    // ScriptComponents in full host builds; headless tests that do not
    // register "Entity" still rely on this path.
    if (ayt::game::SubSystemRegistry::instance().findSubSystem("Entity")
            != nullptr) {
        return;
    }

    auto& world = ayt::entity::World::instance();
    auto entities = world.getAllEntities();
    for (auto* e : entities) {
        if (e && e->isValid()) {
            e->onUpdate(deltaTime);
        }
    }
}

} // namespace ayt::script
