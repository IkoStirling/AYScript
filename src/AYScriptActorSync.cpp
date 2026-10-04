#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include "ScriptActorSync.h"
#include <AYEntity/EntityImpl.h>
#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYEntity/components/TransformComponent.h>
#include <cmath>
#include <limits>
namespace ayt::script
{
void refreshActorSelf(sol::state &lua, sol::table table,
                      const ayt::entity::ActorScriptComponent &actor)
{
    for (const auto &[key, value] : actor.properties)
        std::visit([&](const auto &v) { table[key] = v; }, value);
    if (auto *entity = actor.getEntity())
        if (auto *transform = entity->getComponent<ayt::entity::Transform>())
        {
            auto fields = lua.create_table();
            auto vector = [&](const ayt::math::FVector3 &value) {
                auto t = lua.create_table();
                t["x"] = value.x;
                t["y"] = value.y;
                t["z"] = value.z;
                return t;
            };
            fields["position"] = vector(transform->position);
            fields["scale"] = vector(transform->scale);
            auto rotation = lua.create_table();
            rotation["x"] = transform->rotation.x;
            rotation["y"] = transform->rotation.y;
            rotation["z"] = transform->rotation.z;
            rotation["w"] = transform->rotation.w;
            fields["rotation"] = rotation;
            table["transform"] = fields;
        }
}
void commitActorSelf(sol::table table, ayt::entity::ActorScriptComponent &actor)
{
    for (auto &[key, native] : actor.properties)
    {
        sol::object field = table[key];
        std::visit(
            [&](auto &value) {
                using T = std::decay_t<decltype(value)>;
                if (field.is<T>())
                {
                    auto v = field.as<T>();
                    if constexpr (std::is_same_v<T, double>)
                    {
                        if (std::isfinite(v))
                            value = v;
                    }
                    else
                        value = std::move(v);
                }
            },
            native);
    }
    auto *entity = actor.getEntity();
    if (!entity)
        return;
    auto *transform = entity->getComponent<ayt::entity::Transform>();
    if (!transform)
        return;
    sol::object object = table["transform"];
    if (!object.is<sol::table>())
        return;
    auto fields = object.as<sol::table>();
    auto scalar = [](sol::table t, const char *key, float &out) {
        sol::object v = t[key];
        if (!v.is<double>())
            return false;
        double d = v.as<double>();
        if (!std::isfinite(d) || std::abs(d) > std::numeric_limits<float>::max())
            return false;
        out = static_cast<float>(d);
        return true;
    };
    auto vector = [&](const char *key, ayt::math::FVector3 &out) {
        sol::object value = fields[key];
        if (!value.is<sol::table>())
            return false;
        auto t = value.as<sol::table>();
        return scalar(t, "x", out.x) && scalar(t, "y", out.y) && scalar(t, "z", out.z);
    };
    ayt::math::FVector3 p, s;
    if (vector("position", p) && (p.x != transform->position.x || p.y != transform->position.y ||
                                  p.z != transform->position.z))
        transform->setPosition(p.x, p.y, p.z);
    if (vector("scale", s) &&
        (s.x != transform->scale.x || s.y != transform->scale.y || s.z != transform->scale.z))
        transform->setScale(s.x, s.y, s.z);
    sol::object rotation = fields["rotation"];
    if (rotation.is<sol::table>())
    {
        auto t = rotation.as<sol::table>();
        ayt::math::FQuaternion q;
        if (scalar(t, "x", q.x) && scalar(t, "y", q.y) && scalar(t, "z", q.z) &&
            scalar(t, "w", q.w) && std::isfinite(q.length()) && q.length() > 1e-8f)
        {
            q = q.normalize();
            if (q.x != transform->rotation.x || q.y != transform->rotation.y ||
                q.z != transform->rotation.z || q.w != transform->rotation.w)
                transform->setRotation(q.x, q.y, q.z, q.w);
        }
    }
}
} // namespace ayt::script
