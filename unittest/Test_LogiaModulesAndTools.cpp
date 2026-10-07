#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/ScriptTestEvents.h>
#include <AYScript/ScriptRuntimeBridge.h>
#include <AYScript/ScriptSubSystem.h>
#include <AYScript/logia/LogiaPipeline.h>
#include <AYScript/logia/Package.h>
#include <AYScript/logia/Tooling.h>
#include <AYTest.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
using namespace ayt::script;
namespace
{
struct Workspace
{
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("logia_tools_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Workspace()
    {
        std::filesystem::create_directories(root);
    }
    ~Workspace()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    void put(const char *path, const std::string &source)
    {
        auto file = root / path;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out << source;
    }
    logia::LogiaHostContext context(const char *path = "main.logia") const
    {
        auto ctx = logia::checkedLogiaHostContext(logia::actorLogiaHostContext());
        ctx.actorProperties = {{"hp", "float"}};
        ctx.assetRoot = root.generic_string();
        ctx.sourcePath = (root / path).generic_string();
        return ctx;
    }
};
logia::LogiaHostContext context()
{
    auto ctx = logia::checkedLogiaHostContext(logia::actorLogiaHostContext());
    ctx.actorProperties = {{"hp", "float"}};
    return ctx;
}
} // namespace
TEST_SUITE(LogiaModulesAndTools)
TEST_CASE(modules_share_helpers_but_recompile_changed_dependency)
{
    Workspace files;
    files.put("helpers/math.logia",
              "script Helpers { function twice(n: int): int { return n*2 } }");
    const char *main =
        "script Main { import \"helpers/math.logia\"; on_start() { self.hp = twice(3) } }";
    files.put("main.logia", main);
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("Main", main, files.context(), errors));
    CHECK(bridge.scriptDependencies("Main").size() == 1);
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 6.0, 0.001);
    files.put("helpers/math.logia",
              "script Helpers { function twice(n: int): int { return n*3 } }");
    CHECK(bridge.reloadScript("Main", main, files.context(), errors));
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 9.0, 0.001);
    files.put("helpers/math.logia",
              "script Helpers { function twice(n: int): int { return false } }");
    CHECK(!bridge.reloadScript("Main", main, files.context(), errors));
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 9.0, 0.001);
}
TEST_CASE(module_rejects_cycle_escape_missing_and_shared_exports)
{
    Workspace files;
    files.put("a.logia", "script A { import \"b.logia\"; }");
    files.put("b.logia", "script B { import \"a.logia\"; }");
    CHECK(
        !logia::compileLogiaToLua("script Main { import \"a.logia\"; }", files.context()).success);
    CHECK(!logia::compileLogiaToLua("script Main { import \"../outside.logia\"; }", files.context())
               .success);
    CHECK(!logia::compileLogiaToLua("script Main { import \"missing.logia\"; }", files.context())
               .success);
    files.put("a.logia", "script A { var hp: int=1 }");
    CHECK(
        !logia::compileLogiaToLua("script Main { import \"a.logia\"; }", files.context()).success);
    CHECK(!logia::compileLogiaToLua("script Main { import \"a.logia\"; }", context()).success);
}
TEST_CASE(typed_arrays_validate_elements_and_guard_runtime_bounds)
{
    CHECK(!logia::compileLogiaToLua("script Main { var a: array<int> = {1,true} }", context())
               .success);
    CHECK(!logia::compileLogiaToLua(
               "script Main { var a: array<int> = {1} on_start() { array.push(a, false) } }",
               context())
               .success);
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.loadScript("Main",
                            "script Main { state a: array<int> = {2,3} on_start() { "
                            "array.push(a,5) a[1]=7 self.hp=a[1]+array.pop(a)+array.length(a) } }",
                            context(), errors));
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 14.0, 0.001);
    CHECK(bridge.reloadScript(
        "Main", "script Main { state a: array<int> = {2,3} on_start() { self.hp=a[0] } }",
        context(), errors));
    CHECK(!bridge.callActorLifecycle("Main", "on_start", actor));
    CHECK(bridge.getLastError().luaMessage.find("bounds") != std::string::npos);
}
TEST_CASE(nested_arrays_and_default_empty_arrays_are_typed)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.loadScript("Main",
                            "script Main { var rows: array<array<int>>={{1,2},{3}} on_start() { "
                            "var a: array<int>; array.push(a,rows[2][1]); self.hp=array.pop(a) } }",
                            context(), errors));
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 3.0, 0.001);
}
TEST_CASE(math_uses_canonical_vector_and_quaternion_conventions)
{
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.loadScript("Main",
                            "script Main { on_start() { var up: vec3={x=0,y=1,z=0} var q: "
                            "quat=quat.axis_angle(up,1.5707963) var v: "
                            "vec3=quat.rotate(q,vec3.new(0,0,1)) self.hp=v.x } }",
                            context(), errors));
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 1.0, 0.001);
    CHECK(bridge.reloadScript(
        "Main", "script Main { on_start() { var q: quat=quat.axis_angle(vec3.new(0,0,0),1) } }",
        context(), errors));
    CHECK(!bridge.callActorLifecycle("Main", "on_start", actor));
}
TEST_CASE(event_callbacks_match_actual_positional_payload)
{
    CHECK(!logia::compileLogiaToLua("script Main { function ping(e: table) {} on_start() { "
                                    "event.subscribe(\"script_test_ping\",ping) } }",
                                    context())
               .success);
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    int witness = 0;
    CHECK(bridge.registerHostFunction(
        "probe.value", {{"int"}, "void"}, [&](const auto &args) -> ScriptHostValue {
            witness = static_cast<int>(std::get<std::int64_t>(args[0]));
            return {};
        }));
    CHECK(bridge.loadScript("Main",
                            "script Main { function ping(n: int) { probe.value(n) } on_start() { "
                            "event.subscribe(\"script_test_ping\",ping) } }",
                            context(), errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{17});
    CHECK(witness == 17);
    bridge.releaseInstance("Main", &actor);
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{22});
    CHECK(witness == 17);
}
TEST_CASE(state_initializers_can_read_shared_and_prior_state_and_survive_repeated_reload)
{
    const char *source = "script Main { var shared: int=3 state a: int=shared state b: int=a+2 "
                         "on_start() { b+=1 self.hp=b } on_update(dt: float) { self.hp=b } }";
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.loadScript("Main", source, context(), errors));
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    CHECK(bridge.reloadScript("Main", source, context(), errors));
    CHECK(bridge.reloadScript("Main", source, context(), errors));
    CHECK(bridge.callActorLifecycle("Main", "on_update", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 6.0, 0.001);
}
TEST_CASE(tooling_completion_is_schema_driven_and_definition_uses_resolved_identity)
{
    auto ctx = context();
    ctx.sourcePath = "main.logia";
    const char *source =
        "script Main {\n var score: int=1\n function double_it(n: int): int {\n return n*2\n }\n "
        "on_start() {\n var score: int=4\n self.hp=double_it(score)\n }\n}\n";
    auto analysis = logia::analyzeLogia(source, ctx);
    CHECK(analysis.success);
    auto completions = analysis.complete("self.h");
    CHECK(completions.size() == 1);
    CHECK(completions[0].type == "float");
    auto variables = analysis.complete("score", "main.logia", 8);
    CHECK(variables.size() == 1);
    CHECK(variables[0].definition.line == 7);
    auto function = analysis.definitionAt({"main.logia", 8, 10});
    CHECK(function.has_value());
    if (function)
        CHECK(function->line == 3);
    auto variable = analysis.definitionAt({"main.logia", 8, 20});
    CHECK(variable.has_value());
    if (variable)
        CHECK(variable->line == 7);
    auto invalid = logia::analyzeLogia("script Main { on_start() {", ctx);
    CHECK(!invalid.success);
    CHECK(!invalid.complete("self.hp").empty());
}
TEST_CASE(imported_runtime_failure_reports_source_file_and_call_frames)
{
    Workspace files;
    files.put("helper.logia", "script Helper {\n function broken(): int {\n var a: "
                              "array<int>={1}\n return a[2]\n }\n}\n");
    const char *source =
        "script Main {\n import \"helper.logia\";\n on_start() {\n self.hp=broken()\n }\n}\n";
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    const bool loaded = bridge.loadScript("Main", source, files.context(), errors);
    if (!loaded) for (const auto& error : errors) std::fprintf(stderr, "load: %s\n", error.message.c_str());
    CHECK(loaded);
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(!bridge.callActorLifecycle("Main", "on_start", actor));
    auto failure = bridge.getLastError();
    CHECK(failure.translated);
    CHECK(failure.logiaLoc.file.ends_with("helper.logia"));
    CHECK(failure.logiaLoc.line == 4);
    CHECK(failure.frames.size() >= 2);
}
TEST_CASE(package_runs_after_authoring_sources_are_removed_and_detects_corruption)
{
    Workspace files;
    files.put("src/helper.logia", "script Helper { function twice(n: int): int { return n*2 } }");
    files.put("src/main.logia",
              "script Main { import \"helper.logia\"; on_start() { self.hp=twice(5) } }");
    auto ctx = files.context("src/main.logia");
    ctx.assetRoot = (files.root / "src").generic_string();
    auto package =
        logia::buildLogiaPackage(ctx.sourcePath, (files.root / "published").generic_string(), ctx);
    CHECK(package.success);
    std::filesystem::remove(files.root / "src/main.logia");
    std::filesystem::remove(files.root / "src/helper.logia");
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScriptPackage(package.manifestPath, errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 10.0, 0.001);
    files.put("published/helper.logia",
              "script Helper { function twice(n: int): int { return n*99 } }");
    CHECK(!bridge.loadScriptPackage(package.manifestPath, errors));
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 10.0, 0.001);
}
TEST_CASE(package_requires_matching_registered_providers_and_preserves_existing_output)
{
    Workspace files;
    files.put("main.logia", "script Main { on_start() { self.hp=stats.read(\"hp\") } }");
    auto ctx = files.context();
    ctx.hostFunctions["stats.read"] = {{"string"}, "float"};
    auto package =
        logia::buildLogiaPackage(ctx.sourcePath, (files.root / "published").generic_string(), ctx);
    CHECK(package.success);
    CHECK(
        !logia::buildLogiaPackage(ctx.sourcePath, (files.root / "published").generic_string(), ctx)
             .success);
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(!bridge.loadScriptPackage(package.manifestPath, errors));
    CHECK(bridge.registerHostFunction("stats.read", {{"string"}, "float"},
                                      [](const auto &) -> ScriptHostValue { return 35.0; }));
    CHECK(bridge.loadScriptPackage(package.manifestPath, errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 35.0, 0.001);
}
TEST_CASE(reload_hook_reconnects_callbacks_and_restarts_tasks_without_repeating_start)
{
    const char* source="script Main { state starts: int=0 function ping(n: int) { self.hp=n } function work() { await delay(0.1); self.hp=99 } on_start() { starts+=1 event.subscribe(\"script_test_ping\",ping) } on_reload() { event.subscribe(\"script_test_ping\",ping) tasks.start(work) } on_update(dt:float) { self.hp=starts } }";
    LogiaRuntimeBridge bridge;std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("Main",source,context(),errors));ayt::entity::ActorScriptComponent actor;actor.properties["hp"]=0.0;
    CHECK(bridge.callActorLifecycle("Main","on_start",actor));CHECK(bridge.reloadScript("Main",source,context(),errors));
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{17});CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),17.0,0.001);
    CHECK(bridge.callActorLifecycle("Main","on_update",actor));CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),1.0,0.001);
    bridge.tickTasks(0);bridge.tickTasks(0.2f);CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),99.0,0.001);
    bridge.releaseInstance("Main",&actor);
}
TEST_CASE(dependency_watcher_reload_tracks_imported_source_changes)
{
    Workspace files;files.put("helper.logia","script Helper { function value(): int { return 1 } }");
    const char* source="script Main { import \"helper.logia\"; on_start() { self.hp=value() } }";files.put("main.logia",source);
    ScriptSubSystem subsystem;CHECK(subsystem.initialize());std::vector<logia::CompilerError> errors;
    CHECK(subsystem.bridge().loadScript("Main",source,files.context(),errors));
    CHECK(subsystem.watchScriptPath("Main",files.context().sourcePath,files.context()));subsystem.setHotReloadEnabled(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    files.put("helper.logia","script Helper { function value(): int { return 5 } }");
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while(subsystem.hotReloadApplyCount()==0 && std::chrono::steady_clock::now()<deadline){subsystem.update(0);std::this_thread::sleep_for(std::chrono::milliseconds(25));}
    CHECK(subsystem.hotReloadApplyCount()>0);
    ayt::entity::ActorScriptComponent actor;actor.properties["hp"]=0.0;CHECK(subsystem.bridge().callActorLifecycle("Main","on_start",actor));CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),5.0,0.001);subsystem.shutdown();
}
TEST_CASE(checked_contract_diagnoses_expression_statements_and_legacy_new_syntax)
{
    CHECK(!logia::compileLogiaToLua("script Main { on_start() { 1+2 } }",context()).success);
    CHECK(!logia::compileLogiaToLua("script Main { on_start() { var x:int=0 self.hp=(x=1) } }",context()).success);
    CHECK(!logia::compileLogiaToLua("script Main { var a:float=self.hp }",context()).success);
    CHECK(!logia::compileLogiaToLua("script Main { function f():int { return 1 } }").success);
    CHECK(!logia::compileLogiaToLua("script Main { function f() { await delay(1) } }").success);
}
TEST_CASE(imported_payload_bindings_keep_original_source_and_survive_portable_package_loading)
{
    Workspace files;
    files.put("helpers/event.logia", R"logia(script Helpers {
    function receive(base:int) {
        await wait_event("script_test_ping") into (n:int)
        self.hp=base+n
    }
})logia");
    const char *source = "script Main { import \"helpers/event.logia\" on_start() { tasks.start(receive,5) } }";
    files.put("main.logia", source);
    auto ctx = files.context();
    auto analysis = logia::analyzeLogia(source, ctx);
    CHECK(analysis.success);
    const auto helper = (files.root / "helpers/event.logia").generic_string();
    auto symbols = analysis.complete("n", helper, 4);
    CHECK(symbols.size() == 1);
    auto definition = analysis.definitionAt({helper, 4, 22});
    CHECK(definition.has_value());
    if (definition) { CHECK(definition->file == helper); CHECK(definition->line == 3); }
    auto package = logia::buildLogiaPackage(ctx.sourcePath, (files.root / "published").generic_string(), ctx);
    CHECK(package.success);
    if (!package.success) return;
    std::filesystem::remove(files.root / "main.logia");
    std::filesystem::remove(files.root / "helpers/event.logia");
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScriptPackage(package.manifestPath, errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    bridge.tickTasks(0);
    ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{17});
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 22.0, 0.001);
}
TEST_CASE(reusable_behaviors_compose_hooks_state_persistence_and_reverse_teardown)
{
    Workspace files;
    files.put("counter.logia",R"(script Counter {
        persist count:int=1
        signal changed(value:int)
        function increase() { count+=1 emit("changed",count) }
        on_start() { var local_value:int=7 self.hp=local_value return }
        on_destroy() { self.hp=self.hp*10+1 }
    })");
    files.put("feedback.logia",R"(script Feedback {
        import "counter.logia"
        state extra:int=0
        on_start() { var local_value:int=3 self.hp=self.hp+local_value }
        on_destroy() { self.hp=self.hp*10+2 }
    })");
    const char* main=R"(script Main version 2 {
        import "counter.logia"
        import "feedback.logia"
        on_start() { self.hp=self.hp+1 increase() }
        on_update() { self.hp=count+extra }
        on_destroy() { self.hp=3 }
    })";
    files.put("main.logia",main); LogiaRuntimeBridge bridge; std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("Main",main,files.context(),errors));
    ayt::entity::ActorScriptComponent first,second; first.properties["hp"]=0.0;second.properties["hp"]=0.0;
    CHECK(bridge.callActorLifecycle("Main","on_start",first));
    CHECK_FLOAT_EQ(std::get<double>(first.properties["hp"]),11.0,0.001);
    CHECK(bridge.callActorLifecycle("Main","on_update",first));
    CHECK_FLOAT_EQ(std::get<double>(first.properties["hp"]),2.0,0.001);
    CHECK(bridge.callActorLifecycle("Main","on_update",second));
    CHECK_FLOAT_EQ(std::get<double>(second.properties["hp"]),1.0,0.001);
    auto saved=bridge.snapshotPersistentState("Main",&first); CHECK(saved.success);
    CHECK(saved.json.find("count")!=std::string::npos);
    CHECK(bridge.callActorLifecycle("Main","on_destroy",first));
    CHECK_FLOAT_EQ(std::get<double>(first.properties["hp"]),321.0,0.001);
    auto package=logia::buildLogiaPackage((files.root/"main.logia").generic_string(),(files.root/"package").generic_string(),files.context());
    CHECK(package.success);
    LogiaRuntimeBridge published; CHECK(published.loadScriptPackage(package.manifestPath,errors));
    CHECK(published.callActorLifecycle("Main","on_start",first));
    published.releaseInstance("Main",&first);bridge.releaseInstance("Main",&second);
}
TEST_CASE(behavior_conflicts_and_invalid_lifecycle_contracts_fail_at_compile_time)
{
    Workspace files;
    files.put("bad.logia","script Bad { on_update(dt:int){} }");
    CHECK(!logia::compileLogiaToLua("script Main { import \"bad.logia\" }",files.context()).success);
    files.put("bad.logia","script Bad { on_start(){ await delay(0) } }");
    CHECK(!logia::compileLogiaToLua("script Main { import \"bad.logia\" }",files.context()).success);
    files.put("bad.logia","script Bad { on_start(){} on_start(){} }");
    CHECK(!logia::compileLogiaToLua("script Main { import \"bad.logia\" }",files.context()).success);
    files.put("bad.logia","script Bad { state same:int=1 }");
    CHECK(!logia::compileLogiaToLua("script Main { import \"bad.logia\" state same:int=2 }",files.context()).success);
    files.put("bad.logia","script Bad version 2 { state count:int=0 }");
    CHECK(!logia::compileLogiaToLua("script Main { import \"bad.logia\" }",files.context()).success);
}
TEST_SUITE_END
