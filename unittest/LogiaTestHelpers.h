#pragma once
// LogiaTestHelpers.h — shared compile→Lua helpers for AYScript unit tests.
//
// Rule: do NOT stack-allocate Compiler + LuaCodegen in the same test
// helper or TEST_CASE frame. Use compileLogiaToLua() (library) or the
// wrappers below. Semantic-only tests may still use a lone `Compiler c`.
//
// World teardown: always call resetWorldForTest() at case entry and
// shutdownScriptHost() before exit when a ScriptSubSystem was used.
// Prior tests may leave ScriptComponents whose _bridge points at a
// destroyed adapter — World::shutdown() then crashes in onDetach().

#include "logia/AYLogiaPipeline.h"
#include "AYScriptSubSystem.h"

#include <AYEntity.h>
#include <AYWorld.h>
#include <components/AYScriptComponent.h>

#include <string>
#include <vector>

namespace logia_test {

inline void teardownScriptEntities(ayt::entity::World& world)
{
    const auto entities = world.getAllEntities();
    for (auto* entity : entities) {
        if (!entity || !entity->isValid()) {
            continue;
        }
        if (auto* script = entity->getComponent<ayt::entity::ScriptComponent>()) {
            script->setBridge(nullptr);
            entity->removeComponent<ayt::entity::ScriptComponent>();
        }
        world.destroyEntity(entity);
    }
}

inline void resetWorldForTest(ayt::entity::World& world)
{
    teardownScriptEntities(world);
    world.shutdown();
    world.initialize();
}

inline void shutdownScriptHost(ayt::entity::World& world,
                              ayt::script::ScriptSubSystem* sub)
{
    teardownScriptEntities(world);
    if (sub != nullptr) {
        sub->shutdown();
    }
    world.shutdown();
}

inline std::string compileToLua(
    const std::string& source,
    const ayt::script::logia::LogiaHostContext& ctx =
        ayt::script::logia::defaultLogiaHostContext(),
    ayt::script::logia::LuaCodegenOptions opts = {})
{
    auto result = ayt::script::logia::compileLogiaToLua(source, ctx, opts);
    return result.success ? result.lua : std::string{};
}

inline std::string compileToLua(
    const char* source,
    const ayt::script::logia::LogiaHostContext& ctx =
        ayt::script::logia::defaultLogiaHostContext(),
    ayt::script::logia::LuaCodegenOptions opts = {})
{
    return compileToLua(std::string(source), ctx, opts);
}

inline std::string compileToLua(
    const std::string& source,
    std::vector<ayt::script::logia::CompilerError>& errors,
    const ayt::script::logia::LogiaHostContext& ctx =
        ayt::script::logia::defaultLogiaHostContext(),
    ayt::script::logia::LuaCodegenOptions opts = {})
{
    auto result = ayt::script::logia::compileLogiaToLua(source, ctx, opts);
    if (!result.success) {
        errors.insert(errors.end(), result.errors.begin(), result.errors.end());
    }
    return result.success ? result.lua : std::string{};
}

} // namespace logia_test
