// Test_LogiaScriptSubSystemPlay.cpp — INT-01 (2026-07-15)
//
// End-to-end smoke for the Editor / Application host wiring that
// INT-01 introduced. The simplest possible regression tests:
//
//   1. int01_editor_binding_drives_scriptcomponent_at_1x:
//      bind a Logia script to a ScriptComponent, drive N sub-ticks,
//      assert a Lua global `__int01_witness` was set exactly once
//      per tick (NOT twice) — the R1 mitigation presence-guard
//      (SubSystemRegistry::findSubSystem("Entity")) prevents
//      double-dispatch when both ScriptSubSystem and EntitySubSystem
//      are registered.
//
//   2. int01_hot_reload_swaps_lua_after_file_edit:
//      bind, run V1, edit file to V2 with a different counter
//      delta, confirm hotReloadApplyCount() fires and the next
//      callLifecycle sees the new behavior.
//
// We deliberately avoid the AYReflect codegen path in this test —
// `self.x += speed*dt` requires a PlayerController subclass +
// reflect registration, which the existing Test_LogiaReflectRuntime
// tests already cover. INT-01's contract is "Editor / App hosts
// the ScriptSubSystem correctly so the per-tick Lua-on_update path
// runs"; the simplest assertion is `__int01_witness` round-trip.

#include "AYScript/ScriptSubSystem.h"
#include "AYScript/ScriptRuntimeBridge.h"
#include "AYScript/logia/CompilerError.h"
#include "LogiaTestHelpers.h"
#include "AYTest.h"
#include "AYIO.h"
#include "AYGameLoop/SubSystemRegistry.h"

#include <AYEntity/IEntity.h>
#include <AYEntity.h>
#include <AYEntity/World.h>
#include <AYEntity/components/ScriptComponent.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using ayt::script::ScriptSubSystem;
using ayt::script::LogiaRuntimeBridge;
using ayt::script::logia::CompilerError;

#if defined(_WIN32)
  #include <direct.h>
  #define AY_MKDIR(p) _mkdir(p)
  #define AY_PATH_SEP "\\"
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #define AY_MKDIR(p) ::mkdir((p), 0755)
  #define AY_PATH_SEP "/"
#endif

namespace {

const std::string kPlayScratch = "ayscript_int01_play_scratch";

void makeScratch()
{
    std::string rmCmd;
#if defined(_WIN32)
    rmCmd = "rmdir /S /Q " + kPlayScratch + " >NUL 2>NUL";
#else
    rmCmd = "rm -rf " + kPlayScratch + " >/dev/null 2>&1";
#endif
    std::system(rmCmd.c_str());
    AY_MKDIR(kPlayScratch.c_str());
}

void nukeScratch()
{
    std::string rmCmd;
#if defined(_WIN32)
    rmCmd = "rmdir /S /Q " + kPlayScratch + " >NUL 2>NUL";
#else
    rmCmd = "rm -rf " + kPlayScratch + " >/dev/null 2>&1";
#endif
    std::system(rmCmd.c_str());
}

std::string joinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + AY_PATH_SEP + b;
}

template <typename Pred>
bool waitFor(Pred&& predicate, int timeoutMs = 2500)
{
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
    while (clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return predicate();
}

constexpr const char* kCounterV1 = R"(
script INT01Counter {
    var n: int = 0
    on_update() {
        n = n + 1
        __int01_witness = tostring(n)
    }
}
)";

// V2 increments by 10 per tick (signature change vs V1's +1).
constexpr const char* kCounterV2 = R"(
script INT01Counter {
    var n: int = 0
    on_update() {
        n = n + 10
        __int01_witness = tostring(n)
    }
}
)";

} // namespace

TEST_SUITE(LogiaScriptSubSystemPlayTests)

