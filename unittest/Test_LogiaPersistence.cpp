#include <AYScript/ScriptRuntimeBridge.h>
#include <AYScript/logia/LogiaPipeline.h>
#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/ScriptTestEvents.h>
#include <AYTest.h>
#include <nlohmann/json.hpp>
using namespace ayt::script;
namespace {
auto context() { auto ctx=logia::checkedLogiaHostContext(logia::actorLogiaHostContext());
    ctx.actorProperties={{"hp","float"}}; return ctx; }
const char* source=R"(
script Durable version 1 {
    persist score: int = 7
    persist samples: array<int> = {1,2}
    persist position: vec3 = vec3.new(3,4,5)
    state transient_count: int = 0
    state task: int = 0
    function pulse(value:int) { transient_count += value self.hp=score+transient_count }
    function later() { await delay(1.0) score=999 self.hp=score }
    on_start() {
        transient_count=10 event.subscribe("script_test_ping",pulse)
        task=tasks.start(later) self.hp=score+transient_count
    }
    on_restore() {
        event.subscribe("script_test_ping",pulse)
        self.hp=score+transient_count
        array.push(samples,3)
    }
    on_update() { self.hp=score+transient_count+array.length(samples) }
})";
}
TEST_SUITE(LogiaPersistenceTests)
TEST_CASE(snapshot_is_opt_in_restore_resets_transients_and_rebuilds_one_subscription)
{
    LogiaRuntimeBridge bridge; std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("Durable",source,context(),errors));
    ayt::entity::ActorScriptComponent actor; actor.properties["hp"]=0.0;
    CHECK(bridge.callActorLifecycle("Durable","on_start",actor));
    bridge.tickTasks(0.0f);
    auto snapshot=bridge.snapshotPersistentState("Durable",&actor);
    CHECK_MSG(snapshot.success,snapshot.error.c_str());
    auto data=nlohmann::json::parse(snapshot.json);
    CHECK(data["fields"].size()==3u);
    CHECK(!data["fields"].contains("task"));
    CHECK(!data["fields"].contains("transient_count"));
    data["fields"]["score"]="wrong";
    auto rejected=bridge.restorePersistentState("Durable",&actor,data.dump());
    CHECK(!rejected.success);
    CHECK(bridge.snapshotPersistentState("Durable",&actor).json==snapshot.json);
    auto restored=bridge.restorePersistentState("Durable",&actor,snapshot.json);
    CHECK_MSG(restored.success,restored.error.c_str());
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),7.0,0.001);
    bridge.tickTasks(2.0f);
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{2});
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),9.0,0.001);
    CHECK(bridge.callActorLifecycle("Durable","on_update",actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),12.0,0.001);
    bridge.releaseInstance("Durable",&actor);
}
TEST_CASE(older_versions_require_pure_migration_and_future_or_corrupt_schemas_are_rejected)
{
    LogiaRuntimeBridge bridge; std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("Versioned",R"(script Versioned version 2 {
        persist score:int=0
        function migrate(from:int,data:table):table { return {score=data["old_score"]} }
        on_start() { self.hp=score }
        on_restore() { self.hp=score }
    })",context(),errors));
    ayt::entity::ActorScriptComponent actor; actor.properties["hp"]=0.0;
    CHECK(bridge.callActorLifecycle("Versioned","on_start",actor));
    nlohmann::json data={{"format",1},{"script","Versioned"},{"version",1},
                        {"schema",{{"old_score","int"}}},{"fields",{{"old_score",42}}}};
    auto restored=bridge.restorePersistentState("Versioned",&actor,data.dump());
    CHECK_MSG(restored.success,restored.error.c_str()); CHECK(restored.migrated);
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),42.0,0.001);
    const auto good=bridge.snapshotPersistentState("Versioned",&actor).json;
    data["version"]=3;
    CHECK(!bridge.restorePersistentState("Versioned",&actor,data.dump()).success);
    CHECK(!bridge.restorePersistentState("Versioned",&actor,"{broken").success);
    CHECK(bridge.snapshotPersistentState("Versioned",&actor).json==good);
    CHECK(!logia::compileLogiaToLua(R"(script Bad { persist value:table={}
        on_start(){} })",context()).success);
    CHECK(!logia::compileLogiaToLua(R"(script Bad version 0 { on_start(){} })",context()).success);
    CHECK(!logia::compileLogiaToLua(R"(script Bad version 2 {
        function migrate(from:int,data:table):table { entity.create("side-effect") return data }
    })",context()).success);
    bridge.releaseInstance("Versioned",&actor);
}
TEST_CASE(storage_ambient_is_instance_scoped_and_rejected_restore_keeps_live_work)
{
    LogiaRuntimeBridge bridge; std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("Ambient",R"(script Ambient version 3 {
        persist score:int=1
        state snapshot:string=""
        on_start(){ score=score+1 snapshot=storage.snapshot()
            if storage.version()==3 && !storage.restore("invalid") && storage.error()!="" { self.hp=score }
        }
        on_update(){ score=100 if storage.restore(snapshot) { self.hp=score } }
    })",context(),errors));
    ayt::entity::ActorScriptComponent first,second;
    first.properties["hp"]=0.0;second.properties["hp"]=0.0;
    CHECK(bridge.callActorLifecycle("Ambient","on_start",first));
    CHECK(bridge.callActorLifecycle("Ambient","on_start",second));
    CHECK(bridge.callActorLifecycle("Ambient","on_update",first));
    CHECK_FLOAT_EQ(std::get<double>(first.properties["hp"]),2.0,0.001);
    CHECK(nlohmann::json::parse(bridge.snapshotPersistentState("Ambient",&second).json)["fields"]["score"]==2);
    bridge.releaseInstance("Ambient",&first);bridge.releaseInstance("Ambient",&second);
}
TEST_SUITE_END
