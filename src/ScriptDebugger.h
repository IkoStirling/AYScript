#pragma once
#include <AYScript/logia/Debug.h>
#include <AYScript/logia/LuaCodegen.h>
#include <sol/sol.hpp>
#include <functional>
namespace ayt::script {
class ScriptDebugger {
public:
    ~ScriptDebugger() { stop(); }
    void stop();
    bool start(sol::state& lua, sol::protected_function fn, sol::object self,
               std::string name, logia::LogiaSourceMap map);
    bool resume(LogiaDebugCommand command);
    LogiaDebugSnapshot snapshot() const;
    bool executing() const { return running; }
    bool active() const { return status == LogiaDebugStatus::Paused; }
    std::vector<LogiaDebugBreakpoint> breakpoints;
    static std::vector<LogiaDebugVariable> inspectTable(sol::table table);
private:
    static void hook(lua_State* state, lua_Debug* info);
    bool shouldPause(lua_Debug* info);
    bool running = false;
    lua_State* main = nullptr;
    lua_State* thread = nullptr;
    int reference = LUA_NOREF, arguments = 0, depth = 0, instructions = 0, lastLine = 0, lastDepth = 0;
    LogiaDebugStatus status = LogiaDebugStatus::Idle;
    LogiaDebugCommand command = LogiaDebugCommand::StepInto;
    std::string name, error;
    logia::LogiaSourceMap map;
    logia::SourceLocation previous;
};
}
