#include <AYScript/ScriptRuntimeModule.h>

#include <AYEntity/EntityScriptIntegrationModule.h>
#include <AYScript/ScriptSubSystem.h>

#include <memory>
#include <string>

namespace ayt::script
{

ScriptRuntimeModule::ScriptRuntimeModule()
    : SubSystemModule(
          ayt::module::ModuleDescriptor{
              .id = std::string(kScriptRuntimeModuleId),
              .displayName = "AYScript Runtime",
              .version = "0.2.0",
              .dependencies = {
                  ayt::module::ModuleDependency::required(
                      "AYEntity.Runtime"),
                  ayt::module::ModuleDependency::required(
                      std::string(
                          ayt::entity::kEntityScriptIntegrationModuleId)),
                  ayt::module::ModuleDependency::optional(
                      "AYDevice.Runtime")}},
          "ayt.script.runtime",
          []() {
              return std::make_unique<ScriptSubSystem>();
          })
{
}

} // namespace ayt::script
