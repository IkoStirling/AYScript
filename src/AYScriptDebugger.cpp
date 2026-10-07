#include "ScriptDebugger.h"
#include <algorithm>
#include <sstream>
#include <cctype>
namespace ayt::script {
namespace {
LogiaDebugVariable valueAt(lua_State* L, int index, std::string name) {
    LogiaDebugVariable value; value.name=std::move(name);
    value.type=lua_typename(L,lua_type(L,index));
    switch(lua_type(L,index)) {
    case LUA_TNUMBER: { std::ostringstream out; if(lua_isinteger(L,index)) out<<lua_tointeger(L,index); else out<<lua_tonumber(L,index); value.value=out.str(); break; }
    case LUA_TBOOLEAN: value.value=lua_toboolean(L,index)?"true":"false"; break;
    case LUA_TSTRING: { size_t length=0; const char* text=lua_tolstring(L,index,&length); value.value.assign(text,(std::min)(length,size_t{256})); if(length>256)value.value+="..."; break; }
    case LUA_TTABLE: value.value="table ("+std::to_string(lua_rawlen(L,index))+" array entries)"; break;
    default: value.value=value.type;
    }
    return value;
}
std::string fileKey(std::string value) {
    for(char& c:value) { if(c=='\\')c='/';
#ifdef _WIN32
        c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    } return value;
}
int stackDepth(lua_State* L) { lua_Debug frame; int depth=0; while(lua_getstack(L,depth,&frame))++depth; return depth; }
}
void ScriptDebugger::stop() {
    if(thread) { lua_sethook(thread,nullptr,0,0); *static_cast<ScriptDebugger**>(lua_getextraspace(thread))=nullptr; }
    if(main && reference!=LUA_NOREF)luaL_unref(main,LUA_REGISTRYINDEX,reference);
    main=thread=nullptr; reference=LUA_NOREF; status=LogiaDebugStatus::Idle; error.clear();
}
bool ScriptDebugger::start(sol::state& lua, sol::protected_function fn, sol::object self,
                           std::string script, logia::LogiaSourceMap sourceMap) {
    stop(); main=lua.lua_state(); thread=lua_newthread(main); reference=luaL_ref(main,LUA_REGISTRYINDEX);
    *static_cast<ScriptDebugger**>(lua_getextraspace(thread))=this;
    fn.push(thread); self.push(thread); lua_pushnumber(thread,0.0); arguments=2; name=std::move(script); map=std::move(sourceMap);
    previous={}; depth=0; lastLine=lastDepth=0; status=LogiaDebugStatus::Paused;
    return resume(LogiaDebugCommand::StepInto);
}
void ScriptDebugger::hook(lua_State* L, lua_Debug* info) {
    auto* debugger=*static_cast<ScriptDebugger**>(lua_getextraspace(L));
    if(!debugger)return;
    if(info->event==LUA_HOOKCOUNT) {
        if(++debugger->instructions>=1000)luaL_error(L,"Logia debug instruction budget exceeded");
        return;
    }
    // All C++ temporaries are destroyed before the Lua hook yields.
    if(debugger->shouldPause(info))lua_yield(L,0);
}
bool ScriptDebugger::shouldPause(lua_Debug* info) {
    lua_getinfo(thread,"S",info);
    if(!info->source || std::string_view(info->source)!="logia:"+name)return false;
    auto location=map.lookup(info->currentline);
    if(location.line<=0)return false;
    const int currentDepth=stackDepth(thread);
    const bool plumbing=location.line==previous.line && location.file==previous.file
        && info->currentline>lastLine && currentDepth==lastDepth;
    lastLine=info->currentline; lastDepth=currentDepth;
    if(plumbing)return false;
    bool pause=command==LogiaDebugCommand::StepInto ||
        (command==LogiaDebugCommand::StepOver && currentDepth<=depth) ||
        (command==LogiaDebugCommand::StepOut && currentDepth<depth);
    for(const auto& point:breakpoints)if(point.line==location.line && (point.file.empty()||fileKey(point.file)==fileKey(location.file)))pause=true;
    previous=location;
    if(pause) { depth=currentDepth; status=LogiaDebugStatus::Paused; }
    return pause;
}
bool ScriptDebugger::resume(LogiaDebugCommand next) {
    if(!thread || status!=LogiaDebugStatus::Paused)return false;
    command=next; instructions=0;
    lua_sethook(thread,&hook,LUA_MASKLINE|LUA_MASKCOUNT,1000);
    int results=0; const int count=arguments; arguments=0;
    running=true;
    int result=lua_resume(thread,main,count,&results);
    running=false;
    if(result==LUA_YIELD && results==0) { status=LogiaDebugStatus::Paused; return true; }
    if(result==LUA_OK) { status=LogiaDebugStatus::Completed; if(results)lua_pop(thread,results); return true; }
    status=LogiaDebugStatus::Error;
    if(result==LUA_YIELD)error="Debug lifecycle yielded an await; run async helpers through tasks.start";
    else { const char* text=lua_tostring(thread,-1); error=text?text:"Logia debug execution failed"; }
    return false;
}
LogiaDebugSnapshot ScriptDebugger::snapshot() const {
    LogiaDebugSnapshot result; result.status=status; result.script=name; result.error=error;
    if(!thread || status!=LogiaDebugStatus::Paused)return result;
    for(int i=0;i<32;++i) {
        lua_Debug frame; if(!lua_getstack(thread,i,&frame))break;
        lua_getinfo(thread,"nSl",&frame);
        if(!frame.source || std::string_view(frame.source)!="logia:"+name)continue;
        LogiaDebugFrame view; view.location=map.lookup(frame.currentline); view.function=frame.name?frame.name:"lifecycle";
        for(int local=1;local<=128;++local) {
            const char* key=lua_getlocal(thread,&frame,local); if(!key)break;
            if(key[0]!='(' && std::string_view(key)!="__state")view.locals.push_back(valueAt(thread,-1,key));
            lua_pop(thread,1);
        }
        result.frames.push_back(std::move(view));
    }
    return result;
}
std::vector<LogiaDebugVariable> ScriptDebugger::inspectTable(sol::table table) {
    std::vector<LogiaDebugVariable> result;
    if(!table.valid())return result;
    auto* L=table.lua_state(); const int top=lua_gettop(L); table.push(); const int index=lua_gettop(L);
    lua_pushnil(L);
    while(lua_next(L,index) && result.size()<128) {
        if(lua_type(L,-2)==LUA_TSTRING)result.push_back(valueAt(L,-1,lua_tostring(L,-2)));
        lua_pop(L,1);
    }
    lua_settop(L,top);
    std::sort(result.begin(),result.end(),[](const auto&a,const auto&b){return a.name<b.name;});
    return result;
}
}
