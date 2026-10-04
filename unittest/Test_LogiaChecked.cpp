#include <AYEntity/EntityImpl.h>
#include <AYEntity/World.h>
#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYEntity/components/TransformComponent.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/ScriptTestEvents.h>
#include <AYScript/ScriptRuntimeBridge.h>
#include <AYScript/logia/LogiaPipeline.h>
#include <AYTest.h>
using namespace ayt::script;

namespace
{
logia::LogiaHostContext actorContext()
{
    auto ctx = logia::checkedLogiaHostContext(logia::actorLogiaHostContext());
    ctx.actorProperties = {{"hp", "float"}};
    return ctx;
}
bool acceptsChecked(const char *source)
{
    return logia::compileLogiaToLua(source, actorContext()).success;
}
} // namespace
TEST_SUITE(LogiaCheckedTests)
TEST_CASE(checked_rejects_incompatible_initial_value)
{
    CHECK(!acceptsChecked("script Probe { var count: int = \"oops\" }"));
}
TEST_CASE(checked_rejects_incompatible_assignment)
{
    CHECK(!acceptsChecked(
        "script Probe { var count: int = 1 on_update(dt: float) { count = true } }"));
}
TEST_CASE(checked_rejects_implicit_global_and_bad_lvalue)
{
    CHECK(!acceptsChecked("script Probe { on_update(dt: float) { missing = 1 } }"));
    CHECK(!acceptsChecked("script Probe { on_update(dt: float) { 1 = 2 } }"));
}
TEST_CASE(checked_locals_do_not_escape_block_or_function)
{
    CHECK(!acceptsChecked("script Probe { on_start() { do { var x: int = 1 } end self.hp = x } }"));
    CHECK(!acceptsChecked(
        "script Probe { on_start() { var x: int = 1 } on_update(dt: float) { self.hp = x } }"));
}
TEST_CASE(checked_shadowing_is_lexical_and_duplicates_fail)
{
    CHECK(acceptsChecked("script Probe { var x: int = 1 on_start() { do { var x: string = \"x\" "
                         "log.info(x) } end self.hp = x } }"));
    CHECK(!acceptsChecked("script Probe { on_start() { var x: int = 1 var x: int = 2 } }"));
}
TEST_CASE(checked_validates_function_params_return_and_fallthrough)
{
    CHECK(acceptsChecked("script Probe { function twice(n: int): int { return n * 2 } on_start() { "
                         "self.hp = twice(3) } }"));
    CHECK(!acceptsChecked("script Probe { function twice(n: int): int { return n * 2 } on_start() "
                          "{ self.hp = twice(true) } }"));
    CHECK(!acceptsChecked("script Probe { function f(): int { return \"bad\" } }"));
    CHECK(!acceptsChecked("script Probe { function f(n: bool): int { if n { return 1 } } }"));
}
TEST_CASE(checked_validates_handler_signature)
{
    CHECK(!acceptsChecked("script Probe { signal damaged(n: int); function handler(n: string) {} "
                          "on_start() { connect(\"damaged\", handler) } }"));
    CHECK(!acceptsChecked("script Probe { signal damaged(n: int); function handler() {} on_start() "
                          "{ connect(\"damaged\", handler) } }"));
    CHECK(acceptsChecked(
        "script Probe { signal damaged(n: int); function handler(n: int) { self.hp = n } "
        "on_start() { connect(\"damaged\", handler) emit(\"damaged\", 4) } }"));
}
TEST_CASE(checked_actor_schema_rejects_unknown_property_and_allows_transform)
{
    CHECK(!acceptsChecked("script Probe { on_start() { self.typo = 1 } }"));
    CHECK(acceptsChecked(
        "script Probe { on_update(dt: float) { self.transform.position.x += dt } }"));
}
TEST_CASE(checked_rejects_invalid_operators_and_lifecycle)
{
    CHECK(!acceptsChecked("script Probe { on_start() { var x: bool = 1 && true } }"));
    CHECK(!acceptsChecked("script Probe { on_update(dt: string) {} }"));
    CHECK(!acceptsChecked("script Probe { on_start() {} on_start() {} }"));
}
TEST_CASE(checked_helpers_execute_with_self_and_remain_module_local)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript(
        "A", "script A { function value(): int { return 3 } on_start() { self.hp = value() } }",
        actorContext(), errors));
    CHECK(bridge.loadScript(
        "B", "script B { function value(): int { return 9 } on_start() { self.hp = value() } }",
        actorContext(), errors));
    ayt::entity::ActorScriptComponent a, b;
    a.properties["hp"] = 0.0;
    b.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", a));
    CHECK(bridge.callActorLifecycle("B", "on_start", b));
    CHECK_FLOAT_EQ(std::get<double>(a.properties.at("hp")), 3.0, 0.001);
    CHECK_FLOAT_EQ(std::get<double>(b.properties.at("hp")), 9.0, 0.001);
}
TEST_CASE(checked_signal_handlers_keep_the_receiving_instance)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript(
        "Signal",
        "script Signal { signal hit(n: int); function hit_handler(n: int) { self.hp += n } "
        "on_start() { connect(\"hit\", hit_handler) } on_update(dt: float) { emit(\"hit\", 2) } }",
        actorContext(), errors));
    ayt::entity::ActorScriptComponent a, b;
    a.properties["hp"] = 10.0;
    b.properties["hp"] = 20.0;
    CHECK(bridge.callActorLifecycle("Signal", "on_start", a));
    CHECK(bridge.callActorLifecycle("Signal", "on_start", b));
    float dt = 0.1f;
    CHECK(bridge.callActorLifecycle("Signal", "on_update", a, &dt));
    CHECK_FLOAT_EQ(std::get<double>(a.properties.at("hp")), 12.0, 0.001);
    CHECK_FLOAT_EQ(std::get<double>(b.properties.at("hp")), 20.0, 0.001);
}
TEST_CASE(checked_cache_separates_language_policy_and_property_schema)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    const char *source = "script Probe { on_start() { self.hp = 3 } }";
    CHECK(bridge.loadScript("Probe", source, logia::actorLogiaHostContext(), errors));
    CHECK(bridge.loadScript("Probe", source, actorContext(), errors));
    auto ctx = actorContext();
    ctx.actorProperties.clear();
    CHECK(!bridge.loadScript("Probe", source, ctx, errors));
}
TEST_CASE(checked_errors_have_source_locations)
{
    auto result =
        logia::compileLogiaToLua("script Probe {\n on_start() { missing = 1 }\n}", actorContext());
    CHECK(!result.success);
    CHECK(!result.errors.empty());
    CHECK(result.errors.front().line == 2);
}
TEST_CASE(instance_state_is_independent_and_released_without_destroy_handler)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    const char *source =
        "script State { state count: int = 0 on_update(dt: float) { count += 1 self.hp = count } }";
    CHECK(bridge.loadScript("State", source, actorContext(), errors));
    ayt::entity::ActorScriptComponent a, b;
    a.properties["hp"] = 0.0;
    b.properties["hp"] = 0.0;
    float dt = 0.1f;
    CHECK(bridge.callActorLifecycle("State", "on_update", a, &dt));
    CHECK(bridge.callActorLifecycle("State", "on_update", a, &dt));
    CHECK(bridge.callActorLifecycle("State", "on_update", b, &dt));
    CHECK_FLOAT_EQ(std::get<double>(a.properties["hp"]), 2.0, 0.001);
    CHECK_FLOAT_EQ(std::get<double>(b.properties["hp"]), 1.0, 0.001);
    bridge.callActorLifecycle("State", "on_destroy", a);
    CHECK(bridge.callActorLifecycle("State", "on_update", a, &dt));
    CHECK_FLOAT_EQ(std::get<double>(a.properties["hp"]), 1.0, 0.001);
}
TEST_CASE(instance_state_reload_preserves_matching_fields_and_can_reset)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript(
        "State",
        "script State { state count: int = 0 on_update(dt: float) { count += 1 self.hp = count } }",
        actorContext(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    float dt = 0.1f;
    CHECK(bridge.callActorLifecycle("State", "on_update", actor, &dt));
    CHECK(bridge.reloadScript("State",
                              "script State { state count: int = 20 on_update(dt: float) { count "
                              "+= 2 self.hp = count } }",
                              actorContext(), errors));
    CHECK(bridge.callActorLifecycle("State", "on_update", actor, &dt));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 3.0, 0.001);
    CHECK(!bridge.reloadScript("State", "script State { broken }", actorContext(), errors));
    CHECK(bridge.callActorLifecycle("State", "on_update", actor, &dt));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 5.0, 0.001);
    bridge.resetInstanceState("State");
    CHECK(bridge.callActorLifecycle("State", "on_update", actor, &dt));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 22.0, 0.001);
}
TEST_CASE(instance_state_requires_a_receiver_and_checked_contract)
{
    CHECK(!logia::compileLogiaToLua("script State { state count: int = 0 }",
                                    logia::checkedLogiaHostContext(logia::toolLogiaHostContext()))
               .success);
    CHECK(!logia::compileLogiaToLua("script State { state count: int = 0 }",
                                    logia::actorLogiaHostContext())
               .success);
}
TEST_CASE(entity_facade_defers_structural_changes_and_rejects_stale_world_tokens)
{
    auto &world = ayt::entity::World::instance();
    world.initialize();
    LogiaRuntimeBridge bridge;
    bridge.bindWorld(&world);
    auto ctx = actorContext();
    ctx.actorProperties["token"] = "float";
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript(
        "World",
        "script World { state token: int = 0 on_start() { token = entity.create(\"created\") "
        "self.token = token } on_update(dt: float) { if entity.exists(token) { self.hp = 1 } else "
        "{ self.hp = 0 } } on_destroy() { entity.destroy(token) } }",
        ctx, errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    actor.properties["token"] = 0.0;
    CHECK(bridge.callActorLifecycle("World", "on_start", actor));
    CHECK(world.findEntity("created") == nullptr);
    bridge.flushWorldCommands();
    CHECK(world.findEntity("created") != nullptr);
    float dt = 0.1f;
    CHECK(bridge.callActorLifecycle("World", "on_update", actor, &dt));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 1.0, 0.001);
    world.shutdown();
    world.initialize();
    world.createEntity()->setName("replacement");
    bridge.bindWorld(&world);
    CHECK(bridge.callActorLifecycle("World", "on_update", actor, &dt));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 0.0, 0.001);
    bridge.callActorLifecycle("World", "on_destroy", actor);
    bridge.flushWorldCommands();
    CHECK(world.findEntity("replacement") != nullptr);
    world.shutdown();
}
TEST_CASE(host_facades_validate_signatures_and_unavailable_providers)
{
    LogiaRuntimeBridge bridge;
    CHECK(!bridge.registerHostFunction(
        "entity.find", {{"string"}, "int"},
        [](const auto &) -> ScriptHostValue { return std::int64_t{0}; }));
    CHECK(bridge.registerHostFunction(
        "stats.read", {{"string"}, "float"}, [](const auto &values) -> ScriptHostValue {
            return std::get<std::string>(values[0]) == "hp" ? 42.0 : 0.0;
        }));
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("Host", "script Host { on_start() { self.hp = stats.read(\"hp\") } }",
                            actorContext(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Host", "on_start", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 42.0, 0.001);
    CHECK(!bridge.loadScript("Wrong", "script Wrong { on_start() { self.hp = stats.read(true) } }",
                             actorContext(), errors));
    bridge.unregisterHostFunction("stats.read");
    CHECK(!bridge.callActorLifecycle("Host", "on_start", actor));
}
TEST_CASE(tasks_delay_resumes_at_frame_boundaries_and_updates_actor_state)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("Async",
                            "script Async { function work() { await delay(0.2); self.hp = 7 } "
                            "on_start() { tasks.start(work) } }",
                            actorContext(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Async", "on_start", actor));
    bridge.tickTasks(0);
    bridge.tickTasks(0.1f);
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 0.0, 0.001);
    bridge.tickTasks(0.11f);
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 7.0, 0.001);
}
TEST_CASE(actor_rotation_syncs_lifecycle_and_task_changes_into_ecs)
{
    auto& world = ayt::entity::World::instance();
    world.initialize();
    auto* entity = world.createEntity();
    auto* transform = entity->addComponent<ayt::entity::Transform>();
    auto* actor = entity->addComponent<ayt::entity::ActorScriptComponent>();
    actor->properties["hp"] = 0.0;
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("Rotate", "script Rotate { function reset() { await delay(0); "
        "self.transform.rotation=quat.identity() } on_start() { "
        "self.transform.rotation=quat.axis_angle(vec3.new(0,1,0),1.5707963) tasks.start(reset) } }",
        actorContext(), errors));
    CHECK(bridge.callActorLifecycle("Rotate", "on_start", *actor));
    CHECK_FLOAT_EQ(transform->rotation.y, 0.7071067f, 0.001f);
    CHECK_FLOAT_EQ(transform->rotation.w, 0.7071067f, 0.001f);
    bridge.tickTasks(0);
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(transform->rotation.y, 0.0f, 0.001f);
    CHECK_FLOAT_EQ(transform->rotation.w, 1.0f, 0.001f);
    bridge.releaseInstance("Rotate", actor);
    bridge.shutdown();
    world.shutdown();
}
TEST_CASE(tasks_wait_for_event_and_cancel_when_receiver_is_destroyed)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript(
        "Async",
        "script Async { function work() { await wait_event(\"script_test_ping\"); self.hp = 9 } "
        "on_start() { tasks.start(work) } }",
        actorContext(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Async", "on_start", actor));
    bridge.tickTasks(0);
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{1});
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 0.0, 0.001);
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 9.0, 0.001);
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Async", "on_start", actor));
    bridge.tickTasks(0);
    bridge.callActorLifecycle("Async", "on_destroy", actor);
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{1});
    bridge.tickTasks(1);
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 0.0, 0.001);
}
TEST_CASE(tasks_successful_reload_cancels_work_and_bad_reload_preserves_it)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    const char *source = "script Async { function work() { await delay(0.1); self.hp = 4 } "
                         "on_start() { tasks.start(work) } }";
    CHECK(bridge.loadScript("Async", source, actorContext(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Async", "on_start", actor));
    bridge.tickTasks(0);
    CHECK(!bridge.reloadScript("Async", "script Async { invalid }", actorContext(), errors));
    bridge.tickTasks(0.2f);
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 4.0, 0.001);
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Async", "on_start", actor));
    bridge.tickTasks(0);
    CHECK(bridge.reloadScript("Async", source, actorContext(), errors));
    bridge.tickTasks(1);
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 0.0, 0.001);
}
TEST_CASE(tasks_budget_stops_infinite_work_and_await_cannot_escape_task_context)
{
    CHECK(!acceptsChecked("script Async { on_start() { await delay(1) } }"));
    CHECK(!acceptsChecked("script Async { function f() { await delay(1) } on_start() { f() } }"));
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript(
        "Async",
        "script Async { function work() { while true {} } on_start() { tasks.start(work) } }",
        actorContext(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Async", "on_start", actor));
    bridge.tickTasks(0);
    CHECK(bridge.getLastError().luaMessage.find("budget") != std::string::npos);
}
TEST_SUITE_END
