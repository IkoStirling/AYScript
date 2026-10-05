#pragma once
#include <sol/sol.hpp>
#include <AYEntity/World.h>
#include <AYEntity/WorldLifecycle.h>
#include <functional>
#include <unordered_map>

namespace ayt::script {
/// Private owner-thread facade. Tokens never expose native addresses and are
/// invalidated by the World teardown hook, including same-address restarts.
class ScriptWorldFacade {
public:
    ScriptWorldFacade();
    ~ScriptWorldFacade();
    void bind(ayt::entity::World* world, std::string assetsRoot);
    void flush();
    void install(sol::state& lua);
    ayt::entity::Entity* resolve(std::int64_t token);
    std::int64_t expose(ayt::entity::Entity* entity);
    ayt::entity::World* boundWorld() const { return world; }
    const std::string& boundAssetsRoot() const { return assetsRoot; }
    std::uint64_t generation() const { return epoch; }
private:
    struct Entry { ayt::entity::EntityHandle handle; bool pending = false; };
    std::int64_t create(std::string name, std::string actorClass);
    bool enqueue(std::function<void()> command);
    void clear();
    static void beforeShutdown(ayt::entity::World& world) noexcept;
    inline static std::vector<ScriptWorldFacade*> instances;
    ayt::entity::World* world = nullptr;
    std::string assetsRoot;
    std::int64_t nextToken = 1;
    std::uint64_t epoch = 1;
    std::unordered_map<std::int64_t, Entry> entities;
    std::vector<std::function<void()>> commands;
};
}
