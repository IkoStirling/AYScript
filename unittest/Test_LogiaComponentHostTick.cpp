// Test_LogiaComponentHostTick.cpp - S3.4 entity tick dedup (R1 review)
//
// Guards against double entity tick when both EntitySubSystem and
// ScriptSubSystem run in the same GameLoop frame:
//   EntitySubSystem::update → World::update → Entity::onUpdate
//   ScriptSubSystem::update → tickComponentHosts (must skip when
//   "Entity" is registered).

#include "AYScript/ScriptSubSystem.h"
#include "AYScript/logia/CompilerError.h"
#include "LogiaTestHelpers.h"
#include "AYTest.h"

#include <AYEntity.h>
#include <AYGameLoop/SubSystemRegistry.h>
#include <AYEntity/World.h>
#include <AYEntity/components/ScriptComponent.h>

#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* kSmokeScript = R"(
script SmokeCounter {
    var n: int = 0
    on_update() {
        n = n + 1
        __s34_smoke_counter = tostring(n)
    }
}
)";

// Minimal "Entity" subsystem stub — mirrors EntitySubSystem::update
// without the one-shot static guard in registerEntitySubSystem().
class TestEntitySubSystem : public ayt::game::ISubSystem {
public:
    const char* getName() const override { return "Entity"; }

    const ayt::game::SubSystemDescriptor& getDescriptor() const override
    {
        static ayt::game::SubSystemDescriptor desc = {
            .name = "Entity",
            .dependencies = {},
            .basePriority = 0,
            .timeType = ayt::game::SubSystemDescriptor::TimeType::Scaled
        };
        return desc;
    }

    bool initialize() override { return true; }
    void shutdown() override {}

    void update(float dt) override
    {
        ayt::entity::World::instance().update(dt);
    }

    void fixedUpdate(float /*fixedDeltaTime*/) override {}
};

void resetEntitySubsystemRegistration()
{
    ayt::game::SubSystemRegistry::instance().unregisterSubSystem("Entity");
}

struct SmokeFixture {
    SmokeFixture()
    {
        resetEntitySubsystemRegistration();
        logia_test::resetWorldForTest(world);
    }

    ~SmokeFixture()
    {
        logia_test::shutdownScriptHost(world, &sub);
        resetEntitySubsystemRegistration();
    }

    ayt::entity::World& world = ayt::entity::World::instance();
    ayt::script::ScriptSubSystem sub;
    std::vector<ayt::script::logia::CompilerError> errors;
};

bool bindSmokeScript(ayt::entity::Entity& entity,
                     ayt::script::ScriptSubSystem& sub,
                     std::vector<ayt::script::logia::CompilerError>& errors)
{
    auto* script = entity.addComponent<ayt::entity::ScriptComponent>();
    if (!script) return false;
    script->setScriptName("SmokeCounter");
    if (!sub.initialize()) return false;
    return sub.bindAndLoad(*script, kSmokeScript, errors);
}

} // namespace

TEST_SUITE(LogiaComponentHostTickTests)

TEST_CASE(s34_headless_tickComponentHosts_ticks_once_per_update)
{
    SmokeFixture fx;
    ayt::entity::Entity* entity = fx.world.createEntity();
    CHECK_NOT_NULL(entity);
    CHECK(bindSmokeScript(*entity, fx.sub, fx.errors));
    CHECK(fx.errors.empty());

    fx.sub.update(0.016f);
    CHECK(fx.sub.bridge().getLuaGlobalString("__s34_smoke_counter")
              == std::string("1"));

    fx.sub.update(0.016f);
    CHECK(fx.sub.bridge().getLuaGlobalString("__s34_smoke_counter")
              == std::string("2"));
}

TEST_CASE(s34_host_entity_and_script_subsystem_tick_once_per_frame)
{
    // With "Entity" registered, tickComponentHosts must skip its entity
    // walk; only TestEntitySubSystem::update → World::update ticks
    // ScriptComponents (1× per frame, not 2×).
    SmokeFixture fx;
    ayt::game::SubSystemRegistry::instance().registerSubSystem(
        new TestEntitySubSystem());

    auto* entitySys =
        ayt::game::SubSystemRegistry::instance().findSubSystem("Entity");
    CHECK_NOT_NULL(entitySys);

    ayt::entity::Entity* entity = fx.world.createEntity();
    CHECK_NOT_NULL(entity);
    CHECK(bindSmokeScript(*entity, fx.sub, fx.errors));
    CHECK(fx.errors.empty());

    const float dt = 0.016f;
    entitySys->update(dt);
    fx.sub.update(dt);
    CHECK(fx.sub.bridge().getLuaGlobalString("__s34_smoke_counter")
              == std::string("1"));

    entitySys->update(dt);
    fx.sub.update(dt);
    CHECK(fx.sub.bridge().getLuaGlobalString("__s34_smoke_counter")
              == std::string("2"));
}

TEST_SUITE_END
