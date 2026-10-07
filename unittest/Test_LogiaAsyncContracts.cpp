#include <AYScript/ScriptRuntimeBridge.h>
#include <AYScript/logia/LogiaPipeline.h>
#include <AYScript/logia/Package.h>
#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/ScriptTestEvents.h>
#include <AYIO/File.h>
#include <AYTest.h>
#include <filesystem>
#include <chrono>
using namespace ayt::script;
namespace {
auto context(){auto ctx=logia::checkedLogiaHostContext(logia::actorLogiaHostContext());ctx.actorProperties={{"hp","float"}};return ctx;}
const char* timed=R"(script Timed {
    project_event progress(value:int,label:string)
    function wait_progress() {
        await wait_event("progress",0.25) into (received:bool,value:int,label:string)
        if received { self.hp=value } else { self.hp=100+value }
    }
    on_start(){tasks.start(wait_progress)}
})";
}
TEST_SUITE(LogiaAsyncContracts)
TEST_CASE(project_events_are_typed_runtime_scoped_and_timeouts_supply_defaults)
{
    LogiaRuntimeBridge first,second;std::vector<logia::CompilerError> errors;
    CHECK(first.loadScript("Timed",timed,context(),errors));CHECK(second.loadScript("Timed",timed,context(),errors));
    ayt::entity::ActorScriptComponent a,b;a.properties["hp"]=0.0;b.properties["hp"]=0.0;
    CHECK(first.callActorLifecycle("Timed","on_start",a));CHECK(second.callActorLifecycle("Timed","on_start",b));
    first.tickTasks(0);second.tickTasks(0);
    CHECK(!first.emitProjectEvent("progress",{std::string{"bad"},std::string{"text"}}));
    CHECK(!first.projectEventError().empty());
    CHECK(first.emitProjectEvent("progress",{std::int64_t{7},std::string{"text"}}));
    CHECK_FLOAT_EQ(std::get<double>(a.properties["hp"]),0.0,0.001);
    first.tickTasks(0);CHECK_FLOAT_EQ(std::get<double>(a.properties["hp"]),7.0,0.001);
    second.tickTasks(0.3f);CHECK_FLOAT_EQ(std::get<double>(b.properties["hp"]),100.0,0.001);
    CHECK(!first.registerProjectEvent("progress",{"string","string"}));
    first.releaseInstance("Timed",&a);second.releaseInstance("Timed",&b);
}
TEST_CASE(join_waits_until_next_tick_and_completion_notifications_are_owned)
{
    LogiaRuntimeBridge bridge;std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("Joiner",R"(script Joiner {
        state worker:int=0 state successes:int=0
        function finished(id:int,ok:bool){if ok {successes+=1}}
        function work(){await delay(0.25)}
        function watch(id:int){await join(id,1.0) into(done:bool,ok:bool)
            if done && ok {self.hp=42} else {self.hp=-1}}
        on_start(){event.subscribe("logia_task_complete",finished) worker=tasks.start(work) tasks.start(watch,worker)}
        on_update(){self.hp=successes if tasks.status(worker)=="completed" {self.hp+=100}}
    })",context(),errors));
    ayt::entity::ActorScriptComponent actor;actor.properties["hp"]=0.0;
    CHECK(bridge.callActorLifecycle("Joiner","on_start",actor));bridge.tickTasks(0);
    auto tasks=bridge.inspectTasks();CHECK(tasks.size()==2u);
    if(tasks.size()==2u){CHECK(tasks[1].wait=="join");CHECK(tasks[1].joinedTask==tasks[0].id);}
    bridge.tickTasks(0.25f);CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),0.0,0.001);
    bridge.tickTasks(0);CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),42.0,0.001);
    CHECK(bridge.callActorLifecycle("Joiner","on_update",actor));CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),102.0,0.001);
    bridge.releaseInstance("Joiner",&actor);
}
TEST_CASE(join_reports_failed_cancelled_and_timed_out_tasks_without_reentrant_resume)
{
    for(int mode=0;mode<3;++mode) {
        LogiaRuntimeBridge bridge;std::vector<logia::CompilerError> errors;
        const auto work=mode==0?"var a:array<int>={1} self.hp=a[2]":"await delay(10)";
        std::string source="script Failure { state worker:int=0 function work(){"+std::string(work)+R"(}
            function watch(id:int){await join(id,0.1) into(done:bool,ok:bool)
                if done && !ok {self.hp=33} else if !done {self.hp=44}}
            on_start(){worker=tasks.start(work) tasks.start(watch,worker)}
            on_update(){tasks.cancel(worker)}
        })";
        CHECK(bridge.loadScript("Failure",source,context(),errors));ayt::entity::ActorScriptComponent actor;actor.properties["hp"]=0.0;
        CHECK(bridge.callActorLifecycle("Failure","on_start",actor));bridge.tickTasks(0);
        if(mode==1)CHECK(bridge.callActorLifecycle("Failure","on_update",actor));
        bridge.tickTasks(mode==2?0.2f:0.0f);
        CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),mode==2?44.0:33.0,0.001);
        bridge.releaseInstance("Failure",&actor);
    }
}
TEST_CASE(project_schema_payload_and_join_binding_errors_are_compile_time_failures)
{
    CHECK(!logia::compileLogiaToLua(R"(script Bad {project_event progress(value:int)
        on_start(){event.emit("progress",true)}})",context()).success);
    CHECK(!logia::compileLogiaToLua(R"(script Bad {project_event window_close()})",context()).success);
    CHECK(!logia::compileLogiaToLua(R"(script Bad {project_event progress(value:Scene)})",context()).success);
    CHECK(!logia::compileLogiaToLua(R"(script Bad {function wait(){await wait_event("script_test_ping",1) into(value:int)}})",context()).success);
    CHECK(!logia::compileLogiaToLua(R"(script Bad {function wait(){await join(1) into(done:int,ok:bool)}})",context()).success);
    LogiaRuntimeBridge bridge;std::vector<logia::CompilerError> errors;CHECK(bridge.registerProjectEvent("project.hit",{"int","float"}));
    CHECK(bridge.loadScript("Native",R"(script Native {on_start(){event.emit("project.hit",3,4.0)}})",context(),errors));
    ayt::entity::ActorScriptComponent actor;actor.properties["hp"]=0.0;CHECK(bridge.callActorLifecycle("Native","on_start",actor));
    bridge.releaseInstance("Native",&actor);
}
TEST_CASE(packages_keep_author_declared_event_contracts_without_native_schema_setup)
{
    namespace fs=std::filesystem;auto root=fs::temp_directory_path()/("logia_events_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);struct Cleanup{fs::path path;~Cleanup(){std::error_code error;fs::remove_all(path,error);}}cleanup{root};
    auto source=root/"main.logia";CHECK(ayt::io::File::writeAllText(source.string(),timed));
    auto ctx=context();ctx.sourcePath=source.generic_string();ctx.assetRoot=root.generic_string();
    auto package=logia::buildLogiaPackage(source.generic_string(),(root/"published").generic_string(),ctx);CHECK(package.success);
    if(!package.success)return;fs::remove(source);LogiaRuntimeBridge bridge;std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScriptPackage(package.manifestPath,errors));ayt::entity::ActorScriptComponent actor;actor.properties["hp"]=0.0;
    CHECK(bridge.callActorLifecycle("Timed","on_start",actor));bridge.tickTasks(0);
    CHECK(bridge.emitProjectEvent("progress",{std::int64_t{9},std::string{"published"}}));bridge.tickTasks(0);
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),9.0,0.001);bridge.releaseInstance("Timed",&actor);
}
TEST_CASE(coroutine_subscriptions_survive_collection_and_nested_emit_cannot_hide_a_budget_failure)
{
    LogiaRuntimeBridge bridge;std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("Registered",R"(script Registered {
        project_event progress(value:int)
        function handler(value:int){self.hp+=value}
        function wire(){event.subscribe("progress",handler) event.subscribe("script_test_ping",handler)}
        on_start(){tasks.start(wire)}
    })",context(),errors));ayt::entity::ActorScriptComponent actor;actor.properties["hp"]=0.0;
    CHECK(bridge.callActorLifecycle("Registered","on_start",actor));bridge.tickTasks(0);
    CHECK(bridge.loadScript("Collector",R"(script Collector {run(){collectgarbage("collect")}})",logia::toolLogiaHostContext(),errors));
    CHECK(bridge.callLifecycle("Collector","run",nullptr));
    CHECK(bridge.emitProjectEvent("progress",{std::int64_t{3}}));
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{4});
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),7.0,0.001);
    CHECK(bridge.loadScript("Budget",R"(script Budget {project_event budget_ping()
        function stuck(){while true {}}
        on_start(){event.subscribe("budget_ping",stuck) while true {event.emit("budget_ping")}}
    })",context(),errors));
    CHECK(!bridge.callActorLifecycle("Budget","on_start",actor));CHECK(bridge.getLastError().luaMessage.find("budget")!=std::string::npos);
    bridge.releaseInstance("Registered",&actor);bridge.releaseInstance("Budget",&actor);
}
TEST_CASE(failed_completion_handlers_leave_the_owner_context_before_reload)
{
    LogiaRuntimeBridge bridge;std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("Completion",R"(script Completion {
        function done(id:int,ok:bool){while true {}}
        function work(){}
        on_start(){event.subscribe("logia_task_complete",done) tasks.start(work)}
    })",context(),errors));ayt::entity::ActorScriptComponent actor;actor.properties["hp"]=0.0;
    CHECK(bridge.callActorLifecycle("Completion","on_start",actor));bridge.tickTasks(0);
    CHECK(bridge.getLastError().luaMessage.find("budget")!=std::string::npos);
    bridge.releaseInstance("Completion",&actor);
    CHECK(bridge.loadScript("Healthy","script Healthy {on_start(){self.hp=9}}",context(),errors));
    CHECK(bridge.callActorLifecycle("Healthy","on_start",actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),9.0,0.001);bridge.releaseInstance("Healthy",&actor);
}
TEST_SUITE_END
