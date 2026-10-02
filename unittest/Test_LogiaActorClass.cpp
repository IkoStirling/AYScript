#include <AYScript/ScriptRuntimeBridge.h>
#include <AYScript/ScriptSubSystem.h>
#include <AYScript/logia/LogiaPipeline.h>
#include <AYEntity/ActorClassAsset.h>
#include <AYEntity/EntityImpl.h>
#include <AYEntity/World.h>
#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYEntity/components/TransformComponent.h>
#include <AYTest.h>

#include <filesystem>
#include <fstream>

using namespace ayt::script;

TEST_SUITE(LogiaActorClassTests)

TEST_CASE(actor_self_properties_are_independent_and_persist_across_calls)
{
    LogiaRuntimeBridge bridge;
    const char* source = R"(
script EnemyActor {
    on_start() {
        self.hp = self.hp - 1
    }
    on_update(dt: float) {
        self.hp = self.hp - 2
    }
}
)";
    const auto authored = logia::compileLogiaToLua(
        source, logia::actorLogiaHostContext());
    CHECK(authored.success);
    CHECK(authored.diagnostics.empty());
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("EnemyActor", source,
                            logia::actorLogiaHostContext(), errors));
    ayt::entity::ActorScriptComponent first;
    ayt::entity::ActorScriptComponent second;
    first.properties["hp"] = 20.0;
    second.properties["hp"] = 50.0;
    CHECK(bridge.callActorLifecycle("EnemyActor", "on_start", first));
    CHECK(bridge.callActorLifecycle("EnemyActor", "on_start", second));
    float dt = 0.016f;
    CHECK(bridge.callActorLifecycle("EnemyActor", "on_update", first, &dt));
    CHECK_FLOAT_EQ(std::get<double>(first.properties.at("hp")), 17.0, 0.001);
    CHECK_FLOAT_EQ(std::get<double>(second.properties.at("hp")), 49.0, 0.001);
    CHECK(bridge.callActorLifecycle("EnemyActor", "on_update", second, &dt));
    CHECK_FLOAT_EQ(std::get<double>(second.properties.at("hp")), 47.0, 0.001);
}

TEST_CASE(actor_instances_bind_from_class_and_run_in_world)
{
    namespace fs = std::filesystem;
    ayt::entity::World& world = ayt::entity::World::instance();
    world.initialize();
    const fs::path root = fs::temp_directory_path()
        / "ay_script_actor_bind" / "Assets";
    fs::create_directories(root / "actors");
    ayt::entity::ActorClassAsset asset;
    asset.id = "BoundEnemy";
    asset.scriptPath = "actors/BoundEnemy.logia";
    asset.propertiesJson = R"({"hp":20})";
    std::string error;
    CHECK(ayt::entity::saveActorClassAsset(
        (root / "actors/BoundEnemy.act").string(), asset, &error));
    {
        std::ofstream script(root / "actors/BoundEnemy.logia");
        script << "script BoundEnemy {\n"
               << "  on_start() { self.hp = self.hp - 1 }\n"
               << "  on_update(dt: float) {\n"
               << "    self.hp = self.hp - 2\n"
               << "    self.transform.position.x = self.transform.position.x + 1\n"
               << "  }\n"
               << "}\n";
    }
    ayt::entity::ActorClassAsset child;
    child.id = "EliteEnemy";
    child.parentPath = "actors/BoundEnemy.act";
    child.propertiesJson = R"({"hp":30})";
    CHECK(ayt::entity::saveActorClassAsset(
        (root / "actors/EliteEnemy.act").string(), child, &error));
    auto* first = world.createEntity();
    auto* second = world.createEntity();
    auto* elite = world.createEntity();
    CHECK(ayt::entity::instantiateActorClass(*first, asset,
        "actors/BoundEnemy.act", root.string(), &error));
    CHECK(ayt::entity::instantiateActorClass(*second, asset,
        "actors/BoundEnemy.act", root.string(), &error));
    CHECK(ayt::entity::instantiateActorClass(*elite, child,
        "actors/EliteEnemy.act", root.string(), &error));
    ScriptSubSystem runtime;
    CHECK(runtime.initialize());
    runtime.bindActorHosts();
    auto* one = first->getComponent<ayt::entity::ActorScriptComponent>();
    auto* two = second->getComponent<ayt::entity::ActorScriptComponent>();
    auto* eliteScript = elite->getComponent<ayt::entity::ActorScriptComponent>();
    CHECK_NOT_NULL(one);
    CHECK_NOT_NULL(two);
    CHECK_NOT_NULL(eliteScript);
    CHECK_FLOAT_EQ(std::get<double>(one->properties.at("hp")), 19.0, 0.001);
    CHECK_FLOAT_EQ(std::get<double>(two->properties.at("hp")), 19.0, 0.001);
    CHECK_FLOAT_EQ(std::get<double>(eliteScript->properties.at("hp")), 29.0, 0.001);
    world.update(0.016f);
    CHECK(runtime.bridge().getLastError().luaMessage.empty());
    CHECK_FLOAT_EQ(std::get<double>(one->properties.at("hp")), 17.0, 0.001);
    CHECK_FLOAT_EQ(std::get<double>(two->properties.at("hp")), 17.0, 0.001);
    CHECK_FLOAT_EQ(std::get<double>(eliteScript->properties.at("hp")), 27.0, 0.001);
    CHECK_FLOAT_EQ(first->getComponent<ayt::entity::Transform>()->position.x,
                   1.0f, 0.001f);
    CHECK_FLOAT_EQ(second->getComponent<ayt::entity::Transform>()->position.x,
                   1.0f, 0.001f);
    runtime.shutdown();
    world.shutdown();
    fs::remove(root / "actors/BoundEnemy.logia");
    fs::remove(root / "actors/BoundEnemy.act");
    fs::remove(root / "actors/EliteEnemy.act");
    fs::remove(root / "actors");
    fs::remove(root);
    fs::remove(root.parent_path());
}

TEST_SUITE_END
