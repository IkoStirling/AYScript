#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include "ScriptProjectEvents.h"
#include <AYScript/logia/CheckedSemantics.h>
#include <cmath>
#include <cctype>
#include <stdexcept>
namespace ayt::script {
namespace {
struct ProjectEvent { const void* scope; std::string alias; std::vector<ScriptHostValue> values; };
struct Dispatch { const sol::protected_function* function;const std::vector<ScriptHostValue>* fields; };
int dispatchLua(lua_State* L) {
    auto* context=static_cast<Dispatch*>(lua_touserdata(L,1));lua_settop(L,0);context->function->push(L);
    for(size_t i=0;i<context->fields->size();++i) {
        const auto& value=(*context->fields)[i];
        switch(value.index()) {
        case 1:lua_pushinteger(L,std::get<std::int64_t>(value));break;
        case 2:lua_pushnumber(L,std::get<double>(value));break;
        case 3:lua_pushboolean(L,std::get<bool>(value));break;
        case 4:lua_pushlstring(L,std::get<std::string>(value).data(),std::get<std::string>(value).size());break;
        default:lua_pushnil(L);break;
        }
    }
    lua_call(L,static_cast<int>(context->fields->size()),0);return 0;
}
ScriptHostValue fromLua(sol::object value) {
    switch(value.get_type()) {
    case sol::type::number: { auto* L=value.lua_state();value.push();const bool integer=lua_isinteger(L,-1)!=0;lua_pop(L,1);
        if(integer)return std::int64_t(value.as<lua_Integer>());return value.as<double>(); }
    case sol::type::boolean:return value.as<bool>();
    case sol::type::string:return value.as<std::string>();
    default:throw std::runtime_error("project event payload must be scalar");
    }
}
}
bool ScriptProjectEvents::registerSchema(const std::string& name,const std::vector<std::string>& types) {
    error.clear();
    auto old=schemas.find(name);
    if(old!=schemas.end()) { if(old->second==types)return true;error="project event schema is fixed for the live runtime: "+name;return false; }
    bool valid=!name.empty() && name.size()<=128 && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0]=='_');
    for(char c:name)valid&=std::isalnum(static_cast<unsigned char>(c))||c=='_'||c=='.';
    if(!valid || types.size()>16 || schemas.size()>=129 || logia::checkedEventPayloads().contains(name)) {
        error="invalid, reserved or excessive project event schema";return false;
    }
    for(const auto& type:types)if(type!="int" && type!="float" && type!="bool" && type!="string") {
        error="project event payload types must be scalar";return false;
    }
    schemas[name]=types;return true;
}
bool ScriptProjectEvents::emit(const std::string& alias,std::vector<ScriptHostValue> values) {
    error.clear();auto schema=schemas.find(alias);
    if(schema==schemas.end() || values.size()!=schema->second.size()) { error="unknown project event or payload count mismatch";return false; }
    size_t bytes=0;
    for(size_t i=0;i<values.size();++i) {
        const auto& type=schema->second[i];auto& value=values[i];bool valid=false;
        if(type=="float" && std::holds_alternative<std::int64_t>(value))value=static_cast<double>(std::get<std::int64_t>(value));
        if(type=="int")valid=std::holds_alternative<std::int64_t>(value);
        if(type=="float")valid=std::holds_alternative<double>(value)&&std::isfinite(std::get<double>(value));
        if(type=="bool")valid=std::holds_alternative<bool>(value);
        if(type=="string" && std::holds_alternative<std::string>(value)) {
            const auto size=std::get<std::string>(value).size();bytes+=size;valid=size<=65536 && bytes<=262144;
        }
        if(!valid) { error="project event payload type or size mismatch";return false; }
    }
    dispatchErrors.emplace_back();
    ayt::event::EventBus::instance().emit(ProjectEvent{this,alias,std::move(values)});
    error=std::move(dispatchErrors.back());dispatchErrors.pop_back();
    if(hooks.executionBudgetExhausted && hooks.executionBudgetExhausted())error="Logia execution instruction budget exceeded";
    return error.empty();
}
void ScriptProjectEvents::install(sol::state& state,LogiaEventHooks eventHooks) {
    lua=&state;hooks=std::move(eventHooks);schemas["logia_task_complete"]={"int","bool"};
    sol::table event=state["event"];sol::protected_function originalEmit=event["emit"],originalSubscribe=event["subscribe"];
    event["emit"]=[this,originalEmit](const std::string& alias,sol::variadic_args args,sol::this_state current) -> bool {
        if(!schemas.contains(alias)) { lua_State* L=current;originalEmit.push(L);sol::protected_function local(L,-1);lua_pop(L,1);auto result=local(alias,sol::as_args(args));
            if(!result.valid()){sol::error failure=result;throw failure;}
            if(hooks.executionBudgetExhausted && hooks.executionBudgetExhausted())throw std::runtime_error("Logia execution instruction budget exceeded");
            return result.get<bool>(); }
        std::vector<ScriptHostValue> values;for(auto value:args)values.push_back(fromLua(value));
        if(!emit(alias,std::move(values)))throw std::runtime_error(error);return true;
    };
    event["subscribe"]=[this,originalSubscribe](const std::string& alias,sol::protected_function fn,sol::this_state current) -> lua_Integer {
        if(!schemas.contains(alias)) { lua_State* L=current;originalSubscribe.push(L);sol::protected_function local(L,-1);lua_pop(L,1);auto result=local(alias,fn);
            if(!result.valid()){sol::error failure=result;throw failure;}return result.get<lua_Integer>(); }
        if(!fn.valid())return 0;
        auto* main=lua->lua_state();fn.push(main);fn=sol::protected_function(main,-1);lua_pop(main,1);
        const auto owner=hooks.currentOwner?hooks.currentOwner():std::string{};
        auto connection=ayt::event::EventBus::instance().subscribe<ProjectEvent>([this,alias,fn,owner](const ProjectEvent& value) {
            if(value.scope!=this || value.alias!=alias || (hooks.isRuntimeThread && !hooks.isRuntimeThread()))return;
            if(hooks.resetExecutionBudget)hooks.resetExecutionBudget();if(hooks.enterOwner)hooks.enterOwner(owner);
            struct Guard { ScriptProjectEvents* events;~Guard(){if(events->hooks.leaveOwner)events->hooks.leaveOwner();} } guard{this};
            auto* L=lua->lua_state();const int top=lua_gettop(L);
            Dispatch dispatch{&fn,&value.values};lua_pushcfunction(L,&dispatchLua);lua_pushlightuserdata(L,&dispatch);
            if(lua_pcall(L,1,0,0)!=LUA_OK) { const char* message=lua_tostring(L,-1);if(!dispatchErrors.empty())dispatchErrors.back()=message?message:"project event handler failed"; }
            lua_settop(L,top);
        });
        if(hooks.onSubscribed)hooks.onSubscribed(owner,connection);return static_cast<lua_Integer>(connection);
    };
    event["error"]=[this]{return error;};
}
}
