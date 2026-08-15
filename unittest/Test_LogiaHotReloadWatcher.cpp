// Test_LogiaHotReloadWatcher.cpp — S3.7b FileWatcher integration

#include "AYScript/ScriptSubSystem.h"
#include "AYScript/logia/CompilerError.h"
#include "AYTest.h"
#include "AYIO.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

using ayt::script::ScriptSubSystem;
using ayt::script::logia::CompilerError;

namespace
{

const std::string kScratch = "ayscript_hr_scratch";

void makeScratch()
{
    std::string rmCmd;
#if defined(_WIN32)
    rmCmd = "rmdir /S /Q " + kScratch + " >NUL 2>NUL";
#else
    rmCmd = "rm -rf " + kScratch + " >/dev/null 2>&1";
#endif
    std::system(rmCmd.c_str());
    AY_MKDIR(kScratch.c_str());
}

void nukeScratch()
{
    std::string rmCmd;
#if defined(_WIN32)
    rmCmd = "rmdir /S /Q " + kScratch + " >NUL 2>NUL";
#else
    rmCmd = "rm -rf " + kScratch + " >/dev/null 2>&1";
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

constexpr const char* kScriptV1 = R"(
script Reloadable {
    var n: int = 0
    on_update() {
        n = n + 1
        __test_witness = tostring(n)
    }
}
)";

constexpr const char* kScriptV2 = R"(
script Reloadable {
    var n: int = 0
    on_update() {
        n = n + 10
        __test_witness = tostring(n)
    }
}
)";

} // namespace

TEST_SUITE(LogiaHotReloadWatcherTests)

TEST_CASE(hot_reload_disabled_by_default) {
    auto sys = std::make_unique<ScriptSubSystem>();
    CHECK(sys->isHotReloadEnabled() == false);
    CHECK(sys->hotReloadApplyCount() == 0u);
}

TEST_CASE(watch_and_enable_registers_path) {
    makeScratch();
    const std::string f = joinPath(kScratch, "watch_me.logia");
    ayt::io::File::writeAllText(f, kScriptV1);

    auto sys = std::make_unique<ScriptSubSystem>();
    CHECK(sys->initialize());
    CHECK(sys->watchScriptPath("Reloadable", f));
    sys->setHotReloadEnabled(true);
    CHECK(sys->isHotReloadEnabled());

    sys->shutdown();
    nukeScratch();
}

TEST_CASE(file_edit_reloads_on_update) {
    makeScratch();
    const std::string f = joinPath(kScratch, "reloadable.logia");
    ayt::io::File::writeAllText(f, kScriptV1);

    auto sys = std::make_unique<ScriptSubSystem>();
    CHECK(sys->initialize());
    std::vector<CompilerError> errors;
    CHECK(sys->bridge().loadScript("Reloadable", kScriptV1, errors));
    CHECK(errors.empty());
    CHECK(sys->watchScriptPath("Reloadable", f));
    sys->setHotReloadEnabled(true);

    float dt = 0.016f;
    CHECK(sys->bridge().callLifecycle("Reloadable", "on_update", nullptr, &dt));
    CHECK(sys->bridge().getLuaGlobalString("__test_witness") == std::string("1"));

    ayt::io::File::writeAllText(f, kScriptV2);

    const bool applied = waitFor([&] {
        sys->update(0.016f);
        return sys->hotReloadApplyCount() >= 1u;
    });
    CHECK(applied);

    dt = 0.016f;
    CHECK(sys->bridge().callLifecycle("Reloadable", "on_update", nullptr, &dt));
    CHECK(sys->bridge().getLuaGlobalString("__test_witness") == std::string("10"));

    sys->shutdown();
    nukeScratch();
}

TEST_CASE(shutdown_stops_watcher_without_crash) {
    makeScratch();
    const std::string f = joinPath(kScratch, "shutdown.logia");
    ayt::io::File::writeAllText(f, kScriptV1);

    {
        auto sys = std::make_unique<ScriptSubSystem>();
        CHECK(sys->initialize());
        CHECK(sys->watchScriptPath("Reloadable", f));
        sys->setHotReloadEnabled(true);
        sys->shutdown();
    }

    {
        auto sys = std::make_unique<ScriptSubSystem>();
        CHECK(sys->initialize());
        CHECK(sys->watchScriptPath("Reloadable", f));
        sys->setHotReloadEnabled(true);
    }

    nukeScratch();
}

TEST_CASE(unwatch_stops_reload_for_path) {
    makeScratch();
    const std::string f = joinPath(kScratch, "unwatch.logia");
    ayt::io::File::writeAllText(f, kScriptV1);

    auto sys = std::make_unique<ScriptSubSystem>();
    CHECK(sys->initialize());
    std::vector<CompilerError> errors;
    CHECK(sys->bridge().loadScript("Reloadable", kScriptV1, errors));
    CHECK(sys->watchScriptPath("Reloadable", f));
    sys->setHotReloadEnabled(true);
    CHECK(sys->unwatchScriptPath(f));

    ayt::io::File::writeAllText(f, kScriptV2);

    for (int i = 0; i < 20; ++i) {
        sys->update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(sys->hotReloadApplyCount() == 0u);

    sys->shutdown();
    nukeScratch();
}

TEST_SUITE_END
