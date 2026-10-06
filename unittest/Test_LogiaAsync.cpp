#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/ScriptTestEvents.h>
#include <AYEventSystem/Events/TaskEvents.h>
#include <AYEventSystem/Events/WindowEvents.h>
#include <AYEventSystem/Events/DeviceEvents.h>
#include <AYEventSystem/Events/ResourceEvents.h>
#include <AYEventSystem/Events/PhysicsEvents.h>
#include <AYScript/ScriptRuntimeBridge.h>
#include <AYScript/logia/LogiaPipeline.h>
#include <AYScript/logia/Tooling.h>
#include <AYEntity/components/TransformComponent.h>
#include <AYReflect/ReflectRegistry.h>
#include <AYTest.h>
#include <functional>

using namespace ayt::script;
namespace {
auto context() {
    auto ctx = logia::checkedLogiaHostContext(logia::actorLogiaHostContext());
    ctx.actorProperties = {{"hp", "float"}};
    return ctx;
}
bool accepts(const char *source) {
    return logia::compileLogiaToLua(source, context()).success;
}
double hp(const ayt::entity::ActorScriptComponent &actor) {
    return std::get<double>(actor.properties.at("hp"));
}
} // namespace

TEST_SUITE(LogiaAsyncTests)
TEST_CASE(task_arguments_are_checked_against_the_declared_helper)
{
    CHECK(accepts("script A { function work(n:int, label:string) { await delay(0) } "
                  "on_start() { tasks.start(work,7,\"ready\") } }"));
    CHECK(!accepts("script A { function work(n:int) {} on_start() { tasks.start(work) } }"));
    CHECK(!accepts("script A { function work() {} on_start() { tasks.start(work,7) } }"));
    CHECK(!accepts("script A { function work(n:int) {} on_start() { tasks.start(work,true) } }"));
    CHECK(!accepts("script A { function work():int { return 7 } on_start() { tasks.start(work) } }"));
    CHECK(!accepts("script A { on_start() { tasks.start(missing,7) } }"));
}
TEST_CASE(task_arguments_reject_declared_borrowed_pointers_and_their_arrays)
{
    CHECK(accepts("script A { function receive(scene:Scene) {} on_start() { event.subscribe(\"scene_begin_play\",receive) } }"));
    CHECK(!accepts("script A { function work(scene:Scene) { await delay(0) } function receive(scene:Scene) { tasks.start(work,scene) } on_start() { event.subscribe(\"scene_begin_play\",receive) } }"));
    CHECK(!accepts("script A { function work(scene:Scene) {} function receive(scene:Scene) { tasks.start(work,scene) } }"));
    CHECK(!accepts("script A { function work(entity:Entity) {} function receive(entity:Entity) { tasks.start(work,entity) } }"));
    CHECK(!accepts("script A { function work(values:array<array<Scene>>) {} function receive(values:array<array<Scene>>) { tasks.start(work,values) } }"));
    CHECK(accepts("script A { function work(token:int) { await delay(0) } on_start() { tasks.start(work,entity.self()) } }"));
}
TEST_CASE(task_arguments_capture_scalar_values_at_start_and_keep_receivers_separate)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", R"logia(script A {
        function work(n:float, enabled:bool, label:string) {
            await delay(0)
            if enabled { self.hp=n }
        }
        on_start() { var n:float=self.hp tasks.start(work,n,true,"captured") self.hp=0 }
    })logia", context(), errors));
    ayt::entity::ActorScriptComponent a, b;
    a.properties["hp"] = 7.0; b.properties["hp"] = 13.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", a));
    CHECK(bridge.callActorLifecycle("A", "on_start", b));
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(a), 0.0, 0.001);
    CHECK_FLOAT_EQ(hp(b), 0.0, 0.001);
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(a), 7.0, 0.001);
    CHECK_FLOAT_EQ(hp(b), 13.0, 0.001);
}
TEST_CASE(event_payload_bindings_require_exact_known_scalar_signatures)
{
    CHECK(accepts("script A { function f() { await wait_event(\"task_complete\") "
                  "into (id:int,ok:bool) if ok { self.hp=id } } }"));
    CHECK(accepts("script A { function f() { await wait_event(\"window_close\") } }"));
    CHECK(!accepts("script A { function f() { await delay(0) into (n:int) } }"));
    CHECK(!accepts("script A { function f() { await wait_event(\"task_complete\") into (id:int) } }"));
    CHECK(!accepts("script A { function f() { await wait_event(\"task_complete\") into (id:float,ok:bool) } }"));
    CHECK(!accepts("script A { function f() { await wait_event(\"missing\") into (n:int) } }"));
    CHECK(!accepts("script A { function f(alias:string) { await wait_event(alias) into (n:int) } }"));
    CHECK(!accepts("script A { function f() { await wait_event(\"scene_begin_play\") into (scene:Scene) } }"));
}
TEST_CASE(event_payload_bindings_are_lexical_declarations)
{
    CHECK(!accepts("script A { function f() { var n:int=0 await wait_event(\"script_test_ping\") into (n:int) } }"));
    CHECK(!accepts("script A { function f() { await wait_event(\"window_resize\") into (n:int,n:int) } }"));
    CHECK(!accepts("script A { function f() { do { await wait_event(\"script_test_ping\") into (n:int) } end self.hp=n } }"));
    CHECK(!accepts("script A { function f() { self.hp=n await wait_event(\"script_test_ping\") into (n:int) } }"));
    CHECK(accepts("script A { function f() { var n:int=7 do { await wait_event(\"script_test_ping\") into (n:int) self.hp=n } end self.hp=n } }"));
    // `into` is contextual, so existing local identifiers remain valid.
    CHECK(accepts("script A { function f() { var into:int=1 await delay(0); into+=1 self.hp=into } }"));
}
TEST_CASE(await_captures_first_event_and_resumes_only_on_a_later_tick)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", R"logia(script A {
        function work() {
            await wait_event("task_complete") into (id:int,ok:bool)
            if ok { self.hp=id } else { self.hp=-1 }
        }
        on_start() { tasks.start(work) }
    })logia", context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    bridge.tickTasks(0);
    auto &bus = ayt::event::EventBus::instance();
    bus.emit(ayt::event::TaskCompleteEvent{17, true});
    bus.emit(ayt::event::TaskCompleteEvent{23, false});
    CHECK_FLOAT_EQ(hp(actor), 0.0, 0.001);
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 17.0, 0.001);
    bus.emit(ayt::event::TaskCompleteEvent{31, true});
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 17.0, 0.001);
}
TEST_CASE(event_emitted_by_a_task_cannot_resume_another_task_in_the_same_tick)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", R"logia(script A {
        function send() { await delay(0) event.emit("script_test_ping",19) }
        function receive() { await wait_event("script_test_ping") into (n:int) self.hp=n }
        on_start() { tasks.start(send) tasks.start(receive) }
    })logia", context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    bridge.tickTasks(0);
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 0.0, 0.001);
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 19.0, 0.001);
}
TEST_CASE(cancelling_an_event_ready_task_discards_its_saved_payload)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", R"logia(script A {
        state id:int=0
        function work() { await wait_event("script_test_ping") into (n:int) self.hp=n }
        on_start() { id=tasks.start(work) }
        on_update(dt:float) { tasks.cancel(id) }
    })logia", context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    bridge.tickTasks(0);
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{7});
    float delta = 0;
    CHECK(bridge.callActorLifecycle("A", "on_update", actor, &delta));
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 0.0, 0.001);
}
TEST_CASE(author_analysis_exposes_payload_symbols_and_declaration_navigation)
{
    const std::string source = R"logia(script A {
    function work() {
        await wait_event("task_complete") into (id:int,ok:bool)
        if ok { self.hp=id }
    }
    on_start() { tasks.start(work) }
})logia";
    auto ctx = context(); ctx.sourcePath = "async.logia";
    auto analysis = logia::analyzeLogia(source, ctx);
    CHECK(analysis.success);
    auto visible = analysis.complete("id", ctx.sourcePath, 4);
    CHECK(visible.size() == 1);
    if (!visible.empty()) {
        CHECK(visible[0].kind == "event-payload");
        CHECK(visible[0].definition.line == 3);
        auto location = analysis.definitionAt({ctx.sourcePath, 4, 25});
        CHECK(location.has_value());
        if (location) CHECK(location->line == 3);
    }
    CHECK(analysis.complete("id", ctx.sourcePath, 2).empty());
    CHECK(analysis.complete("id", ctx.sourcePath, 6).empty());
}
TEST_CASE(frame_budget_rotates_through_more_than_256_recurring_tasks)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", R"logia(script A {
        state seen:array<int>={}
        function work(n:int) {
            array.push(seen,n)
            while true { await delay(0) }
        }
        on_start() { for (var i:1024) { tasks.start(work,i) } }
        on_update(dt:float) { self.hp=array.length(seen) }
    })logia", context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    float delta = 0;
    for (int frame = 1; frame <= 4; ++frame) {
        bridge.tickTasks(0);
        CHECK(bridge.callActorLifecycle("A", "on_update", actor, &delta));
        CHECK_FLOAT_EQ(hp(actor), frame * 256.0, 0.001);
    }
    bridge.releaseInstance("A", &actor);
    // All slots are reclaimed by native owner cleanup.
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    bridge.tickTasks(0);
    CHECK(bridge.callActorLifecycle("A", "on_update", actor, &delta));
    CHECK_FLOAT_EQ(hp(actor), 256.0, 0.001);
}
TEST_CASE(cancelling_your_own_task_before_await_does_not_install_a_subscription)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", R"logia(script A {
        state id:int=0
        function work() {
            tasks.cancel(id)
            await wait_event("script_test_ping") into (n:int)
            self.hp=n
        }
        on_start() { id=tasks.start(work) }
        on_update(dt:float) { if tasks.pending(id) { self.hp=99 } }
    })logia", context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    bridge.tickTasks(0);
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{7});
    bridge.tickTasks(0);
    float delta = 0;
    CHECK(bridge.callActorLifecycle("A", "on_update", actor, &delta));
    CHECK_FLOAT_EQ(hp(actor), 0.0, 0.001);
}
TEST_CASE(false_and_empty_event_payloads_survive_deferred_resume)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", R"logia(script A {
        function completed() {
            await wait_event("task_complete") into (id:int,ok:bool)
            if !ok { self.hp+=id }
        }
        function closed() { await wait_event("window_close") self.hp+=100 }
        on_start() { tasks.start(completed) tasks.start(closed) }
    })logia", context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    bridge.tickTasks(0);
    auto &bus = ayt::event::EventBus::instance();
    bus.emit(ayt::event::TaskCompleteEvent{7, false});
    bus.emit(ayt::event::WindowCloseEvent{});
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 107.0, 0.001);
}
TEST_CASE(synchronous_subscriber_can_forward_event_arguments_to_a_task)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", R"logia(script A {
        function work(id:int,pressed:bool) { await delay(0) if pressed { self.hp=id } }
        function action(id:int,pressed:bool) { tasks.start(work,id,pressed) }
        on_start() { event.subscribe("device_action",action) }
    })logia", context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    ayt::event::EventBus::instance().emit(ayt::event::DeviceActionEvent{9, true});
    CHECK_FLOAT_EQ(hp(actor), 0.0, 0.001);
    bridge.tickTasks(0); bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 9.0, 0.001);
}
TEST_CASE(mutable_task_arguments_keep_the_languages_existing_reference_semantics)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", R"logia(script A {
        function work(values:array<int>,p:vec3) { await delay(0) self.hp=values[1]+p.x }
        on_start() {
            var values:array<int>={1} var p:vec3=vec3.new(2,0,0)
            tasks.start(work,values,p)
            values[1]=7 p.x=11
        }
    })logia", context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    bridge.tickTasks(0); bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 18.0, 0.001);
}
TEST_CASE(receiver_release_after_delivery_discards_payload_and_reclaims_its_owner)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", R"logia(script A {
        function work() { await wait_event("script_test_ping") into (n:int) self.hp=n }
        on_start() { tasks.start(work) }
    })logia", context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    bridge.tickTasks(0);
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{7});
    bridge.releaseInstance("A", &actor);
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 0.0, 0.001);
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    bridge.tickTasks(0);
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{11});
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 11.0, 0.001);
}
TEST_CASE(failed_reload_preserves_ready_payload_and_successful_reload_cancels_it)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    const char *source = R"logia(script A {
        function work(base:int) { await wait_event("script_test_ping") into (n:int) self.hp=base+n }
        on_start() { tasks.start(work,10) }
    })logia";
    CHECK(bridge.loadScript("A", source, context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    bridge.tickTasks(0);
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{7});
    CHECK(!bridge.reloadScript("A", "script A { invalid }", context(), errors));
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 17.0, 0.001);
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    bridge.tickTasks(0);
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{11});
    CHECK(bridge.reloadScript("A", source, context(), errors));
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 17.0, 0.001);
}
TEST_CASE(contextual_into_preserves_old_helper_calls_after_an_await)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("A", R"logia(script A {
        function into() { self.hp=9 }
        function work() { await wait_event("script_test_ping") into() }
        on_start() { tasks.start(work) }
    })logia", context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("A", "on_start", actor));
    bridge.tickTasks(0);
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{7});
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(hp(actor), 9.0, 0.001);
    CHECK(accepts("script A { function into(n:int) { self.hp=n } function work() { await delay(0); into(7) } on_start() { tasks.start(work) } }"));
}
TEST_CASE(all_scalar_event_aliases_keep_native_payload_order_and_values)
{
    struct Witness {
        const char *body;
        double expected;
        std::function<void()> emit;
    };
    auto &bus = ayt::event::EventBus::instance();
    const Witness cases[] = {
        {"await wait_event(\"window_resize\") into (w:int,h:int) self.hp=w*1000+h", 640480,
         [&] { bus.emit(ayt::event::WindowResizeEvent{640,480}); }},
        {"await wait_event(\"resource_ready\") into (id:int,refs:int,ok:bool) if !ok { self.hp=id+refs }",
         4294967301.0, [&] { bus.emit(ayt::event::ResourceLoadCompleteEvent{4294967299ull,2,false}); }},
        {"await wait_event(\"script_test_ping\") into (n:int) self.hp=n", -37,
         [&] { bus.emit(ayt::event::ScriptTestPingEvent{-37}); }},
        {"await wait_event(\"device_action\") into (id:int,pressed:bool) if !pressed { self.hp=id }", 73,
         [&] { bus.emit(ayt::event::DeviceActionEvent{73,false}); }},
        {"await wait_event(\"task_complete\") into (id:int,ok:bool) if !ok { self.hp=id }", 4294967299.0,
         [&] { bus.emit(ayt::event::TaskCompleteEvent{4294967299ull,false}); }},
        {"await wait_event(\"physics_collision\") into (a:int,b:int,kind:int) self.hp=a+b+kind", 4294967303.0,
         [&] { bus.emit(ayt::event::PhysicsCollisionEvent{4294967295u,7,1}); }}
    };
    for (const auto &witness : cases) {
        LogiaRuntimeBridge bridge;
        std::vector<logia::CompilerError> errors;
        const auto source = std::string("script A { function work() { ") + witness.body +
            " } on_start() { tasks.start(work) } }";
        const bool loaded = bridge.loadScript("A", source, context(), errors);
        CHECK(loaded); if (!loaded) return;
        ayt::entity::ActorScriptComponent actor;
        actor.properties["hp"] = 0.0;
        CHECK(bridge.callActorLifecycle("A", "on_start", actor));
        bridge.tickTasks(0);
        witness.emit();
        CHECK_FLOAT_EQ(hp(actor), 0.0, 0.001);
        bridge.tickTasks(0);
        CHECK_FLOAT_EQ(hp(actor), witness.expected, 0.001);
        CHECK(bridge.getLastError().luaMessage.empty());
        bridge.releaseInstance("A", &actor);
    }
}
TEST_CASE(repeated_checked_compiles_keep_transform_descriptor_and_fields_stable)
{
    CHECK(accepts("script A { on_start() { self.transform.position=vec3.new(1,2,3) } }"));
    auto &registry = ayt::reflect::TypeRegistryImpl::instance();
    const auto *type = registry.findType<ayt::entity::Transform>();
    CHECK(type != nullptr); if (!type) return;
    const auto *position = type->findField("position");
    const auto *scale = type->findField("scale");
    const auto *rotation = type->findField("rotation");
    CHECK(position != nullptr); CHECK(scale != nullptr); CHECK(rotation != nullptr);
    for (int i = 0; i < 32; ++i) {
        CHECK(accepts("script A { function work(n:int) { await delay(0) self.hp=n } on_start() { tasks.start(work,7) } }"));
        const auto *current = registry.findType<ayt::entity::Transform>();
        CHECK(current == type);
        CHECK(current->findField("position") == position);
        CHECK(current->findField("scale") == scale);
        CHECK(current->findField("rotation") == rotation);
    }
}
TEST_SUITE_END
