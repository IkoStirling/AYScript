#include <AYScript/ScriptRuntimeBridge.h>
#include <AYScript/logia/LogiaPipeline.h>
#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYTest.h>
#include <AYIO/File.h>
#include <filesystem>
#include <chrono>
using namespace ayt::script;
TEST_SUITE(LogiaDebugTests)
TEST_CASE(source_breakpoints_preserve_locals_state_and_resume_real_stack)
{
    LogiaRuntimeBridge bridge; std::vector<logia::CompilerError> errors;
    auto ctx=logia::checkedLogiaHostContext(logia::actorLogiaHostContext()); ctx.actorProperties={{"hp","float"}};
    const std::string source="script Debugged {\n state score:int=0\n function add(x:int):int {\n var next:int=x+2\n return next\n }\n on_start() {\n var input:int=3\n score=add(input)\n self.hp=score\n }\n}";
    CHECK(bridge.loadScript("Debugged",source,ctx,errors));
    ayt::entity::ActorScriptComponent actor; actor.properties["hp"]=0.0;
    bridge.setDebugBreakpoints({{"",10}});
    CHECK(bridge.startDebugActorLifecycle("Debugged","on_start",actor));
    CHECK(bridge.debugSnapshot().status==LogiaDebugStatus::Paused);
    CHECK(bridge.resumeDebug(LogiaDebugCommand::Continue));
    auto paused=bridge.debugSnapshot(); CHECK(paused.status==LogiaDebugStatus::Paused);
    CHECK(!paused.frames.empty()); CHECK(paused.frames.front().location.line==10);
    bool local=false; for(auto& v:paused.frames.front().locals)if(v.name=="input"&&v.value=="3")local=true;
    CHECK(local); bool state=false; for(auto& v:paused.state)if(v.name=="score"&&v.value=="5")state=true; CHECK(state);
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),0.0,0.001);
    bridge.setDebugBreakpoints({}); CHECK(bridge.resumeDebug(LogiaDebugCommand::Continue));
    CHECK(bridge.debugSnapshot().status==LogiaDebugStatus::Completed);
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),5.0,0.001);
    CHECK(bridge.startDebugActorLifecycle("Debugged","on_start",actor));
    CHECK(bridge.resumeDebug(LogiaDebugCommand::StepInto));
    CHECK(bridge.debugSnapshot().status==LogiaDebugStatus::Paused);
    bridge.releaseInstance("Debugged",&actor); CHECK(bridge.debugSnapshot().status==LogiaDebugStatus::Idle);
}
TEST_CASE(task_inspection_and_debug_budget_are_bounded)
{
    LogiaRuntimeBridge bridge; std::vector<logia::CompilerError> errors;
    auto ctx=logia::checkedLogiaHostContext(logia::actorLogiaHostContext());
    CHECK(bridge.loadScript("Waiter",R"(script Waiter {
        function wait() { await delay(2.0) }
        on_start() { tasks.start(wait) }
    })",ctx,errors));
    ayt::entity::ActorScriptComponent actor;
    CHECK(bridge.callActorLifecycle("Waiter","on_start",actor)); bridge.tickTasks(0.0f);
    auto tasks=bridge.inspectTasks(); CHECK(tasks.size()==1u); CHECK(tasks.front().wait=="delay");
    CHECK_FLOAT_EQ(tasks.front().remainingSeconds,2.0,0.001);
    CHECK(bridge.loadScript("Loop",R"(script Loop { on_start() { while true {} } })",ctx,errors));
    CHECK(bridge.startDebugActorLifecycle("Loop","on_start",actor));
    bridge.setDebugBreakpoints({}); CHECK(!bridge.resumeDebug(LogiaDebugCommand::Continue));
    CHECK(bridge.debugSnapshot().error.find("budget")!=std::string::npos);
    bridge.shutdown(); CHECK(bridge.debugSnapshot().status==LogiaDebugStatus::Idle);
}
TEST_CASE(step_into_over_and_out_preserve_helper_frames)
{
    LogiaRuntimeBridge bridge; std::vector<logia::CompilerError> errors;
    auto ctx=logia::checkedLogiaHostContext(logia::actorLogiaHostContext()); ctx.actorProperties={{"hp","float"}};
    const char* source="script Steps {\n function helper(x:int):int {\n var next:int=x+1\n return next\n }\n on_start() {\n var input:int=3\n self.hp=helper(input)\n self.hp+=2\n }\n}";
    CHECK(bridge.loadScript("Steps",source,ctx,errors)); ayt::entity::ActorScriptComponent actor; actor.properties["hp"]=0.0;
    CHECK(bridge.startDebugActorLifecycle("Steps","on_start",actor));
    bool entered=false;
    for(int i=0;i<12 && bridge.debugSnapshot().status==LogiaDebugStatus::Paused;++i) {
        auto view=bridge.debugSnapshot();
        if(view.frames.size()>=2) { entered=true; CHECK(bridge.resumeDebug(LogiaDebugCommand::StepOut));
            CHECK(bridge.debugSnapshot().frames.size()==1u); break; }
        CHECK(bridge.resumeDebug(LogiaDebugCommand::StepInto));
    }
    CHECK(entered); CHECK(bridge.resumeDebug(LogiaDebugCommand::Continue));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),6.0,0.001);
    CHECK(bridge.startDebugActorLifecycle("Steps","on_start",actor));
    for(int i=0;i<12 && bridge.debugSnapshot().status==LogiaDebugStatus::Paused;++i) {
        CHECK(bridge.debugSnapshot().frames.size()==1u);
        CHECK(bridge.resumeDebug(LogiaDebugCommand::StepOver));
    }
    CHECK(bridge.debugSnapshot().status==LogiaDebugStatus::Completed); bridge.releaseInstance("Steps",&actor);
}
TEST_CASE(imported_breakpoints_use_original_file_and_line)
{
    namespace fs=std::filesystem;
    auto root=fs::temp_directory_path()/("logia_debug_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup { fs::path path; ~Cleanup(){std::error_code error;fs::remove_all(path,error);} } cleanup{root};
    auto helper=root/"helper.logia"; auto main=root/"main.logia";
    CHECK(ayt::io::File::writeAllText(helper.string(),"script Helpers {\n function value():int {\n var n:int=4\n return n\n }\n}"));
    LogiaRuntimeBridge bridge; std::vector<logia::CompilerError> errors;
    auto ctx=logia::checkedLogiaHostContext(logia::actorLogiaHostContext()); ctx.actorProperties={{"hp","float"}};
    ctx.sourcePath=main.string();ctx.assetRoot=root.string();
    CHECK(bridge.loadScript("Main","script Main { import \"helper.logia\" on_start(){self.hp=value()} }",ctx,errors));
    ayt::entity::ActorScriptComponent actor; actor.properties["hp"]=0.0;
    bridge.setDebugBreakpoints({{helper.string(),4}});CHECK(bridge.startDebugActorLifecycle("Main","on_start",actor));
    CHECK(bridge.resumeDebug(LogiaDebugCommand::Continue));auto view=bridge.debugSnapshot();
    CHECK(view.status==LogiaDebugStatus::Paused);CHECK(!view.frames.empty());
    if(!view.frames.empty()){CHECK(view.frames.front().location.line==4);CHECK(view.frames.front().location.file==helper.generic_string());}
    bridge.setDebugBreakpoints({});CHECK(bridge.resumeDebug(LogiaDebugCommand::Continue));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),4.0,0.001);bridge.releaseInstance("Main",&actor);
}
TEST_SUITE_END