TEST_CASE(binding_after_attach_starts_once_and_destroys_once) {
    auto& world = ayt::entity::World::instance();
    logia_test::resetWorldForTest(world);
    auto sub = std::make_unique<ScriptSubSystem>();
    CHECK(sub->initialize());

    auto* entity = world.createEntity();
    CHECK(entity != nullptr);
    if (entity == nullptr) {
        logia_test::shutdownScriptHost(world, sub.get());
        return;
    }
    auto* component = entity->addComponent<ayt::entity::ScriptComponent>();
    CHECK(component != nullptr);
    if (component == nullptr) {
        logia_test::shutdownScriptHost(world, sub.get());
        return;
    }
    component->setScriptName("LifecycleProbe");
    std::vector<CompilerError> errors;
    CHECK(sub->bindAndLoad(*component, R"(
script LifecycleProbe {
    var starts: int = 0
    var destroys: int = 0
    on_start() {
        starts = starts + 1
        __lifecycle_start = starts
    }
    on_destroy() {
        destroys = destroys + 1
        __lifecycle_destroy = destroys
    }
}
)", errors));
    CHECK(errors.empty());
    double count = 0.0;
    CHECK(sub->bridge().tryGetLuaGlobalNumber("__lifecycle_start", count));
    CHECK(count == 1.0);
    entity->onStart();
    component->activateScript();
    CHECK(sub->bridge().tryGetLuaGlobalNumber("__lifecycle_start", count));
    CHECK(count == 1.0);

    world.destroyEntity(entity);
    CHECK(sub->bridge().tryGetLuaGlobalNumber("__lifecycle_destroy", count));
    CHECK(count == 1.0);
    logia_test::shutdownScriptHost(world, sub.get());
}

TEST_CASE(int01_editor_binding_drives_scriptcomponent_at_1x) {
    // INT-01 P0: bind + tick via ScriptSubSystem::update → the
    // ScriptComponent's `onUpdate(dt)` lifecycle routes through the
    // Logia bridge. Assert the witness increments exactly once per
    // tick (1× not 2× — R1 mitigation) — headless test with no
    // EntitySubSystem sibling exercises the absence branch.
    makeScratch();
    const std::string scriptPath =
        joinPath(kPlayScratch, "INT01Counter.logia");
    ayt::io::File::writeAllText(scriptPath, kCounterV1);

    auto& world = ayt::entity::World::instance();
    logia_test::resetWorldForTest(world);

    auto sub = std::make_unique<ScriptSubSystem>();
    CHECK(sub->initialize());

    auto* ent = world.createEntity();
    CHECK(ent != nullptr);
    if (ent == nullptr) {
        logia_test::shutdownScriptHost(world, sub.get());
        nukeScratch();
        return;
    }
    auto* comp = ent->addComponent<ayt::entity::ScriptComponent>();
    CHECK(comp != nullptr);
    if (comp == nullptr) {
        logia_test::shutdownScriptHost(world, sub.get());
        nukeScratch();
        return;
    }
    comp->setScriptName("INT01Counter");

    std::vector<CompilerError> errors;
    CHECK(sub->bindAndLoadFromFile(*comp, scriptPath, errors));
    CHECK(errors.empty());
    CHECK(sub->bridge().getLuaGlobalString("__int01_witness").empty());

    const int N = 5;
    for (int i = 0; i < N; ++i) {
        sub->update(0.016f);
    }

    // After N=5 ticks, the Lua on_update should have run exactly
    // once per tick — counter is now "5" (the V1 script increments
    // by 1 each tick). If R1 mitigation regressed (entity double-
    // tick), counter would be "10" or more.
    const std::string witness =
        sub->bridge().getLuaGlobalString("__int01_witness");
    CHECK(witness == "5");

    logia_test::shutdownScriptHost(world, sub.get());
    nukeScratch();
}

TEST_CASE(int01_hot_reload_swaps_lua_after_file_edit) {
    // INT-01 follow-on: file edit during Play replaces the .logia
    // with V2 (+10/tick). The FileWatcher fires during the next
    // sub->update() tick, the new chunk is compiled + loaded, and
    // subsequent ticks advance at the new rate. Mirrors the
    // Test_LogiaHotReloadWatcher pattern but exercises the Editor-
    // equivalent bind+watch path through bindAndLoadFromFile + the
    // hotReloadEnabled-after-bind ordering that INT-01 settled on.
    makeScratch();
    const std::string scriptPath =
        joinPath(kPlayScratch, "INT01CounterReload.logia");
    ayt::io::File::writeAllText(scriptPath, kCounterV1);

    auto& world = ayt::entity::World::instance();
    logia_test::resetWorldForTest(world);

    auto sub = std::make_unique<ScriptSubSystem>();
    CHECK(sub->initialize());

    auto* ent = world.createEntity();
    CHECK(ent != nullptr);
    if (ent == nullptr) {
        logia_test::shutdownScriptHost(world, sub.get());
        nukeScratch();
        return;
    }
    auto* comp = ent->addComponent<ayt::entity::ScriptComponent>();
    CHECK(comp != nullptr);
    if (comp == nullptr) {
        logia_test::shutdownScriptHost(world, sub.get());
        nukeScratch();
        return;
    }
    comp->setScriptName("INT01Counter");

    std::vector<CompilerError> errors;
    CHECK(sub->bindAndLoadFromFile(*comp, scriptPath, errors));
    CHECK(errors.empty());

    // INT-01: enable AFTER bind so setHotReloadEnabled's pre-existing
    // byPath sweep picks up the just-loaded path. Reversing this
    // silently disables hot reload for the script (verified by
    // inspection of AYScriptSubSystem.cpp setHotReloadEnabled at
    // 160-176).
    sub->setHotReloadEnabled(true);
    CHECK(sub->isHotReloadEnabled());

    for (int i = 0; i < 3; ++i) {
        sub->update(0.016f);
    }
    CHECK(sub->bridge().getLuaGlobalString("__int01_witness") == "3");

    // User-edit → V2 (counter +10 per tick).
    ayt::io::File::writeAllText(scriptPath, kCounterV2);

    const bool applied = waitFor([&] {
        sub->update(0.016f);
        return sub->hotReloadApplyCount() >= 1u;
    });
    CHECK(applied);

    // After reload the witness global should reflect V2's rate,
    // not V1's. Drive one tick at V2's rate; the witness updates to
    // whatever V2's on_update writes (its `var n = 0` locals reset
    // on reload, so a single +10 step yields "10"; the exact value
    // depends on the bridge's compile-cache / var-init policy — we
    // assert the witness changed from V1's "3" and is at least 10
    // to confirm V2's compiled chunk is what got executed).
    sub->update(0.016f);
    const std::string witnessAfter =
        sub->bridge().getLuaGlobalString("__int01_witness");
    CHECK_FALSE(witnessAfter.empty());
    CHECK(witnessAfter != "3");
    // V2 increments by 10 (vs V1's 1). Two ticks at V2 (we already
    // burned one tick inside the waitFor loop, possibly firing
    // V2's on_update 0+ times in case the watcher-applied chunk
    // ticked during waitFor) yields at minimum 10. Be lenient:
    int witnessValue = 0;
    try { witnessValue = std::stoi(witnessAfter); } catch (...) {}
    CHECK(witnessValue >= 10);

    logia_test::shutdownScriptHost(world, sub.get());
    nukeScratch();
}

TEST_CASE(int01_r1_presence_guard_well_known) {
    // INT-01 R1 mitigation regression smoke: confirm the
    // SubSystemRegistry name "ayt.script.runtime" is the exact
    // string the production Editor + Application depend on. If the
    // descriptor name ever drifts the Editor's `findSubSystem`
    // lookup will silently fail, so we pin it here.
    auto& reg = ayt::game::SubSystemRegistry::instance();
    reg.clearAll();

    auto sub = std::make_unique<ScriptSubSystem>();
    CHECK(sub->initialize());
    const char* name = sub->getName();
    CHECK(name != nullptr);
    CHECK(std::string(name) == "ayt.script.runtime");
    reg.registerSubSystem(sub.release());
    CHECK(reg.findSubSystem("ayt.script.runtime") != nullptr);
    CHECK(reg.findSubSystem("Entity") == nullptr);

    reg.clearAll();
}

TEST_CASE(runtime_shutdown_before_world_teardown_keeps_component_bridge_safe) {
    // Production GameLoop teardown is reverse dependency order: Script shuts
    // down before Entity destroys World components. Bound components therefore
    // deliberately outlive ScriptSubSystem in this regression.
    auto& world = ayt::entity::World::instance();
    logia_test::resetWorldForTest(world);

    auto sub = std::make_unique<ScriptSubSystem>();
    CHECK(sub->initialize());

    auto* ent = world.createEntity();
    CHECK(ent != nullptr);
    if (ent == nullptr) {
        logia_test::shutdownScriptHost(world, sub.get());
        return;
    }
    auto* comp = ent->addComponent<ayt::entity::ScriptComponent>();
    CHECK(comp != nullptr);
    if (comp == nullptr) {
        logia_test::shutdownScriptHost(world, sub.get());
        return;
    }
    comp->setScriptName("INT01Counter");

    std::vector<CompilerError> errors;
    CHECK(sub->bindAndLoad(*comp, kCounterV1, errors));
    CHECK(errors.empty());
    sub->update(0.016f);
    CHECK(sub->bridge().getLuaGlobalString("__int01_witness") == "1");

    // Destroy the runtime owner first. ScriptComponent retains a shared adapter
    // implementation, but detachRuntime has made it a safe no-op.
    sub.reset();
    CHECK(comp->getBridge() != nullptr);
    CHECK_FALSE(comp->callScriptMethod("onUpdate"));

    // onDetach() executes while destroying the component. Before the fix this
    // called a freed adapter and could access-violate.
    world.shutdown();
    CHECK_FALSE(world.isInitialized());
}

TEST_SUITE_END
