#pragma once
// AYLogiaEventBridge.h — INT-04: Logia ambient event.* → EventBus string aliases
//
// Does NOT route S4.1 signal/emit/connect through the bus (design §14.5.1).
// LogiaHostKind::EventHandler deferred to INT-04b.

#include <ayevent/EventBus.h>

#include <sol/sol.hpp>

namespace ayt::script
{

/// Register a minimal builtin alias set on `bus` (idempotent).
/// Aliases: window_resize, window_close, resource_ready, script_test_ping.
void registerBuiltinEventAliases(ayt::event::EventBus& bus);

/// Install ambient `event.emit` / `event.subscribe` / `event.unsubscribe`
/// into the Lua state. Soft-fails unknown aliases (log + skip / return 0).
void installLogiaEventAmbient(sol::state& lua, ayt::event::EventBus& bus);

} // namespace ayt::script
