// AYLogiaEventBridge.cpp — INT-04 / INT-04b Logia event.* ambient

#include "AYLogiaEventBridge.h"

#include <ayevent/Events/DeviceEvents.h>
#include <ayevent/Events/PhysicsEvents.h>
#include <ayevent/Events/ResourceEvents.h>
#include <ayevent/Events/SceneEvents.h>
#include <ayevent/Events/ScriptTestEvents.h>
#include <ayevent/Events/TaskEvents.h>
#include <ayevent/Events/WindowEvents.h>

#include <AYLog.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ayt::script
{
namespace {

bool readIntArg(const sol::object& o, int& out)
{
    if (!o.valid()) {
        return false;
    }
    if (o.is<int>()) {
        out = o.as<int>();
        return true;
    }
    if (o.is<double>()) {
        out = static_cast<int>(o.as<double>());
        return true;
    }
    return false;
}

bool readU64Arg(const sol::object& o, uint64_t& out)
{
    if (!o.valid()) {
        return false;
    }
    if (o.is<double>()) {
        out = static_cast<uint64_t>(o.as<double>());
        return true;
    }
    if (o.is<int>()) {
        out = static_cast<uint64_t>(o.as<int>());
        return true;
    }
    return false;
}

bool packWindowResize(sol::variadic_args args, ayt::event::WindowResizeEvent& out)
{
    // Positional: emit("window_resize", w, h)
    if (args.size() >= 2) {
        int w = 0;
        int h = 0;
        if (readIntArg(args[0], w) && readIntArg(args[1], h)) {
            out.width = w;
            out.height = h;
            return true;
        }
    }
    // Table: emit("window_resize", {width=…, height=…}) or {w=, h=}
    if (args.size() >= 1 && args[0].is<sol::table>()) {
        sol::table t = args[0];
        sol::optional<int> w = t["width"];
        sol::optional<int> h = t["height"];
        if (!w) w = t["w"];
        if (!h) h = t["h"];
        if (w && h) {
            out.width = *w;
            out.height = *h;
            return true;
        }
    }
    return false;
}

bool packScriptTestPing(sol::variadic_args args, ayt::event::ScriptTestPingEvent& out)
{
    if (args.size() >= 1) {
        int v = 0;
        if (readIntArg(args[0], v)) {
            out.value = v;
            return true;
        }
    }
    if (args.size() >= 1 && args[0].is<sol::table>()) {
        sol::table t = args[0];
        sol::optional<int> v = t["value"];
        if (v) {
            out.value = *v;
            return true;
        }
    }
    out.value = 0;
    return true; // allow empty payload → value 0
}

bool packResourceReady(sol::variadic_args args, ayt::event::ResourceLoadCompleteEvent& out)
{
    // emit("resource_ready", handle [, refCount [, ok]])
    if (args.size() >= 1) {
        if (args[0].is<double>()) {
            out.handle = static_cast<uint64_t>(args[0].as<double>());
        } else if (args[0].is<int>()) {
            out.handle = static_cast<uint64_t>(args[0].as<int>());
        } else {
            return false;
        }
        out.refCount = 1;
        out.ok = true;
        if (args.size() >= 2) {
            int rc = 1;
            if (readIntArg(args[1], rc)) {
                out.refCount = rc;
            }
        }
        if (args.size() >= 3 && args[2].is<bool>()) {
            out.ok = args[2].as<bool>();
        }
        return true;
    }
    return false;
}

bool packDeviceAction(sol::variadic_args args, ayt::event::DeviceActionEvent& out)
{
    // emit("device_action", actionId, pressed)
    if (args.size() >= 2) {
        int id = 0;
        if (!readIntArg(args[0], id)) {
            return false;
        }
        out.actionId = id;
        if (args[1].is<bool>()) {
            out.pressed = args[1].as<bool>();
            return true;
        }
        int pressed = 0;
        if (readIntArg(args[1], pressed)) {
            out.pressed = (pressed != 0);
            return true;
        }
    }
    return false;
}

bool packTaskComplete(sol::variadic_args args, ayt::event::TaskCompleteEvent& out)
{
    // emit("task_complete", taskId [, ok])
    if (args.size() >= 1) {
        uint64_t id = 0;
        if (!readU64Arg(args[0], id)) {
            return false;
        }
        out.taskId = id;
        out.ok = true;
        if (args.size() >= 2) {
            if (args[1].is<bool>()) {
                out.ok = args[1].as<bool>();
            } else {
                int v = 1;
                if (readIntArg(args[1], v)) {
                    out.ok = (v != 0);
                }
            }
        }
        return true;
    }
    return false;
}

bool packPhysicsCollision(sol::variadic_args args, ayt::event::PhysicsCollisionEvent& out)
{
    // emit("physics_collision", bodyA, bodyB [, kind])
    if (args.size() >= 2) {
        int a = 0;
        int b = 0;
        if (!readIntArg(args[0], a) || !readIntArg(args[1], b)) {
            return false;
        }
        out.bodyA = static_cast<uint32_t>(a);
        out.bodyB = static_cast<uint32_t>(b);
        out.kind = 0;
        if (args.size() >= 3) {
            int k = 0;
            if (readIntArg(args[2], k) && k >= 0 && k <= 2) {
                out.kind = static_cast<uint8_t>(k);
            }
        }
        return true;
    }
    return false;
}

} // namespace

void registerBuiltinEventAliases(ayt::event::EventBus& bus)
{
    bus.registerAlias<ayt::event::WindowResizeEvent>("window_resize");
    bus.registerAlias<ayt::event::WindowCloseEvent>("window_close");
    bus.registerAlias<ayt::event::ResourceLoadCompleteEvent>("resource_ready");
    bus.registerAlias<ayt::event::ScriptTestPingEvent>("script_test_ping");
    bus.registerAlias<ayt::event::DeviceActionEvent>("device_action");
    bus.registerAlias<ayt::event::TaskCompleteEvent>("task_complete");
    bus.registerAlias<ayt::event::SceneCurrentChangedEvent>("scene_current_changed");
    bus.registerAlias<ayt::event::SceneBeginPlayEvent>("scene_begin_play");
    bus.registerAlias<ayt::event::SceneEndPlayEvent>("scene_end_play");
    bus.registerAlias<ayt::event::PhysicsCollisionEvent>("physics_collision");
}

void installLogiaEventAmbient(sol::state& lua, ayt::event::EventBus& bus)
{
    registerBuiltinEventAliases(bus);

    auto eventTbl = lua.create_table();

    eventTbl["emit"] = [&bus](const std::string& alias, sol::variadic_args args) -> bool {
        if (alias == "window_resize") {
            ayt::event::WindowResizeEvent ev{};
            if (!packWindowResize(args, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): bad args", alias.c_str());
                return false;
            }
            if (!bus.emitByAlias<ayt::event::WindowResizeEvent>(alias, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): unknown alias", alias.c_str());
                return false;
            }
            return true;
        }
        if (alias == "window_close") {
            ayt::event::WindowCloseEvent ev{};
            if (!bus.emitByAlias<ayt::event::WindowCloseEvent>(alias, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): unknown alias", alias.c_str());
                return false;
            }
            return true;
        }
        if (alias == "resource_ready") {
            ayt::event::ResourceLoadCompleteEvent ev{};
            if (!packResourceReady(args, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): bad args", alias.c_str());
                return false;
            }
            if (!bus.emitByAlias<ayt::event::ResourceLoadCompleteEvent>(alias, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): unknown alias", alias.c_str());
                return false;
            }
            return true;
        }
        if (alias == "script_test_ping") {
            ayt::event::ScriptTestPingEvent ev{};
            if (!packScriptTestPing(args, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): bad args", alias.c_str());
                return false;
            }
            if (!bus.emitByAlias<ayt::event::ScriptTestPingEvent>(alias, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): unknown alias", alias.c_str());
                return false;
            }
            return true;
        }
        if (alias == "device_action") {
            ayt::event::DeviceActionEvent ev{};
            if (!packDeviceAction(args, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): bad args", alias.c_str());
                return false;
            }
            if (!bus.emitByAlias<ayt::event::DeviceActionEvent>(alias, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): unknown alias", alias.c_str());
                return false;
            }
            return true;
        }
        if (alias == "task_complete") {
            ayt::event::TaskCompleteEvent ev{};
            if (!packTaskComplete(args, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): bad args", alias.c_str());
                return false;
            }
            if (!bus.emitByAlias<ayt::event::TaskCompleteEvent>(alias, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): unknown alias", alias.c_str());
                return false;
            }
            return true;
        }
        if (alias == "physics_collision") {
            ayt::event::PhysicsCollisionEvent ev{};
            if (!packPhysicsCollision(args, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): bad args", alias.c_str());
                return false;
            }
            if (!bus.emitByAlias<ayt::event::PhysicsCollisionEvent>(alias, ev)) {
                ayt::log::warn("[INT-04] event.emit(\"%s\"): unknown alias", alias.c_str());
                return false;
            }
            return true;
        }
        // Scene* payloads are opaque — Lua emit not supported (subscribe-only).
        if (alias == "scene_current_changed" || alias == "scene_begin_play"
            || alias == "scene_end_play") {
            ayt::log::warn(
                "[INT-04] event.emit(\"%s\"): Scene* payload is subscribe-only "
                "(soft fail)",
                alias.c_str());
            return false;
        }

        ayt::log::warn("[INT-04] event.emit(\"%s\"): unknown alias (soft fail)", alias.c_str());
        return false;
    };

    eventTbl["subscribe"] = [&bus](const std::string& alias,
                                   sol::protected_function fn) -> double {
        if (!fn.valid()) {
            ayt::log::warn("[INT-04] event.subscribe(\"%s\"): invalid handler", alias.c_str());
            return 0.0;
        }

        // Keep a shared_ptr copy so the listener outlives the sol stack.
        auto holder = std::make_shared<sol::protected_function>(std::move(fn));

        ayt::event::ConnectionId id = ayt::event::kInvalidConnectionId;

        if (alias == "window_resize") {
            id = bus.subscribeByAlias<ayt::event::WindowResizeEvent>(
                alias,
                [holder](const ayt::event::WindowResizeEvent& e) {
                    sol::protected_function_result r = (*holder)(e.width, e.height);
                    if (!r.valid()) {
                        sol::error err = r;
                        ayt::log::error("[INT-04] window_resize handler: %s", err.what());
                    }
                });
        } else if (alias == "window_close") {
            id = bus.subscribeByAlias<ayt::event::WindowCloseEvent>(
                alias,
                [holder](const ayt::event::WindowCloseEvent&) {
                    sol::protected_function_result r = (*holder)();
                    if (!r.valid()) {
                        sol::error err = r;
                        ayt::log::error("[INT-04] window_close handler: %s", err.what());
                    }
                });
        } else if (alias == "resource_ready") {
            id = bus.subscribeByAlias<ayt::event::ResourceLoadCompleteEvent>(
                alias,
                [holder](const ayt::event::ResourceLoadCompleteEvent& e) {
                    sol::protected_function_result r =
                        (*holder)(static_cast<double>(e.handle), e.refCount, e.ok);
                    if (!r.valid()) {
                        sol::error err = r;
                        ayt::log::error("[INT-04] resource_ready handler: %s", err.what());
                    }
                });
        } else if (alias == "script_test_ping") {
            id = bus.subscribeByAlias<ayt::event::ScriptTestPingEvent>(
                alias,
                [holder](const ayt::event::ScriptTestPingEvent& e) {
                    sol::protected_function_result r = (*holder)(e.value);
                    if (!r.valid()) {
                        sol::error err = r;
                        ayt::log::error("[INT-04] script_test_ping handler: %s", err.what());
                    }
                });
        } else if (alias == "device_action") {
            id = bus.subscribeByAlias<ayt::event::DeviceActionEvent>(
                alias,
                [holder](const ayt::event::DeviceActionEvent& e) {
                    sol::protected_function_result r = (*holder)(e.actionId, e.pressed);
                    if (!r.valid()) {
                        sol::error err = r;
                        ayt::log::error("[INT-04] device_action handler: %s", err.what());
                    }
                });
        } else if (alias == "task_complete") {
            id = bus.subscribeByAlias<ayt::event::TaskCompleteEvent>(
                alias,
                [holder](const ayt::event::TaskCompleteEvent& e) {
                    sol::protected_function_result r =
                        (*holder)(static_cast<double>(e.taskId), e.ok);
                    if (!r.valid()) {
                        sol::error err = r;
                        ayt::log::error("[INT-04] task_complete handler: %s", err.what());
                    }
                });
        } else if (alias == "physics_collision") {
            id = bus.subscribeByAlias<ayt::event::PhysicsCollisionEvent>(
                alias,
                [holder](const ayt::event::PhysicsCollisionEvent& e) {
                    sol::protected_function_result r = (*holder)(
                        static_cast<double>(e.bodyA),
                        static_cast<double>(e.bodyB),
                        static_cast<int>(e.kind));
                    if (!r.valid()) {
                        sol::error err = r;
                        ayt::log::error("[INT-04] physics_collision handler: %s", err.what());
                    }
                });
        } else if (alias == "scene_current_changed") {
            // Scene* → lightuserdata (opaque). No full Scene usertype.
            id = bus.subscribeByAlias<ayt::event::SceneCurrentChangedEvent>(
                alias,
                [holder](const ayt::event::SceneCurrentChangedEvent& e) {
                    sol::protected_function_result r =
                        (*holder)(static_cast<void*>(e.current));
                    if (!r.valid()) {
                        sol::error err = r;
                        ayt::log::error("[INT-04] scene_current_changed handler: %s",
                                        err.what());
                    }
                });
        } else if (alias == "scene_begin_play") {
            id = bus.subscribeByAlias<ayt::event::SceneBeginPlayEvent>(
                alias,
                [holder](const ayt::event::SceneBeginPlayEvent& e) {
                    sol::protected_function_result r =
                        (*holder)(static_cast<void*>(e.play));
                    if (!r.valid()) {
                        sol::error err = r;
                        ayt::log::error("[INT-04] scene_begin_play handler: %s", err.what());
                    }
                });
        } else if (alias == "scene_end_play") {
            id = bus.subscribeByAlias<ayt::event::SceneEndPlayEvent>(
                alias,
                [holder](const ayt::event::SceneEndPlayEvent& e) {
                    sol::protected_function_result r =
                        (*holder)(static_cast<void*>(e.edit));
                    if (!r.valid()) {
                        sol::error err = r;
                        ayt::log::error("[INT-04] scene_end_play handler: %s", err.what());
                    }
                });
        } else {
            ayt::log::warn("[INT-04] event.subscribe(\"%s\"): unknown alias (soft fail)",
                           alias.c_str());
            return 0.0;
        }

        return static_cast<double>(id);
    };

    eventTbl["unsubscribe"] = [&bus](double idNum) {
        const auto id = static_cast<ayt::event::ConnectionId>(idNum);
        if (id == ayt::event::kInvalidConnectionId) {
            return;
        }
        bus.unsubscribe(id);
    };

    lua["event"] = eventTbl;
}

} // namespace ayt::script
