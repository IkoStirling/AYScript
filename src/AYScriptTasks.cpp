#include "ScriptTasks.h"
#include <AYEventSystem/EventBus.h>
#include <cmath>
#include <stdexcept>
namespace ayt::script
{
namespace
{
void taskBudgetHook(lua_State *state, lua_Debug *)
{
    luaL_error(state, "Logia task instruction budget exceeded");
}
int resumeTask(lua_State *state)
{
    lua_State *thread = lua_tothread(state, 1);
    if (!thread)
        return luaL_error(state, "invalid Logia task");
    lua_sethook(thread, taskBudgetHook, LUA_MASKCOUNT, 1'000'000);
    int results = 0;
    const int arguments = lua_gettop(state) - 1;
    if (!lua_checkstack(thread, arguments))
        return luaL_error(state, "Logia task argument stack exhausted");
    lua_xmove(state, thread, arguments);
    int status = lua_resume(thread, state, arguments, &results);
    lua_pushboolean(state, status == LUA_OK || status == LUA_YIELD);
    lua_pushboolean(state, status == LUA_OK);
    if (status == LUA_OK || status == LUA_YIELD)
    {
        lua_xmove(thread, state, results);
        return results + 2;
    }
    const char *message = lua_tostring(thread, -1);
    luaL_traceback(state, thread, message ? message : "Logia task failed", 1);
    lua_settop(thread, 0);
    return 3;
}
constexpr const char *schedulerSource = R"lua(
local co = coroutine
local resume = __ay_resume_task
local S = { items = {}, next = 1, now = 0, tick_index = 0, count = 0, last_resumed = 0 }
local function remove(id)
    local item = S.items[id]
    if not item then return false end
    S.items[id] = nil
    S.count = S.count - 1
    if item.connection then event.unsubscribe(item.connection) end
    return true
end
function S.start(fn, owner, ...)
    if S.count >= 1024 then error("Logia task limit exceeded") end
    local id = S.next; S.next = id + 1
    S.items[id] = { thread = co.create(fn), owner = owner, due = S.now,
                    eligible_tick = S.tick_index + 1, arguments = table.pack(...) }
    S.count = S.count + 1
    return id
end
S.cancel = remove
function S.pending(id) return S.items[id] ~= nil end
function S.cancel_owner(owner)
    local ids = {}
    for id, item in pairs(S.items) do
        if item.owner == owner or string.sub(item.owner, 1, #owner + 10) == owner .. "/instance/" then ids[#ids + 1] = id end
    end
    for _, id in ipairs(ids) do remove(id) end
end
function S.clear()
    local ids = {}; for id in pairs(S.items) do ids[#ids + 1] = id end
    for _, id in ipairs(ids) do remove(id) end
end
function S.tick(delta)
    S.now = S.now + delta; S.error_owner = nil; S.error_message = nil
    S.tick_index = S.tick_index + 1
    local ids = {}; for id in pairs(S.items) do ids[#ids + 1] = id end
    -- Resume in ascending ID order, rotating after the last resumed ID so
    -- recurring zero-delay work cannot starve tasks beyond the per-tick cap.
    local cursor = S.last_resumed
    table.sort(ids, function(a,b)
        if (a > cursor) ~= (b > cursor) then return a > cursor end
        return a < b
    end)
    local resumed = 0
    for _, id in ipairs(ids) do
        local item = S.items[id]
        if item and item.due and item.due <= S.now and item.eligible_tick <= S.tick_index and resumed < 256 then
            resumed = resumed + 1
            S.last_resumed = id
            if item.connection then event.unsubscribe(item.connection); item.connection = nil end
            S.enter(item.owner)
            local arguments = item.arguments or { n = 0 }
            item.arguments = nil
            local ok, done, kind, value = resume(item.thread, table.unpack(arguments, 1, arguments.n))
            S.leave()
            if S.items[id] ~= item then
                -- Cancellation during the task body must not create a new subscription.
            elseif not ok then
                S.error_owner = item.owner; S.error_message = kind; remove(id)
            elseif done then remove(id)
            elseif kind == "delay" and type(value) == "number" and value >= 0 and value < math.huge then
                item.due = S.now + value
                item.eligible_tick = S.tick_index + 1
            elseif kind == "event" and type(value) == "string" then
                item.due = nil
                S.enter(item.owner)
                item.connection = event.subscribe(value, function(...)
                    -- First delivery wins; never resume inside an EventBus callback.
                    if S.items[id] and not item.due then
                        item.arguments = table.pack(...)
                        item.due = S.now
                        item.eligible_tick = S.tick_index + 1
                    end
                end)
                S.leave()
                if not item.connection or item.connection == 0 then
                    S.error_owner = item.owner; S.error_message = "unknown event alias: " .. value; remove(id)
                end
            else
                S.error_owner = item.owner; S.error_message = "invalid task wait operation"; remove(id)
            end
        end
    end
end
function S.delay(seconds)
    if not co.isyieldable() then error("await requires tasks.start") end
    if type(seconds) ~= "number" or seconds < 0 or seconds >= math.huge then error("delay must be finite and nonnegative") end
    return co.yield("delay", seconds)
end
function S.wait_event(alias)
    if not co.isyieldable() then error("await requires tasks.start") end
    return co.yield("event", alias)
end
return S
)lua";
} // namespace
void ScriptTasks::install(sol::state &lua, ScriptTaskHooks hooks)
{
    lua.open_libraries(sol::lib::coroutine);
    lua_register(lua.lua_state(), "__ay_resume_task", &resumeTask);
    auto result = lua.safe_script(schedulerSource, sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error error = result;
        throw error;
    }
    state = result.get<sol::table>();
    state["enter"] = hooks.enter;
    state["leave"] = hooks.leave;
    auto table = lua.create_table();
    table["start"] = [this, owner = std::move(hooks.owner)](sol::function function, sol::variadic_args args) {
        const auto name = owner();
        if (name.empty())
            throw std::runtime_error("tasks.start requires a script execution owner");
        sol::protected_function start = state["start"];
        auto result = start(function, name, sol::as_args(args));
        if (!result.valid())
        {
            sol::error error = result;
            throw error;
        }
        return result.get<lua_Integer>();
    };
    table["cancel"] = state["cancel"];
    table["pending"] = state["pending"];
    lua["tasks"] = table;
    lua["__ay_wait_delay"] = state["delay"];
    lua["__ay_wait_event"] = state["wait_event"];
    lua["__ay_resume_task"] = sol::nil;
    lua["coroutine"] = sol::nil;
}
void ScriptTasks::tick(double delta)
{
    if (!state.valid())
        return;
    if (!std::isfinite(delta) || delta < 0)
        delta = 0;
    sol::protected_function tick = state["tick"];
    auto result = tick(delta);
    if (!result.valid())
    {
        sol::error error = result;
        throw error;
    }
}
void ScriptTasks::cancelOwner(const std::string &owner)
{
    if (!state.valid())
        return;
    sol::table items = state["items"];
    std::vector<lua_Integer> ids;
    for (const auto &entry : items)
    {
        const sol::table item = entry.second.as<sol::table>();
        const auto name = item.get_or("owner", std::string{});
        if (name == owner || name.starts_with(owner + "/instance/"))
        {
            ids.push_back(entry.first.as<lua_Integer>());
            sol::optional<lua_Integer> connection = item["connection"];
            if (connection)
                ayt::event::EventBus::instance().unsubscribe(
                    static_cast<ayt::event::ConnectionId>(*connection));
        }
    }
    // Teardown does not execute Lua: an exhausted script budget must not
    // prevent instance/reload cleanup. The bridge also retires owner records.
    for (auto id : ids)
        items[id] = sol::nil;
    state["count"] = state.get_or("count", 0) - static_cast<int>(ids.size());
}
void ScriptTasks::clear()
{
    if (state.valid())
    {
        sol::protected_function clear = state["clear"];
        (void)clear();
    }
    state = sol::table{};
}
std::string ScriptTasks::errorOwner() const
{
    return state.valid() ? state.get_or("error_owner", std::string{}) : std::string{};
}
std::string ScriptTasks::errorMessage() const
{
    return state.valid() ? state.get_or("error_message", std::string{}) : std::string{};
}
} // namespace ayt::script
