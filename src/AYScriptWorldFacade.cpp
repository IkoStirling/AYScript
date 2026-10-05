#include "ScriptWorldFacade.h"
#include <AYEntity/ActorClassAsset.h>
#include <AYEntity/EntityImpl.h>
#include <AYEntity/components/TransformComponent.h>
#include <AYLog.h>
#include <AYReflect.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace ayt::script
{
ScriptWorldFacade::ScriptWorldFacade()
{
    if (instances.empty() &&
        !ayt::entity::registerWorldBeforeShutdownCallback(&instances, &beforeShutdown))
        throw std::runtime_error("could not register Logia World teardown hook");
    instances.push_back(this);
}
ScriptWorldFacade::~ScriptWorldFacade()
{
    std::erase(instances, this);
    if (instances.empty())
        ayt::entity::unregisterWorldBeforeShutdownCallback(&instances);
}
void ScriptWorldFacade::beforeShutdown(ayt::entity::World &ending) noexcept
{
    for (auto *facade : instances)
        if (facade->world == &ending)
            facade->clear();
}
void ScriptWorldFacade::clear()
{
    ++epoch;
    world = nullptr;
    entities.clear();
    commands.clear();
    assetsRoot.clear();
}
void ScriptWorldFacade::bind(ayt::entity::World *target, std::string root)
{
    if (world != target)
        clear();
    world = target;
    if (!root.empty())
        assetsRoot = std::move(root);
}
ayt::entity::Entity *ScriptWorldFacade::resolve(std::int64_t token)
{
    if (!world || !world->isInitialized())
        return nullptr;
    auto it = entities.find(token);
    return it == entities.end() || it->second.pending ? nullptr
                                                      : world->getEntityByHandle(it->second.handle);
}
std::int64_t ScriptWorldFacade::expose(ayt::entity::Entity *entity)
{
    if (!entity || !world)
        return 0;
    auto handle = world->getEntityHandle(entity->getId());
    if (world->getEntityByHandle(handle) != entity) return 0;
    for (const auto &[token, entry] : entities)
        if (!entry.pending && entry.handle == handle)
            return token;
    const auto token = nextToken++;
    entities.emplace(token, Entry{handle, false});
    return token;
}
bool ScriptWorldFacade::enqueue(std::function<void()> command)
{
    if (!world || !world->isInitialized() || commands.size() >= 4096)
        return false;
    commands.push_back(std::move(command));
    return true;
}
std::int64_t ScriptWorldFacade::create(std::string name, std::string classPath)
{
    if (!world || !world->isInitialized())
        return 0;
    const auto token = nextToken++;
    entities[token] = {{}, true};
    if (!enqueue([this, token, name = std::move(name), classPath = std::move(classPath)] {
            ayt::entity::ActorClassAsset asset;
            std::string error;
            if (!classPath.empty() &&
                !ayt::entity::resolveActorClassAsset(assetsRoot, classPath, asset, &error))
            {
                ayt::log::error("Logia spawn_actor: %s", error.c_str());
                entities.erase(token);
                return;
            }
            auto *entity = world->createEntity();
            if (!entity)
            {
                entities.erase(token);
                return;
            }
            entity->setName(name.c_str());
            if (!classPath.empty() &&
                !ayt::entity::instantiateActorClass(*entity, asset, classPath, assetsRoot, &error))
            {
                world->destroyEntity(entity);
                entities.erase(token);
                ayt::log::error("Logia spawn_actor: %s", error.c_str());
                return;
            }
            entities[token] = {world->getEntityHandle(entity->getId()), false};
        }))
    {
        entities.erase(token);
        return 0;
    }
    return token;
}
void ScriptWorldFacade::flush()
{
    auto pending = std::move(commands);
    commands.clear();
    for (auto &command : pending)
        if (world && world->isInitialized())
            command();
}
void ScriptWorldFacade::install(sol::state &lua)
{
    auto table = lua.create_table();
    table["create"] = [this](const std::string &name) { return create(name, {}); };
    table["spawn_actor"] = [this](const std::string &path, const std::string &name) {
        return create(name, path);
    };
    table["find"] = [this](const std::string &name) {
        return world && world->isInitialized() ? expose(world->findEntity(name.c_str())) : 0;
    };
    table["exists"] = [this](std::int64_t token) { return resolve(token) != nullptr; };
    table["pending"] = [this](std::int64_t token) {
        auto it = entities.find(token);
        return it != entities.end() && it->second.pending;
    };
    table["destroy"] = [this](std::int64_t token) {
        if (!entities.contains(token))
            return false;
        return enqueue([this, token] {
            if (auto *entity = resolve(token))
                world->destroyEntity(entity);
            entities.erase(token);
        });
    };
    table["has_component"] = [this](std::int64_t token, const std::string &component) {
        auto *entity = resolve(token);
        return entity && entity->hasComponentByName(component.c_str());
    };
    table["add_component"] = [this](std::int64_t token, const std::string &component) {
        if (!entities.contains(token))
            return false;
        return enqueue([this, token, component] {
            if (auto *entity = resolve(token))
                entity->addComponentByName(component.c_str());
        });
    };
    table["remove_component"] = [this](std::int64_t token, const std::string &component) {
        if (!entities.contains(token))
            return false;
        return enqueue([this, token, component] {
            if (auto *entity = resolve(token))
                entity->removeComponentByName(component.c_str());
        });
    };
    table["set_position"] = [this](std::int64_t token, double x, double y, double z) {
        if (!entities.contains(token) || !std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(z) || std::abs(x) > std::numeric_limits<float>::max() ||
            std::abs(y) > std::numeric_limits<float>::max() ||
            std::abs(z) > std::numeric_limits<float>::max())
            return false;
        return enqueue([this, token, x, y, z] {
            if (auto *entity = resolve(token))
                if (auto *transform = entity->getComponent<ayt::entity::Transform>())
                    transform->setPosition(static_cast<float>(x), static_cast<float>(y),
                                           static_cast<float>(z));
        });
    };
    auto fieldFor = [this](std::int64_t token, const std::string &typeName,
                           const std::string &fieldName, bool write) {
        auto *entity = resolve(token);
        auto *component = entity ? entity->getComponentByName(typeName.c_str()) : nullptr;
        auto *type = ayt::reflect::TypeRegistryImpl::instance().findType(typeName.c_str());
        auto *field = type ? type->findField(fieldName.c_str()) : nullptr;
        if (!component || !field || !ayt::reflect::isScriptReadable(field, type) ||
            (write && !ayt::reflect::isScriptWritable(field)))
            throw std::runtime_error("Logia component field is unavailable or inaccessible");
        return std::pair{field, field->get(component)};
    };
    table["get_number"] = [fieldFor](std::int64_t token, const std::string &type,
                                     const std::string &name) {
        auto [field, data] = fieldFor(token, type, name, false);
        const std::string kind = field->getType()->getName();
        if (kind == "float" || kind == "Float32")
            return static_cast<double>(*static_cast<float *>(data));
        if (kind == "double" || kind == "Float64")
            return *static_cast<double *>(data);
        if (kind == "int" || kind == "Int32")
            return static_cast<double>(*static_cast<int *>(data));
        throw std::runtime_error("Logia get_number requires a numeric field");
    };
    table["set_number"] = [this, fieldFor](std::int64_t token, const std::string &type,
                                           const std::string &name, double value) {
        auto [field, data] = fieldFor(token, type, name, true);
        (void)data;
        if (!std::isfinite(value))
            return false;
        const std::string kind = field->getType()->getName();
        if (kind != "float" && kind != "Float32" && kind != "double" && kind != "Float64" &&
            kind != "int" && kind != "Int32")
            return false;
        if ((kind == "int" || kind == "Int32") &&
            (std::trunc(value) != value || value < std::numeric_limits<int>::min() ||
             value > std::numeric_limits<int>::max()))
            return false;
        if ((kind == "float" || kind == "Float32") &&
            std::abs(value) > std::numeric_limits<float>::max())
            return false;
        return enqueue([fieldFor, token, type, name, value, kind] {
            try
            {
                auto [field, target] = fieldFor(token, type, name, true);
                (void)field;
                if (kind == "float" || kind == "Float32")
                    *static_cast<float *>(target) = static_cast<float>(value);
                else if (kind == "double" || kind == "Float64")
                    *static_cast<double *>(target) = value;
                else
                    *static_cast<int *>(target) = static_cast<int>(value);
            }
            catch (const std::exception &error)
            {
                ayt::log::warn("Logia deferred field write: %s", error.what());
            }
        });
    };
    lua["entity"] = table;
}
} // namespace ayt::script
