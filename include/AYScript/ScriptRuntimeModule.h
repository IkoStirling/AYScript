#pragma once

#include <AYGameLoop/SubSystemModule.h>

#include <string_view>

namespace ayt::script
{

inline constexpr std::string_view kScriptRuntimeModuleId =
    "AYScript.Runtime";

class ScriptRuntimeModule final : public ayt::game::SubSystemModule
{
public:
    ScriptRuntimeModule();
};

} // namespace ayt::script
