#pragma once
#include <AYScript/ScriptRuntimeBridge.h>
#include <sol/sol.hpp>
namespace ayt::script {
using PersistentResult = LogiaRuntimeBridge::PersistentStateResult;
PersistentResult captureScriptState(sol::state& lua, sol::table module, sol::object self,
                                    const std::string& name);
PersistentResult applyScriptState(sol::state& lua, sol::table module, sol::object self,
                                  const std::string& name, const std::string& json);
}
