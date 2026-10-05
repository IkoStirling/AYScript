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
    /// Host hook installs its providers before any lifecycle source is loaded.
    explicit ScriptRuntimeModule(InstallHook hostFunctions = {});
};

} // namespace ayt::script
