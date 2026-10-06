#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/ScriptTestEvents.h>
#include <AYScript/ScriptRuntimeBridge.h>
#include <AYTest.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <string_view>
#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif

using namespace ayt::script;
namespace {
constexpr const char *source = R"logia(script AsyncModel {
    function work(amount:float, pause:float) {
        await delay(pause)
        await wait_event("script_test_ping") into (value:int)
        await delay(0)
        self.hp += amount + value
    }
    on_start() { tasks.start(work,self.amount,self.pause) }
})logia";
enum class Phase { Start, InitialDelay, WaitEvent, EventReady, FinalDelay };
struct Task {
    size_t owner;
    Phase phase = Phase::Start;
    double amount, pause;
    int value = 0;
    double due = 0;
    uint64_t eligible = 0;
};
uint64_t option(const char *name, uint64_t fallback, uint64_t maximum) {
    const char *text = std::getenv(name);
    if (!text || !*text) return fallback;
    uint64_t value = 0;
    const std::string_view input(text);
    auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    if (error != std::errc{} || end != input.data() + input.size() || value > maximum)
        throw std::runtime_error(std::string("invalid ") + name);
    return value;
}
} // namespace

TEST_SUITE(LogiaAsyncStress)
TEST_CASE(randomized_scheduler_matches_a_native_lifecycle_model)
{
    const auto seconds = option("AY_LOGIA_ASYNC_STRESS_SECONDS", 0, 18000);
    const auto seed = option("AY_LOGIA_ASYNC_STRESS_SEED", 20261007, UINT64_MAX);
    std::mt19937_64 random(seed);
    const auto started = std::chrono::steady_clock::now();
    auto lastReport = started;
    const auto deadline = started + std::chrono::seconds(seconds);
    std::printf("[Logia async stress] seed=%llu seconds=%llu\n",
                static_cast<unsigned long long>(seed), static_cast<unsigned long long>(seconds));
    auto ctx = logia::checkedLogiaHostContext(logia::actorLogiaHostContext());
    ctx.actorProperties = {{"hp", "float"}, {"amount", "float"}, {"pause", "float"}};
    LogiaRuntimeBridge bridge;
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScript("AsyncModel", source, ctx, errors));
    if (!bridge.hasScript("AsyncModel")) return;
    std::array<ayt::entity::ActorScriptComponent, 8> actors;
    std::array<double, 8> expected{};
    std::vector<Task> tasks;
    double now = 0;
    uint64_t tick = 0, rounds = 0, actions = 0, starts = 0, completed = 0;
    const auto fail = [&](const char *message, uint64_t step) {
        std::printf("[Logia async stress failure] %s seed=%llu round=%llu step=%llu actions=%llu\n",
                    message, static_cast<unsigned long long>(seed),
                    static_cast<unsigned long long>(rounds), static_cast<unsigned long long>(step),
                    static_cast<unsigned long long>(actions));
        CHECK(false);
    };
    do {
        ++rounds;
        for (size_t i = 0; i < actors.size(); ++i) {
            bridge.releaseInstance("AsyncModel", &actors[i]);
            actors[i].properties["hp"] = 0.0;
            actors[i].properties["amount"] = 1.0;
            actors[i].properties["pause"] = 0.0;
            expected[i] = 0;
        }
        tasks.clear();
        for (uint64_t step = 0; step < 256; ++step) {
            ++actions;
            const size_t owner = random() % actors.size();
            switch (random() % 10) {
            case 0: case 1: case 2: {
                const double amount = static_cast<double>(1 + random() % 31);
                const double pause = static_cast<double>(random() % 3) / 16.0;
                actors[owner].properties["amount"] = amount;
                actors[owner].properties["pause"] = pause;
                if (!bridge.callActorLifecycle("AsyncModel", "on_start", actors[owner])) {
                    fail("start failed", step); return;
                }
                tasks.push_back({owner, Phase::Start, amount, pause, 0, now, tick + 1});
                ++starts;
                break;
            }
            case 3: case 4: case 5: {
                const float delta = static_cast<float>(random() % 3) / 32.0f;
                now += delta; ++tick;
                bridge.tickTasks(delta);
                for (auto &task : tasks) {
                    if (task.phase == Phase::WaitEvent || task.eligible > tick || task.due > now)
                        continue;
                    switch (task.phase) {
                    case Phase::Start:
                        task.phase = Phase::InitialDelay; task.due = now + task.pause;
                        task.eligible = tick + 1; break;
                    case Phase::InitialDelay: task.phase = Phase::WaitEvent; break;
                    case Phase::EventReady:
                        task.phase = Phase::FinalDelay; task.due = now;
                        task.eligible = tick + 1; break;
                    case Phase::FinalDelay:
                        expected[task.owner] += task.amount + task.value;
                        task.eligible = UINT64_MAX; ++completed; break;
                    default: break;
                    }
                }
                std::erase_if(tasks, [](const Task &task) { return task.eligible == UINT64_MAX; });
                break;
            }
            case 6: {
                const int value = static_cast<int>(random() % 101) - 50;
                ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{value});
                for (auto &task : tasks)
                    if (task.phase == Phase::WaitEvent) {
                        task.phase = Phase::EventReady; task.value = value;
                        task.due = now; task.eligible = tick + 1;
                    }
                break;
            }
            case 7:
                bridge.releaseInstance("AsyncModel", &actors[owner]);
                std::erase_if(tasks, [&](const Task &task) { return task.owner == owner; });
                break;
            case 8:
                if (random() % 2) {
                    if (!bridge.reloadScript("AsyncModel", source, ctx, errors)) {
                        fail("valid reload failed", step); return;
                    }
                    tasks.clear();
                } else if (bridge.reloadScript("AsyncModel", "script AsyncModel { invalid }", ctx, errors)) {
                    fail("invalid reload accepted", step); return;
                }
                break;
            case 9:
                if (random() % 8 != 0) break;
                bridge.shutdown();
                if (!bridge.loadScript("AsyncModel", source, ctx, errors)) {
                    fail("restart failed", step); return;
                }
                tasks.clear(); now = 0; tick = 0;
                break;
            }
            for (size_t i = 0; i < actors.size(); ++i)
                if (std::get<double>(actors[i].properties.at("hp")) != expected[i]) {
                    std::printf("owner=%zu expected=%.0f actual=%.0f\n", i, expected[i],
                                std::get<double>(actors[i].properties.at("hp")));
                    fail("runtime diverged from model", step); return;
                }
        }
        const auto current = std::chrono::steady_clock::now();
        if (current - lastReport >= std::chrono::seconds(30)) {
            std::printf("[Logia async stress] elapsed=%llds rounds=%llu actions=%llu starts=%llu completed=%llu\n",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(current-started).count()),
                        static_cast<unsigned long long>(rounds), static_cast<unsigned long long>(actions),
                        static_cast<unsigned long long>(starts), static_cast<unsigned long long>(completed));
            std::fflush(stdout); lastReport = current;
        }
    } while (seconds ? std::chrono::steady_clock::now() < deadline : rounds < 32);
    for (auto &actor : actors) bridge.releaseInstance("AsyncModel", &actor);
    bridge.shutdown();
    CHECK(starts > 0);
    CHECK(completed > 0);
    std::printf("[Logia async stress complete] rounds=%llu actions=%llu starts=%llu completed=%llu\n",
                static_cast<unsigned long long>(rounds), static_cast<unsigned long long>(actions),
                static_cast<unsigned long long>(starts), static_cast<unsigned long long>(completed));
}
TEST_CASE(runtime_restart_releases_native_heap_allocations)
{
#if defined(_WIN32) && defined(_DEBUG)
    const auto run = [](int count) {
        auto ctx = logia::checkedLogiaHostContext(logia::actorLogiaHostContext());
        ctx.actorProperties = {{"hp", "float"}, {"amount", "float"}, {"pause", "float"}};
        LogiaRuntimeBridge bridge;
        ayt::entity::ActorScriptComponent actor;
        actor.properties = {{"hp", 0.0}, {"amount", 1.0}, {"pause", 0.0}};
        std::vector<logia::CompilerError> errors;
        for (int i = 0; i < count; ++i) {
            if (!bridge.loadScript("AsyncModel", source, ctx, errors) ||
                !bridge.callActorLifecycle("AsyncModel", "on_start", actor)) return false;
            bridge.tickTasks(0); bridge.tickTasks(0);
            ayt::event::EventBus::instance().emit(ayt::event::ScriptTestPingEvent{7});
            bridge.tickTasks(0); bridge.tickTasks(0);
            bridge.releaseInstance("AsyncModel", &actor);
            bridge.shutdown();
        }
        return true;
    };
    CHECK(run(16));
    _CrtMemState before{}, after{};
    _CrtMemCheckpoint(&before);
    CHECK(run(256));
    _CrtMemCheckpoint(&after);
    const auto bytesBefore = before.lSizes[_NORMAL_BLOCK];
    const auto bytesAfter = after.lSizes[_NORMAL_BLOCK];
    std::printf("[Logia native heap] before=%zu after=%zu blocks_before=%zu blocks_after=%zu\n",
                bytesBefore, bytesAfter, before.lCounts[_NORMAL_BLOCK], after.lCounts[_NORMAL_BLOCK]);
    // Small process-global caches may warm up; per-restart allocations must
    // not be retained after the bridge and its borrowed callbacks are gone.
    CHECK(bytesAfter <= bytesBefore + 65536);
#else
    CHECK(true); // Native debug-heap accounting is a Windows Debug gate.
#endif
}
TEST_SUITE_END
