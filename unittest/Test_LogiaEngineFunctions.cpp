#include <AYEntity/EntityImpl.h>
#include <AYEntity/World.h>
#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYScript/ScriptRuntimeBridge.h>
#include <AYScript/logia/EngineFunctions.h>
#include <AYScript/logia/LogiaPipeline.h>
#include <AYTest.h>
#include <limits>
using namespace ayt::script;
namespace
{
auto context()
{
    auto ctx = logia::checkedLogiaHostContext(logia::actorLogiaHostContext());
    ctx.actorProperties = {{"hp", "float"}};
    return ctx;
}
} // namespace
TEST_SUITE(LogiaEngineFunctionsTests)
TEST_CASE(shared_engine_metadata_checks_arguments_without_granting_runtime_access)
{
    auto ctx = logia::engineLogiaHostContext(context());
    CHECK(logia::engineLogiaFunctions().size() == 37u);
    CHECK(logia::compileLogiaToLua("script A { on_start() { audio.play(\"a.audio\",1.0,false) "
                                   "physics.raycast(vec3.new(0,0,0),vec3.new(1,0,0),10.0,1) "
                                   "ui.set_text(\"hud/hp\",\"7\") ui.emit_signal(\"start\") "
                                   "world.request(\"level\") gameflow.request(\"play\") } }",
                                   ctx)
              .success);
    CHECK(!logia::compileLogiaToLua("script A { on_start() { audio.play(true,1.0,false) } }", ctx)
               .success);
    CHECK(!logia::compileLogiaToLua("script A { on_start() { world.request(1) } }", ctx).success);
    CHECK(!logia::compileLogiaToLua("script A { on_start() { gameflow.request(false) } }", ctx).success);
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", "script A { on_start() { audio.available() } }", ctx, errors));
    ayt::entity::ActorScriptComponent actor;
    CHECK(!bridge.callActorLifecycle("A", "on_start", actor));
}
TEST_CASE(native_vectors_round_trip_and_nonfinite_returns_fail_protected_call)
{
    LogiaRuntimeBridge bridge;
    CHECK(bridge.registerHostFunction("probe.vector", {{"vec3"}, "vec3"},
                                      [](const auto &args) -> ScriptHostValue {
                                          auto value = std::get<ayt::math::FVector3>(args.at(0));
                                          return value * 2.0f;
                                      }));
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript(
        "A", "script A { on_start() { var p:vec3=probe.vector(vec3.new(3,4,5)) self.hp=p.y } }",
        context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 8.0, 0.001);
    CHECK(bridge.registerHostFunction(
        "probe.vector", {{"vec3"}, "vec3"}, [](const auto &) -> ScriptHostValue {
            return ayt::math::FVector3(std::numeric_limits<float>::infinity(), 0, 0);
        }));
    CHECK(!bridge.callActorLifecycle("A", "on_start", actor));
}
TEST_CASE(actor_self_uses_world_tokens_and_rebind_invalidates_native_resolution)
{
    auto &world = ayt::entity::World::instance();
    world.initialize();
    LogiaRuntimeBridge bridge;
    bridge.bindWorld(&world);
    auto *entity = world.createEntity();
    auto *actor = entity->addComponent<ayt::entity::ActorScriptComponent>();
    actor->properties["hp"] = 0.0;
    std::int64_t captured = 0;
    CHECK(bridge.registerHostFunction(
        "probe.self", {{"int"}, "float"}, [&](const auto &args) -> ScriptHostValue {
            captured = std::get<std::int64_t>(args[0]);
            return bridge.resolveEntityToken(captured) == entity ? 1.0 : 0.0;
        }));
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", "script A { on_start() { self.hp=probe.self(entity.self()) } }",
                            context(), errors));
    CHECK(bridge.callActorLifecycle("A", "on_start", *actor));
    CHECK_FLOAT_EQ(std::get<double>(actor->properties["hp"]), 1.0, 0.001);
    auto generation = bridge.worldGeneration();
    bridge.bindWorld(nullptr);
    CHECK(bridge.worldGeneration() != generation);
    CHECK(bridge.resolveEntityToken(captured) == nullptr);
    world.shutdown();
}
TEST_SUITE_END
