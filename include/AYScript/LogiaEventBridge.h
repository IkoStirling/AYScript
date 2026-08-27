#pragma once
// AYScript/LogiaEventBridge.h — INT-04 / INT-04b: Logia ambient event.* → EventBus
//
// Does NOT route S4.1 signal/emit/connect through the bus (design §14.5.1).
// LogiaHostKind::EventHandler: see eventHandlerLogiaHostContext() +
// LogiaRuntimeBridge::loadEventHandler().

#include <AYEventSystem/EventBus.h>

#include <sol/sol.hpp>

#include <functional>
#include <string>

namespace ayt::script
{

struct LogiaEventHooks {
    // EventBus publishers may run on worker threads. The callback must verify
    // runtime affinity before touching sol::function or any Lua-owned state.
    std::function<bool()> isRuntimeThread;
    std::function<void()> resetExecutionBudget;
    std::function<std::string()> currentOwner;
    std::function<void(const std::string&, ayt::event::ConnectionId)> onSubscribed;
    std::function<void(ayt::event::ConnectionId)> onUnsubscribed;
    std::function<void(const std::string&)> enterOwner;
    std::function<void()> leaveOwner;
};

/// Register builtin aliases on `bus` (idempotent).
/// Aliases: window_resize, window_close, resource_ready, script_test_ping,
/// device_action, task_complete, scene_current_changed, scene_begin_play,
/// scene_end_play, physics_collision.
void registerBuiltinEventAliases(ayt::event::EventBus& bus);

/// Install ambient `event.emit` / `event.subscribe` / `event.unsubscribe`
/// into the Lua state. Soft-fails unknown aliases (log + skip / return 0).
/// Scene aliases are subscribe-only (Scene* as lightuserdata); Lua emit soft-fails.
void installLogiaEventAmbient(sol::state& lua,
                              ayt::event::EventBus& bus,
                              LogiaEventHooks hooks = {});

} // namespace ayt::script
