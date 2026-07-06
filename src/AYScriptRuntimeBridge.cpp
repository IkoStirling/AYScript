// AYScriptRuntimeBridge.cpp - sol2-backed runtime for compiled Logia

#include "AYScriptRuntimeBridge.h"

#include "logia/AYLogia.h"
#include "logia/AYLuaCodegen.h"

#define SOL_ALL_SAFETIES_ON 1
#define SOL_SAFE_NUMERICS   1
#include <sol/sol.hpp>

#include "AYLog/AYChannel.h"
#include "AYLog/AYLogger.h"

#include <unordered_map>

namespace ayt::script
{

// ------------------------------------------------------------
// Engine API mocks
//
// S1 ships minimal stubs so the codegen output can be exercised
// end-to-end. S3 replaces these with real AYInput/AYTime bindings.
// ------------------------------------------------------------

namespace {

bool mockIsPressed(const std::string& key)
{
    // Minimal mock: always returns true for "jump" so tests can observe
    // conditional branches without wiring real input.
    return key == "jump";
}

bool mockIsJustPressed(const std::string& /*key*/)
{
    return false;
}

} // namespace

// ------------------------------------------------------------
// Impl
// ------------------------------------------------------------

struct LogiaRuntimeBridge::Impl {
    // sol::state is non-copyable and non-movable; pimpl it.
    sol::state lua;

    // scriptName → module table (the table returned by the chunk)
    std::unordered_map<std::string, sol::table> scripts;

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

        // input table → mocks (S3 will replace with real AYInput)
        auto inputTbl = lua.create_table();
        inputTbl["is_pressed"]      = &mockIsPressed;
        inputTbl["is_just_pressed"] = &mockIsJustPressed;
        lua["input"] = inputTbl;
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
                                       void* arg1,
                                       void* arg2)
{
    auto it = _impl->scripts.find(scriptName);
    if (it == _impl->scripts.end()) {
        return false;
    }

    sol::table M = it->second;
    sol::object fnObj = M[methodName];
    if (fnObj.get_type() != sol::type::function &&
        fnObj.get_type() != sol::type::table) {
        return false;
    }

    sol::protected_function fn = M[methodName];
    if (!fn.valid()) {
        return false;
    }

    sol::protected_function_result r;
    if (methodName == "on_update") {
        // on_update(self, dt) — second param from arg2 pointer.
        float dt = arg2 ? *static_cast<float*>(arg2) : 0.0f;
        r = fn.call(scriptName, dt);
    } else if (methodName == "on_start") {
        // on_start(self, entity) — entity as lightuserdata.
        r = fn.call(scriptName, sol::lightuserdata(arg1));
    } else {
        // on_destroy(self) — no extra args.
        r = fn.call(scriptName);
    }

    if (!r.valid()) {
        sol::error err = r;
        ayt::log::error("Logia call '%s.%s' failed: %s",
                        scriptName.c_str(), methodName.c_str(), err.what());
        return false;
    }
    return true;
}

void* LogiaRuntimeBridge::implHandle()
{
    return _impl.get();
}

} // namespace ayt::script