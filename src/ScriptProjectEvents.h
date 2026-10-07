#pragma once
#include <AYScript/ScriptRuntimeBridge.h>
#include <AYScript/LogiaEventBridge.h>
#include <sol/sol.hpp>
#include <unordered_map>
namespace ayt::script {
class ScriptProjectEvents {
public:
    void install(sol::state& lua, LogiaEventHooks hooks);
    bool registerSchema(const std::string& name,const std::vector<std::string>& types);
    bool emit(const std::string& alias,std::vector<ScriptHostValue> values);
    void clear() { schemas.clear();error.clear(); }
    std::unordered_map<std::string,std::vector<std::string>> schemas;
    std::string error;
private:
    sol::state* lua = nullptr;
    LogiaEventHooks hooks;
    std::vector<std::string> dispatchErrors;
};
}
