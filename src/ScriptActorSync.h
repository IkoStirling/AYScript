#pragma once
#include <sol/sol.hpp>
namespace ayt::entity { struct ActorScriptComponent; }
namespace ayt::script {
void refreshActorSelf(sol::state& lua, sol::table table, const ayt::entity::ActorScriptComponent& actor);
void commitActorSelf(sol::table table, ayt::entity::ActorScriptComponent& actor);
}
