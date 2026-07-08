// AYScriptRuntimeBridge.cpp - sol2-backed runtime for compiled Logia

// Windows headers (transitively included via AYScriptComponent.h →
// AYCore.h) pollute `min` / `max` macros via <windows.h>. Define
// NOMINMAX before any system header is processed.
#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include "AYScriptRuntimeBridge.h"

#include "logia/AYLogia.h"
#include "logia/AYLuaCodegen.h"

#define SOL_ALL_SAFETIES_ON 1
#define SOL_SAFE_NUMERICS   1
#include <sol/sol.hpp>

#include "AYChannel.h"
#include "AYLogger.h"

// LG-05 / S3.3: AYReflect introspection for `self.field` runtime
// reads/writes. lua_State* comes from sol2 via its bundled
// compat layer, so the lua_pushinteger/pop helpers below are
// visible without an extra `<lua.h>` include.
#include "IAYReflect.h"
#include "AYReflect.h"  // full TypeRegistryImpl definition

#include <unordered_map>
#include <cstring>

namespace ayt::script
{

// ------------------------------------------------------------
// Engine API mocks
//
// S1 shipped minimal stubs so the codegen output can be exercised
// end-to-end. S3.5 replaces these with real-input plumbing:
//
//   - `time.delta` / `time.total` now read from the bridge's
//     `tickAmbient(dt)` accumulator, which ScriptSubSystem drives
//     with the authoritative scaled delta from GameLoop. No more
//     zero-everywhere mock.
//   - `input.is_pressed` / `input.is_just_pressed` now route
//     through an injectable InputProvider*; default is the
//     MockInputProvider below (jump-only). Production hosts can
//     plug in a real AYDevice / OS-level poll by calling
//     setInputProvider().
//
// Keeping a mock as the default keeps AYScript_Test headless-
// runnable on CI without an HWND.
// ------------------------------------------------------------

namespace {

class MockInputProvider final
    : public ayt::script::LogiaRuntimeBridge::InputProvider {
public:
    bool isPressed(const std::string& key) const override {
        // Mimics S1 behavior so legacy unittests remain stable;
        // only the named jump key is treated as held.
        return key == "jump";
    }
    bool isJustPressed(const std::string& /*key*/) const override {
        return false;
    }
};

MockInputProvider g_defaultInputProvider;

} // namespace

// ------------------------------------------------------------
// LG-05 / S3.3 — AYReflect-backed self.field read/write
// ------------------------------------------------------------
// Logia codegen routes `self.field` accesses through these plain
// `lua_CFunction` globals. Lua passes `self` as lightuserdata
// (= ScriptComponent*) plus two string args: the host type name
// (= the `script Name { … }` declaration) and the field name.
// The bridge looks up the IFieldInfo in AYReflect's TypeRegistry
// and reads/writes through the field's stored offset.
//
// Name-keyed instead of typeid-keyed because the codegen only
// knows the host *name* (a string). Lifetime: IFieldInfo* lives
// in the registry which is process-scope; fieldCache() below
// retains pointers indefinitely but they never get freed.
// ------------------------------------------------------------

struct FieldKey {
    std::string typeName;
    std::string fieldName;
    bool operator==(const FieldKey& o) const {
        return typeName == o.typeName && fieldName == o.fieldName;
    }
};
struct FieldKeyHash {
    size_t operator()(const FieldKey& k) const {
        return std::hash<std::string>{}(k.typeName) ^
               (std::hash<std::string>{}(k.fieldName) << 1);
    }
};
std::unordered_map<FieldKey, const ayt::reflect::IFieldInfo*, FieldKeyHash>& fieldCache()
{
    static std::unordered_map<FieldKey, const ayt::reflect::IFieldInfo*, FieldKeyHash> c;
    return c;
}

const ayt::reflect::IFieldInfo* lookupField(const std::string& typeName,
                                            const std::string& fieldName)
{
    FieldKey k{typeName, fieldName};
    auto& cache = fieldCache();
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    auto* typeInfo = ayt::reflect::TypeRegistryImpl::instance().findType(typeName.c_str());
    if (!typeInfo) return nullptr;
    auto* field = typeInfo->findField(fieldName.c_str());
    if (!field) return nullptr;
    cache[k] = field;
    return field;
}

// Push a primitive (int/float/bool/double/int64) onto the Lua
// stack from a `void*` field pointer. Caller is responsible for
// the type tag — returns 1 on success, 0 on miss. Used only by
// `ayt_reflect_get_field_c` below.
int pushFieldPrimitive(lua_State* L, const ayt::reflect::IFieldInfo* field, void* fieldPtr)
{
    auto* type = field->getType();
    if (!type) return 0;
    const char* tname = type->getName();
    if (!tname) return 0;
    auto eq = [tname](const char* n) { return std::strcmp(tname, n) == 0; };
    if (eq("int") || eq("Int32")) {
        lua_pushinteger(L, static_cast<lua_Integer>(*static_cast<int32_t*>(fieldPtr)));
        return 1;
    }
    if (eq("float") || eq("Float32")) {
        lua_pushnumber(L, static_cast<lua_Number>(*static_cast<float*>(fieldPtr)));
        return 1;
    }
    if (eq("bool") || eq("Bool")) {
        lua_pushboolean(L, *static_cast<bool*>(fieldPtr));
        return 1;
    }
    if (eq("double") || eq("Float64")) {
        lua_pushnumber(L, static_cast<lua_Number>(*static_cast<double*>(fieldPtr)));
        return 1;
    }
    if (eq("Int64")) {
        lua_pushinteger(L, static_cast<lua_Integer>(*static_cast<int64_t*>(fieldPtr)));
        return 1;
    }
    return 0;
}

int ayt_reflect_get_field_c(lua_State* L)
{
    if (!lua_islightuserdata(L, 1) || !lua_isstring(L, 2) || !lua_isstring(L, 3)) {
        lua_pushnil(L);
        return 1;
    }
    void* selfPtr = lua_touserdata(L, 1);
    std::string typeName = lua_tostring(L, 2);
    std::string fieldName = lua_tostring(L, 3);

    auto* field = lookupField(typeName, fieldName);
    if (!field || selfPtr == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    void* fieldPtr = field->get(selfPtr);
    if (pushFieldPrimitive(L, field, fieldPtr) == 0) {
        lua_pushnil(L);
    }
    return 1;
}

int ayt_reflect_set_field_c(lua_State* L)
{
    if (!lua_islightuserdata(L, 1) || !lua_isstring(L, 2) || !lua_isstring(L, 3)) {
        return 0;
    }
    void* selfPtr = lua_touserdata(L, 1);
    std::string typeName = lua_tostring(L, 2);
    std::string fieldName = lua_tostring(L, 3);

    auto* field = lookupField(typeName, fieldName);
    if (!field || selfPtr == nullptr) {
        ayt::log::error("ayt_reflect_set_field: %s.%s (no field or null self)",
                        typeName.c_str(), fieldName.c_str());
        return 0;
    }
    void* fieldPtr = field->get(selfPtr);
    auto* type = field->getType();
    if (!type) return 0;
    const char* tname = type->getName();
    if (!tname) return 0;
    auto eq = [tname](const char* n) { return std::strcmp(tname, n) == 0; };

    if (eq("int") || eq("Int32")) {
        *static_cast<int32_t*>(fieldPtr) = static_cast<int32_t>(lua_tointeger(L, 4));
        return 0;
    }
    if (eq("float") || eq("Float32")) {
        *static_cast<float*>(fieldPtr) = static_cast<float>(lua_tonumber(L, 4));
        return 0;
    }
    if (eq("bool") || eq("Bool")) {
        *static_cast<bool*>(fieldPtr) = (lua_toboolean(L, 4) != 0);
        return 0;
    }
    if (eq("double") || eq("Float64")) {
        *static_cast<double*>(fieldPtr) = static_cast<double>(lua_tonumber(L, 4));
        return 0;
    }
    if (eq("Int64")) {
        *static_cast<int64_t*>(fieldPtr) = static_cast<int64_t>(lua_tointeger(L, 4));
        return 0;
    }
    ayt::log::error("ayt_reflect_set_field: %s.%s unsupported type %s",
                    typeName.c_str(), fieldName.c_str(), tname);
    return 0;
}

// ------------------------------------------------------------
// Impl
// ------------------------------------------------------------

struct LogiaRuntimeBridge::Impl {
    // sol::state is non-copyable and non-movable; pimpl it.
    sol::state lua;

    // scriptName → module table (the table returned by the chunk)
    std::unordered_map<std::string, sol::table> scripts;

    // S3.5: ambient time/input state, owned by the bridge.
    // Populated by tickAmbient() before any lifecycle dispatch;
    // read by `time.delta` / `time.total` Lua accessors. The
    // ScriptSubSystem is the only legitimate writer — tests can
    // poke the values directly through the public setters.
    float _currentDelta  = 0.0f;
    float _totalElapsed  = 0.0f;

    // S3.5: injectable input backend. Default points at the
    // file-local MockInputProvider defined in the anon namespace
    // above. setInputProvider() swaps it out; lifetime is the
    // caller's (test or production host).
    LogiaRuntimeBridge::InputProvider* _input = &g_defaultInputProvider;

    bool initialized = false;

    Impl()
    {
        // Open only the safe subset of Lua standard libraries.
        // Deliberately excluded: package, io, os, debug, coroutine, ffi.
        lua.open_libraries(
            sol::lib::base,
            sol::lib::string,
            sol::lib::table,
            sol::lib::math,
            sol::lib::utf8);

        registerEngineApi();
        initialized = true;
    }

    void registerEngineApi()
    {
        // log table → wraps AYLog
        auto logTbl = lua.create_table();
        logTbl["info"]  = [](const std::string& s) {
            ayt::log::info("%s", s.c_str());
        };
        logTbl["warn"]  = [](const std::string& s) {
            ayt::log::warn("%s", s.c_str());
        };
        logTbl["error"] = [](const std::string& s) {
            ayt::log::error("%s", s.c_str());
        };
        lua["log"] = logTbl;

        // S3.5: time table — `delta` / `total` are functions so Lua
        // can call `time.delta()` (codegen lowers Logia `time.delta`).
        // Lambdas read live Impl scalars updated by tickAmbient().
        auto timeTbl = lua.create_table();
        timeTbl["delta"] = [this]() -> double {
            return static_cast<double>(_currentDelta);
        };
        timeTbl["total"] = [this]() -> double {
            return static_cast<double>(_totalElapsed);
        };
        lua["time"] = timeTbl;

        // S3.5: input table defers to the injectable provider.
        // Lambdas capture `this` so swapping providers takes
        // effect immediately for subsequent lifecycle calls (no
        // need to rebuild the table). Tests assert this in
        // Test_LogiaAmbient.cpp.
        auto inputTbl = lua.create_table();
        inputTbl["is_pressed"] = [this](const std::string& k) -> bool {
            auto* p = _input ? _input : &g_defaultInputProvider;
            return p->isPressed(k);
        };
        inputTbl["is_just_pressed"] = [this](const std::string& k) -> bool {
            auto* p = _input ? _input : &g_defaultInputProvider;
            return p->isJustPressed(k);
        };
        lua["input"] = inputTbl;

        // LG-05 / S3.3: AYReflect-backed self.field read/write.
        // Registered as plain lua_CFunction entries (raw
        // lua_register) so generated Logia can call them as
        // `ayt_reflect_get_field(self, "Type", "field")`. The first
        // arg is the receiver lightuserdata (= the
        // ScriptComponent* pass-through from callLifecycle), the
        // next two args are string type/field names resolved
        // against AYReflect's TypeRegistry at runtime.
        lua_register(lua, "ayt_reflect_get_field", &ayt_reflect_get_field_c);
        lua_register(lua, "ayt_reflect_set_field", &ayt_reflect_set_field_c);

        // Test-only witness: scripts can assign a string here to make
        // observable side effects for unit tests. Production code
        // never touches this; it exists solely so the LogiaAdapter
        // tests can verify that a lifecycle method actually ran and
        // that the receiver/self mapping is correct without parsing
        // AYLog output.
        lua["__test_witness"] = std::string{};
    }
};

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------

LogiaRuntimeBridge::LogiaRuntimeBridge()
    : _impl(std::make_unique<Impl>()) {}

LogiaRuntimeBridge::~LogiaRuntimeBridge() = default;

bool LogiaRuntimeBridge::initialize()
{
    if (_impl->initialized) return true;
    _impl->initialized = true;
    return true;
}

void LogiaRuntimeBridge::shutdown()
{
    _impl->scripts.clear();
    _impl->lua = sol::state{};  // reset state (releases everything)
    _impl->lua.open_libraries(
        sol::lib::base, sol::lib::string, sol::lib::table,
        sol::lib::math, sol::lib::utf8);
    _impl->registerEngineApi();
    _impl->initialized = true;
}

bool LogiaRuntimeBridge::isInitialized() const
{
    return _impl->initialized;
}

bool LogiaRuntimeBridge::loadScript(const std::string& scriptName,
                                    const std::string& logiaSource,
                                    std::vector<logia::CompilerError>& errors)
{
    errors.clear();

    // 1. Lex + parse via existing S0 Compiler.
    logia::Compiler compiler;
    auto compiled = compiler.compile(logiaSource);
    if (!compiled.success) {
        errors = std::move(compiled.errors);
        _impl->scripts.erase(scriptName);
        return false;
    }

    // 2. Generate Lua source.
    logia::LuaCodegenOptions opts;
    opts.scriptName = scriptName;
    logia::LuaCodegen codegen(opts);
    auto gen = codegen.generate(*compiled.program);
    if (!gen.success) {
        errors = std::move(gen.errors);
        _impl->scripts.erase(scriptName);
        return false;
    }

    // 3. Execute the Lua chunk in sol::state.
    auto result = _impl->lua.safe_script(gen.source,
                                         sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        logia::CompilerError ce;
        ce.code = logia::ErrorCode::InvalidOperation;
        ce.message = std::string("Lua load failed: ") + err.what();
        ce.line = 0;
        ce.column = 0;
        errors.push_back(std::move(ce));
        _impl->scripts.erase(scriptName);
        return false;
    }

    // 4. The chunk returns the module table; cache it.
    sol::object returned = result;
    if (returned.get_type() != sol::type::table) {
        logia::CompilerError ce;
        ce.code = logia::ErrorCode::InvalidOperation;
        ce.message = "Logia chunk did not return a table";
        ce.line = 0;
        ce.column = 0;
        errors.push_back(std::move(ce));
        _impl->scripts.erase(scriptName);
        return false;
    }
    _impl->scripts[scriptName] = returned;
    return true;
}

bool LogiaRuntimeBridge::hasScript(const std::string& scriptName) const
{
    return _impl->scripts.find(scriptName) != _impl->scripts.end();
}

bool LogiaRuntimeBridge::callLifecycle(const std::string& scriptName,
                                       const std::string& methodName,
                                       void* receiver,
                                       void* arg2)
{
    auto it = _impl->scripts.find(scriptName);
    if (it == _impl->scripts.end()) {
        return false;
    }

    sol::table M = it->second;
    sol::protected_function fn = M[methodName];
    if (!fn.valid()) {
        return false;
    }

    // `receiver` is the ScriptComponent*. S2 changed the Lua `self`
    // argument from the scriptName string (S1 placeholder) to the
    // actual receiver pointer — sol2 5.5+ pushes a raw typed pointer
    // through lua_pushlightuserdata without an explicit wrapper.
    sol::protected_function_result r;
    if (methodName == "on_update") {
        // on_update(self, dt) — second param from arg2 pointer.
        float dt = arg2 ? *static_cast<float*>(arg2) : 0.0f;
        r = fn.call(receiver, dt);
    } else if (methodName == "on_start") {
        // on_start(self, entity) — entity passed as lightuserdata.
        r = fn.call(receiver, arg2);
    } else {
        // on_destroy(self) — no extra args.
        r = fn.call(receiver);
    }

    if (!r.valid()) {
        sol::error err = r;
        ayt::log::error("Logia call '%s.%s' failed: %s",
                        scriptName.c_str(), methodName.c_str(), err.what());
        return false;
    }
    return true;
}

// ------------------------------------------------------------
// S3.5 — ambient API: tick -> time.delta/time.total
// ------------------------------------------------------------

void LogiaRuntimeBridge::tickAmbient(float scaledDelta)
{
    if (!_impl) return;
    if (scaledDelta < 0.0f) scaledDelta = 0.0f;
    _impl->_currentDelta = scaledDelta;
    _impl->_totalElapsed += scaledDelta;
}

float LogiaRuntimeBridge::currentDelta() const noexcept
{
    return _impl ? _impl->_currentDelta : 0.0f;
}

float LogiaRuntimeBridge::totalElapsed() const noexcept
{
    return _impl ? _impl->_totalElapsed : 0.0f;
}

void LogiaRuntimeBridge::setInputProvider(InputProvider* provider) noexcept
{
    if (!_impl) return;
    _impl->_input = provider ? provider : &g_defaultInputProvider;
}

LogiaRuntimeBridge::InputProvider* LogiaRuntimeBridge::inputProvider() const noexcept
{
    return _impl ? _impl->_input : &g_defaultInputProvider;
}

void* LogiaRuntimeBridge::implHandle()
{
    return _impl.get();
}

std::string LogiaRuntimeBridge::getLuaGlobalString(const char* name) const
{
    if (_impl == nullptr || name == nullptr) return {};
    sol::object obj = _impl->lua[name];
    if (obj.is<std::string>()) {
        return obj.as<std::string>();
    }
    return {};
}

bool LogiaRuntimeBridge::tryGetLuaGlobalNumber(const char* name, double& out) const
{
    if (_impl == nullptr || name == nullptr) return false;
    sol::object obj = _impl->lua[name];
    if (!obj.valid()) return false;
    if (obj.get_type() == sol::type::number) {
        out = obj.as<double>();
        return true;
    }
    return false;
}

} // namespace ayt::script