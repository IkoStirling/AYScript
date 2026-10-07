#pragma once
#include <sol/sol.hpp>
#include <AYScript/logia/Debug.h>
#include <functional>
#include <string>
namespace ayt::script {
struct ScriptTaskHooks {
    std::function<std::string()> owner;
    std::function<void(const std::string&)> enter;
    std::function<void()> leave;
};
class ScriptTasks {
public:
    void install(sol::state& lua, ScriptTaskHooks hooks);
    void tick(double delta);
    void cancelOwner(const std::string& owner);
    void clear();
    std::vector<LogiaTaskInspection> inspect() const;
    std::string errorOwner() const;
    std::string errorMessage() const;
private:
    sol::table state;
};
}
