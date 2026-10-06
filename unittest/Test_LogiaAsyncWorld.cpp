#include "LogiaTestHelpers.h"
#include <AYEntity/ActorClassAsset.h>
#include <AYEntity/EntityImpl.h>
#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYEntity/components/TransformComponent.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/PhysicsEvents.h>
#include <AYTest.h>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
struct AsyncWorld {
    ayt::entity::World &world = ayt::entity::World::instance();
    ayt::script::ScriptSubSystem runtime;
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("logia_async_world_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) / "Assets";
    AsyncWorld() {
        logia_test::resetWorldForTest(world);
        std::filesystem::create_directories(root / "actors");
        std::filesystem::create_directories(root / "helpers");
    }
    ~AsyncWorld() {
        logia_test::shutdownScriptHost(world, &runtime);
        std::error_code ignored;
        std::filesystem::remove_all(root.parent_path(), ignored);
    }
};
} // namespace
TEST_SUITE(LogiaAsyncWorldTests)
TEST_CASE(actor_imported_task_consumes_native_event_only_in_gameplay_and_updates_transform)
{
    AsyncWorld fixture;
    auto &world = fixture.world;
    const auto &root = fixture.root;
    ayt::entity::ActorClassAsset asset;
    asset.id = "AsyncActor"; asset.scriptPath = "actors/AsyncActor.logia";
    asset.propertiesJson = R"({"hp":0,"amount":5})";
    std::string error;
    CHECK(ayt::entity::saveActorClassAsset((root / "actors/AsyncActor.act").string(), asset, &error));
    std::ofstream(root / "actors/AsyncActor.logia") << R"logia(script AsyncActor {
        import "../helpers/collision.logia"
        on_start() { tasks.start(receive_collision,self.amount) }
    })logia";
    std::ofstream(root / "helpers/collision.logia") << R"logia(script Helpers {
        function receive_collision(amount:float) {
            await wait_event("physics_collision") into (body_a:int,body_b:int,kind:int)
            self.hp=amount+body_a+body_b+kind
            self.transform.position=vec3.new(self.hp,0,0)
        }
    })logia";
    auto *first = world.createEntity(); auto *second = world.createEntity();
    CHECK(first != nullptr); CHECK(second != nullptr);
    if (!first || !second) return;
    CHECK(ayt::entity::instantiateActorClass(*first, asset, "actors/AsyncActor.act", root.string(), &error));
    CHECK(ayt::entity::instantiateActorClass(*second, asset, "actors/AsyncActor.act", root.string(), &error));
    auto *instance = second->getComponent<ayt::entity::ActorInstanceComponent>();
    CHECK(instance != nullptr); if (!instance) return;
    instance->propertyOverridesJson = R"({"amount":7})";
    CHECK(fixture.runtime.initialize());
    ayt::game::FrameContext frame{}; frame.deltaTime = 0.016f; frame.fixedDeltaTime = 0.016f;
    fixture.runtime.tick(ayt::game::FramePhase::Gameplay, frame);
    auto *one = first->getComponent<ayt::entity::ActorScriptComponent>();
    auto *two = second->getComponent<ayt::entity::ActorScriptComponent>();
    auto *transform = first->getComponent<ayt::entity::Transform>();
    CHECK(one != nullptr); CHECK(two != nullptr); CHECK(transform != nullptr);
    if (!one || !two || !transform) return;
    ayt::event::EventBus::instance().emit(ayt::event::PhysicsCollisionEvent{3, 4, 1});
    fixture.runtime.tick(ayt::game::FramePhase::FixedPrePhysics, frame);
    CHECK_FLOAT_EQ(std::get<double>(one->properties.at("hp")), 0.0, 0.001);
    CHECK_FLOAT_EQ(transform->position.x, 0.0, 0.001);
    fixture.runtime.tick(ayt::game::FramePhase::Gameplay, frame);
    CHECK_FLOAT_EQ(std::get<double>(one->properties.at("hp")), 13.0, 0.001);
    CHECK_FLOAT_EQ(std::get<double>(two->properties.at("hp")), 15.0, 0.001);
    CHECK_FLOAT_EQ(transform->position.x, 13.0, 0.001);
    CHECK(fixture.runtime.bridge().getLastError().luaMessage.empty());
}
TEST_SUITE_END
