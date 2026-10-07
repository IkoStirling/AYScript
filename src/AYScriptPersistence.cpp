#include "ScriptPersistence.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>
namespace ayt::script {
namespace {
using Json = nlohmann::json;
constexpr std::size_t maxElements = 65536, maxBytes = 1024 * 1024;
std::string canonical(std::string type) {
    if (type == "Int32" || type == "int32_t" || type == "Int64" || type == "int64_t") return "int";
    if (type == "Float32" || type == "Float64" || type == "double") return "float";
    if (type == "Bool" || type == "Boolean") return "bool";
    if (type == "std::string" || type == "String") return "string";
    return type;
}
Json encode(sol::state& lua, sol::object value, std::string type, unsigned depth, std::size_t& count) {
    if (++count > maxElements || depth > 8) throw std::runtime_error("persistent state data limit exceeded");
    type = canonical(type);
    if (type == "int" && value.is<lua_Integer>()) return value.as<lua_Integer>();
    if (type == "float" && value.is<double>() && std::isfinite(value.as<double>())) return value.as<double>();
    if (type == "bool" && value.get_type() == sol::type::boolean) return value.as<bool>();
    if (type == "string" && value.get_type() == sol::type::string) return value.as<std::string>();
    if (type.starts_with("array<") && type.ends_with(">") && value.is<sol::table>()) {
        auto table = value.as<sol::table>();
        sol::protected_function length = lua["array"]["length"];
        auto result = length(table);
        if (!result.valid()) { sol::error e = result; throw e; }
        const auto size = result.get<std::size_t>();
        if (size > maxElements) throw std::runtime_error("persistent array limit exceeded");
        Json data = Json::array();
        for (std::size_t i = 1; i <= size; ++i)
            data.push_back(encode(lua, table.get<sol::object>(i), type.substr(6, type.size()-7), depth+1, count));
        return data;
    }
    if ((type == "vec2" || type == "vec3" || type == "quat") && value.is<sol::table>()) {
        Json data = Json::object();
        auto table = value.as<sol::table>();
        for (const char* axis : {"x", "y", "z", "w"}) {
            if ((type == "vec2" && (axis[0]=='z' || axis[0]=='w')) || (type == "vec3" && axis[0]=='w')) continue;
            data[axis] = encode(lua, table.get<sol::object>(axis), "float", depth+1, count);
        }
        return data;
    }
    throw std::runtime_error("persistent value does not match " + type);
}
sol::object decode(sol::state& lua, const Json& data, std::string type, unsigned depth, std::size_t& count) {
    if (++count > maxElements || depth > 8) throw std::runtime_error("persistent state data limit exceeded");
    type = canonical(type);
    if (type == "int" && data.is_number_integer()) {
        if (data.is_number_unsigned() && data.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX))
            throw std::runtime_error("persistent integer out of range");
        return sol::make_object(lua, data.get<std::int64_t>());
    }
    if (type == "float" && data.is_number() && std::isfinite(data.get<double>())) return sol::make_object(lua, data.get<double>());
    if (type == "bool" && data.is_boolean()) return sol::make_object(lua, data.get<bool>());
    if (type == "string" && data.is_string()) return sol::make_object(lua, data.get<std::string>());
    if (type.starts_with("array<") && type.ends_with(">") &&
        (data.is_array() || (data.is_object() && data.empty()))) {
        auto raw = lua.create_table();
        std::size_t index = 1;
        for (const auto& item : data) raw[index++] = decode(lua, item, type.substr(6,type.size()-7), depth+1, count);
        sol::protected_function make = lua["__ay_array_new"];
        auto result = make(raw);
        if (!result.valid()) { sol::error e = result; throw e; }
        return result.get<sol::object>();
    }
    if ((type == "vec2" || type == "vec3" || type == "quat") && data.is_object()) {
        auto table = lua.create_table();
        for (const char* axis : {"x", "y", "z", "w"}) {
            if ((type == "vec2" && (axis[0]=='z' || axis[0]=='w')) || (type == "vec3" && axis[0]=='w')) continue;
            table[axis] = decode(lua, data.at(axis), "float", depth+1, count);
        }
        return sol::make_object(lua, table);
    }
    throw std::runtime_error("saved value does not match " + type);
}
sol::object plain(sol::state& lua, const Json& data, unsigned depth=0) {
    if (depth > 16) throw std::runtime_error("migration data nesting limit");
    if (data.is_object() || data.is_array()) {
        auto table = lua.create_table();
        if (data.is_object()) for (auto it=data.begin();it!=data.end();++it) table[it.key()] = plain(lua,it.value(),depth+1);
        else { std::size_t i=1; for (const auto& item:data) table[i++] = plain(lua,item,depth+1); }
        return sol::make_object(lua,table);
    }
    if (data.is_boolean()) return sol::make_object(lua,data.get<bool>());
    if (data.is_number_integer()) {
        if(data.is_number_unsigned() && data.get<std::uint64_t>()>static_cast<std::uint64_t>(INT64_MAX))
            throw std::runtime_error("migration integer out of range");
        return sol::make_object(lua,data.get<std::int64_t>());
    }
    if (data.is_number_float() && std::isfinite(data.get<double>())) return sol::make_object(lua,data.get<double>());
    if (data.is_string()) return sol::make_object(lua,data.get<std::string>());
    throw std::runtime_error("migration payload contains unsupported JSON data");
}
Json migratedValue(sol::object value, unsigned depth, std::size_t& count) {
    if (++count > maxElements || depth > 16) throw std::runtime_error("migration data limit exceeded");
    if (value.get_type() == sol::type::boolean) return value.as<bool>();
    if (value.get_type() == sol::type::string) return value.as<std::string>();
    if (value.get_type() == sol::type::number) {
        value.push(); const bool integer=lua_isinteger(value.lua_state(),-1); lua_pop(value.lua_state(),1);
        if (integer) return value.as<lua_Integer>();
        const auto number=value.as<double>(); if (std::isfinite(number)) return number;
    }
    if (value.is<sol::table>()) {
        auto table=value.as<sol::table>(); Json result=Json::object();
        bool array=false; std::size_t size=0;
        for(const auto& entry:table) {
            if(entry.first.get_type()==sol::type::string) {
                if(array) throw std::runtime_error("mixed migration table keys");
                result[entry.first.as<std::string>()]=migratedValue(entry.second,depth+1,count);
            } else if(entry.first.is<lua_Integer>()) {
                if(!result.empty()) throw std::runtime_error("mixed migration table keys");
                array=true; const auto index=entry.first.as<lua_Integer>();
                if(index<1 || index>static_cast<lua_Integer>(maxElements)) throw std::runtime_error("invalid migration array index");
                size=std::max(size,static_cast<std::size_t>(index));
            } else throw std::runtime_error("invalid migration table key");
        }
        if(array) { result=Json::array(); for(std::size_t i=1;i<=size;++i) result.push_back(migratedValue(table.get<sol::object>(i),depth+1,count)); }
        return result;
    }
    throw std::runtime_error("migration returned non-durable data");
}
sol::table bag(sol::table module, sol::object self) {
    sol::protected_function get=module["_get_state"];
    if(!get.valid()) throw std::runtime_error("persistence requires a checked receiver");
    auto result=get(self); if(!result.valid()) { sol::error e=result; throw e; }
    return result.get<sol::table>();
}
}
PersistentResult captureScriptState(sol::state& lua, sol::table module, sol::object self, const std::string& name) {
    PersistentResult result;
    try {
        sol::object schemaObject=module["_persistentSchema"];
        if(!schemaObject.is<sol::table>()) throw std::runtime_error("persistence requires a checked receiver");
        auto state=bag(module,self); Json fields=Json::object(), schema=Json::object(); std::size_t count=0;
        for(const auto& entry:schemaObject.as<sol::table>()) {
            const auto key=entry.first.as<std::string>(), type=entry.second.as<std::string>();
            schema[key]=type; fields[key]=encode(lua,state.get<sol::object>(key),type,0,count);
        }
        result.json=Json{{"format",1},{"script",name},{"version",module.get<std::uint32_t>("_persistenceVersion")},
                         {"schema",schema},{"fields",fields}}.dump();
        if(result.json.size()>maxBytes) throw std::runtime_error("persistent snapshot size limit");
        result.success=true;
    } catch(const std::exception& error) { result.json.clear(); result.error=error.what(); }
    return result;
}
PersistentResult applyScriptState(sol::state& lua, sol::table module, sol::object self, const std::string& name, const std::string& text) {
    PersistentResult result;
    try {
        if(text.size()>maxBytes) throw std::runtime_error("persistent snapshot size limit");
        auto data=Json::parse(text,[](int depth,Json::parse_event_t,const Json&) {
            if(depth>16) throw std::runtime_error("persistent JSON nesting limit exceeded");
            return true;
        });
        if(data.at("format")!=1 || data.at("script")!=name || !data.at("fields").is_object() || !data.at("schema").is_object())
            throw std::runtime_error("invalid persistent snapshot envelope");
        if(!data.at("version").is_number_integer()) throw std::runtime_error("persistent version must be an integer");
        const auto version=data.at("version").get<std::uint64_t>();
        const auto target=module.get<std::uint32_t>("_persistenceVersion");
        if(version==0 || version>target) throw std::runtime_error("unsupported persistent snapshot version");
        sol::table schema=module["_persistentSchema"];
        if(version<target) {
            sol::protected_function migrate=module["migrate"];
            if(!migrate.valid()) throw std::runtime_error("older snapshot requires migrate(from:int,data:table):table");
            auto migrated=migrate(self,static_cast<lua_Integer>(version),plain(lua,data["fields"]));
            if(!migrated.valid()) { sol::error error=migrated; throw error; }
            std::size_t count=0; data["fields"]=migratedValue(migrated.get<sol::object>(),0,count);
            if(!data["fields"].is_object()) throw std::runtime_error("migration must return a field table");
            result.migrated=true;
        }
        auto decoded=lua.create_table(); std::size_t count=0;
        for(auto it=data["fields"].begin();it!=data["fields"].end();++it) {
            sol::object expected=schema[it.key()];
            if(!expected.is<std::string>()) throw std::runtime_error("unknown persistent field: "+it.key());
            const auto type=expected.as<std::string>();
            if(!result.migrated && data["schema"].at(it.key())!=type) throw std::runtime_error("persistent field schema mismatch: "+it.key());
            decoded[it.key()]=decode(lua,it.value(),type,0,count);
        }
        auto prior=bag(module,self); sol::table instances=module["_instanceState"];
        instances[self]=sol::nil;
        sol::table defaults;
        try { defaults=bag(module,self); } catch(...) { instances[self]=prior; throw; }
        instances[self]=prior;
        std::vector<sol::object> keys; for(const auto& entry:prior) keys.push_back(entry.first);
        for(const auto& key:keys) prior[key]=sol::nil;
        for(const auto& entry:defaults) prior[entry.first]=entry.second;
        for(const auto& entry:decoded) prior[entry.first]=entry.second;
        result.success=true;
    } catch(const std::exception& error) { result.error=error.what(); }
    return result;
}
}
