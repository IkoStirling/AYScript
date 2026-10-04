#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include "ScriptMath.h"
#include <AYMath/MathTypes.h>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace ayt::script
{
namespace
{
float finite(float value)
{
    if (!std::isfinite(value)) throw std::runtime_error("nonfinite math result");
    return value;
}
float number(sol::table table, const char *field)
{
    const sol::object value = table[field];
    if (!value.is<double>())
        throw std::runtime_error("vector fields must be numeric");
    const auto v = value.as<double>();
    if (!std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max())
        throw std::runtime_error("vector field out of range");
    return static_cast<float>(v);
}
ayt::math::FVector3 vector(sol::table t)
{
    return {number(t, "x"), number(t, "y"), number(t, "z")};
}
ayt::math::FQuaternion quaternion(sol::table t)
{
    return {number(t, "x"), number(t, "y"), number(t, "z"), number(t, "w")};
}
sol::table toTable(sol::state &lua, const ayt::math::FVector3 &v)
{
    auto t = lua.create_table();
    t["x"] = finite(v.x);
    t["y"] = finite(v.y);
    t["z"] = finite(v.z);
    return t;
}
sol::table toTable(sol::state &lua, const ayt::math::FQuaternion &q)
{
    auto t = lua.create_table();
    t["x"] = finite(q.x);
    t["y"] = finite(q.y);
    t["z"] = finite(q.z);
    t["w"] = finite(q.w);
    return t;
}
ayt::math::FQuaternion unit(sol::table t)
{
    auto q = quaternion(t);
    if (finite(q.length()) < 1e-8f)
        throw std::runtime_error("zero quaternion");
    return q.normalize();
}
} // namespace
void registerScriptMathAndArrays(sol::state &lua)
{
    auto v = lua.create_table();
    v["new"] = [&lua](float x, float y, float z) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            throw std::runtime_error("nonfinite vector");
        return toTable(lua, ayt::math::FVector3{x, y, z});
    };
    v["length"] = [](sol::table a) { return finite(vector(a).length()); };
    v["normalized"] = [&lua](sol::table a) {
        auto p = vector(a);
        auto n = finite(p.length());
        return toTable(lua, n < 1e-8f ? ayt::math::FVector3{} : p / n);
    };
    v["add"] = [&lua](sol::table a, sol::table b) { return toTable(lua, vector(a) + vector(b)); };
    v["subtract"] = [&lua](sol::table a, sol::table b) {
        return toTable(lua, vector(a) - vector(b));
    };
    v["scale"] = [&lua](sol::table a, float n) {
        if (!std::isfinite(n))
            throw std::runtime_error("nonfinite scale");
        return toTable(lua, vector(a) * n);
    };
    v["dot"] = [](sol::table a, sol::table b) { return finite(vector(a).dot(vector(b))); };
    v["cross"] = [&lua](sol::table a, sol::table b) {
        return toTable(lua, vector(a).cross(vector(b)));
    };
    lua["vec3"] = v;
    auto q = lua.create_table();
    q["identity"] = [&lua]() { return toTable(lua, ayt::math::FQuaternion{0, 0, 0, 1}); };
    q["axis_angle"] = [&lua](sol::table axis, float radians) {
        auto a = vector(axis);
        auto n = finite(a.length());
        if (n < 1e-8f || !std::isfinite(radians))
            throw std::runtime_error("invalid axis/angle");
        return toTable(lua, ayt::math::FQuaternion::fromAxisAngle(a / n, radians));
    };
    q["normalized"] = [&lua](sol::table a) { return toTable(lua, unit(a)); };
    q["multiply"] = [&lua](sol::table a, sol::table b) {
        return toTable(lua, (unit(a) * unit(b)).normalize());
    };
    q["rotate"] = [&lua](sol::table a, sol::table b) { return toTable(lua, unit(a) * vector(b)); };
    q["slerp"] = [&lua](sol::table a, sol::table b, float t) {
        if (!std::isfinite(t) || t < 0 || t > 1)
            throw std::runtime_error("slerp factor must be in [0,1]");
        return toTable(lua, unit(a).slerp(unit(b), t));
    };
    lua["quat"] = q;
    auto result = lua.safe_script(R"lua(
local storage=setmetatable({}, {__mode='k'})
local function data(a) local d=storage[a]; if not d then error('typed array required',2) end;return d end
local function index(d,i) if math.type(i)~='integer' or i<1 or i>#d then error('array index out of bounds',3) end end
local mt={__metatable='Logia array',__len=function(a)return #data(a) end,
    __index=function(a,i)local d=data(a);index(d,i);return d[i] end,
    __newindex=function(a,i,v)local d=data(a);index(d,i);if v==nil then error('nil array element',2) end;d[i]=v end}
__ay_array_new=function(d) if #d>65536 then error('array size limit') end;local a=setmetatable({},mt);storage[a]=d;return a end
array={length=function(a)return #data(a) end,
    push=function(a,v)local d=data(a);if #d>=65536 then error('array size limit',2) end;if v==nil then error('nil array element',2) end;d[#d+1]=v end,
    pop=function(a)local d=data(a);if #d==0 then error('empty array',2) end;local v=d[#d];d[#d]=nil;return v end}
)lua",
                                  sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error error = result;
        throw error;
    }
}
} // namespace ayt::script
