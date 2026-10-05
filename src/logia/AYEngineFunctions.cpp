#include "AYScript/logia/EngineFunctions.h"
namespace ayt::script::logia
{
const std::unordered_map<std::string, HostFunctionSignature> &engineLogiaFunctions()
{
    static const std::unordered_map<std::string, HostFunctionSignature> functions{
        {"audio.available", {{}, "bool"}},
        {"audio.play", {{"string", "float", "bool"}, "int"}},
        {"audio.stop", {{"int"}, "bool"}},
        {"audio.playing", {{"int"}, "bool"}},
        {"audio.set_volume", {{"int", "float"}, "bool"}},
        {"animation.available", {{}, "bool"}},
        {"animation.play", {{"int", "string", "bool"}, "bool"}},
        {"animation.stop", {{"int"}, "bool"}},
        {"animation.pause", {{"int", "bool"}, "bool"}},
        {"animation.set_rate", {{"int", "float"}, "bool"}},
        {"physics.available", {{}, "bool"}},
        {"physics.available2d", {{}, "bool"}},
        {"physics.raycast", {{"vec3", "vec3", "float", "int"}, "int"}},
        {"physics.raycast2d", {{"vec3", "vec3", "float", "int"}, "int"}},
        {"physics.hit_entity", {{"int"}, "int"}},
        {"physics.hit_point", {{"int"}, "vec3"}},
        {"physics.hit_normal", {{"int"}, "vec3"}},
        {"physics.hit_distance", {{"int"}, "float"}},
        {"physics.release_hit", {{"int"}, "bool"}},
        {"physics.apply_force", {{"int", "vec3"}, "bool"}},
        {"physics.set_velocity", {{"int", "vec3"}, "bool"}},
        {"ui.available", {{}, "bool"}},
        {"ui.exists", {{"string"}, "bool"}},
        {"ui.set_text", {{"string", "string"}, "bool"}},
        {"ui.set_visible", {{"string", "bool"}, "bool"}},
        {"ui.set_enabled", {{"string", "bool"}, "bool"}}};
    return functions;
}
LogiaHostContext engineLogiaHostContext(LogiaHostContext context)
{
    for (const auto &[name, signature] : engineLogiaFunctions())
        context.hostFunctions.try_emplace(name, signature);
    return context;
}
} // namespace ayt::script::logia
