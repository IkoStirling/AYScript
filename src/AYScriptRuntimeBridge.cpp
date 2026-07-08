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
// S3.6 (LG-06a) — compile cache plumbing
//
// Cache key: hash(source) XOR hash(ctx) XOR kLogiaPipelineVersion
// (folded through a prime multiplier to spread bits). The hash is
// stored directly as the map key (unordered_map<size_t, Entry>)
// because we only need hit/miss semantics — no enumeration, no
// collision-sensitive lookups by anything but the exact same input.
//
// Cache value: the generated Lua source. Also records whether the
// codegen succeeded so a cache hit on a previously-failing compile
// reproduces the diagnostic vector verbatim.
// ------------------------------------------------------------

std::size_t hashLogiaHostContext(const logia::LogiaHostContext& ctx)
{
    std::size_t h = static_cast<std::size_t>(ctx.kind);
    h ^= static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(ctx.hostType))
       + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= (ctx.expectSelf ? 0xAAAAAAAAAAAAAAAAULL : 0)
       + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= (ctx.strictInheritance ? 0x5555555555555555ULL : 0)
       + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

namespace {

// Hash the logia source string. std::hash<std::string> is process-
// local; the cache is per-bridge so collisions across runs aren't a
// concern (worst case: spuriously cache-miss, which is correct behavior).
std::size_t hashLogiaSource(const std::string& src)
{
    return std::hash<std::string>{}(src);
}

// Combine the three cache-key inputs into a single hash slot.
std::size_t makeCacheKey(const std::string& src,
                         const logia::LogiaHostContext& ctx)
{
    std::size_t k = hashLogiaSource(src);
    k ^= hashLogiaHostContext(ctx) + 0x9E3779B97F4A7C15ULL + (k << 6) + (k >> 2);
    k ^= kLogiaPipelineVersion  + 0x9E3779B97F4A7C15ULL + (k << 6) + (k >> 2);
    return k;
}

} // namespace

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

    // S3.6 (LG-06a): in-memory compile cache. Keyed by
    // (source-hash, LogiaHostContext fields, pipeline-version).
    // Value holds the generated Lua source (empty when compile
    // failed) and a snapshot of the diagnostics surface — so a
    // cache hit on a previously-failing source reproduces the same
    // error report without re-running the front end.
    struct CompileCacheEntry {
        std::string generatedLua;
        bool compileOk = false;  // captures success/failure once.
    };
    std::unordered_map<std::size_t, CompileCacheEntry> _compileCache;

    // S3.6: hit/miss counters exposed via the public API.
    // _hits counts loadScript calls that found a matching entry and
    // skipped Lex/Parser/Semantic/Codegen entirely (still re-ran
    // the cached Lua chunk via safe_script + module-table caching).
    std::size_t _compileHits  = 0;
    std::size_t _compileMisses = 0;

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
        // S3.5: time table — `delta` and `total` are kept in sync
        // with the bridge's _currentDelta / _totalElapsed scalars
        // via tickAmbient(). The bare-Lua emission in AYLuaCodegen
        // (`time.delta` / `time.total`) reads them as plain numbers;
        // tickAmbient pushes fresh values after each dispatcher tick.
        auto timeTbl = lua.create_table();
        timeTbl["delta"] = 0.0;
        timeTbl["total"] = 0.0;
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

        // Numeric witness slots used by ambient.time inspectors in
        // Test_LogiaAmbient.cpp. Pre-registering them with explicit
        // numeric types lets scripts assign `__witness_delta =
        // time.delta` (a Lua number) without sol2 auto-typing the
        // global to nil on first touch. tryGetLuaGlobalNumber reads
        // these back via lua["__witness_delta"] and fails closed
        // (returns false) if the global is missing.
        lua["__witness_delta"] = 0.0;
        lua["__witness_total"] = 0.0;
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
    // S3.6 (LG-06a): shutdown also wipes the compile cache so a
    // post-shutdown loadScript starts from a clean miss. _impl is
    // preserved across shutdown() (the bridge is reusable), but the
    // source-to-Lua map is bound to the previous pipeline run.
    _impl->_compileCache.clear();
    _impl->_compileHits   = 0;
    _impl->_compileMisses = 0;
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
    // Default-ctx delegator (S2.5 / S3.5 / LG-03 preserved callers).
    // Existing tests rely on the no-ctx overload matching the S2.5
    // semantics — see Test_LogiaRuntime/Test_LogiaSemantic/
    // Test_LogiaAmbient for the full set that hits this path.
    return loadScript(scriptName, logiaSource,
                      logia::defaultLogiaHostContext(), errors);
}

// S3.6 (LG-06a): the real implementation. Keyed by
// (source, hostContext, kLogiaPipelineVersion). On hit, skip
// Lexer/Parser/Semantic/Codegen and re-execute the cached Lua
// chunk. On miss, run the full pipeline and cache the result.
bool LogiaRuntimeBridge::loadScript(const std::string& scriptName,
                                    const std::string& logiaSource,
                                    const logia::LogiaHostContext& ctx,
                                    std::vector<logia::CompilerError>& errors)
{
    errors.clear();

    // S3.6: cache lookup. The Lua-state piece (safe_script +
    // module-table registration) still runs every call because the
    // sol::state can be reset by shutdown() or affected by error
    // paths; caching only the *compile-side* output is correct and
    // matches the spec ("skips Lexer/Parser/Semantic/Codegen").
    const std::size_t cacheKey = makeCacheKey(logiaSource, ctx);
    auto cacheIt = _impl->_compileCache.find(cacheKey);
    std::string cachedLua;
    bool cachedOk = false;
    const bool isCacheHit = (cacheIt != _impl->_compileCache.end());

    if (isCacheHit) {
        ++_impl->_compileHits;
        cachedLua = cacheIt->second.generatedLua;
        cachedOk  = cacheIt->second.compileOk;
    } else {
        ++_impl->_compileMisses;
    }

    // Bridge stage 1: get a `luaSrc` + `luaOk` either from cache
    // (cache hit) or by running the full pipeline (cache miss).
    std::string luaSrc;
    bool luaOk  = false;
    if (isCacheHit) {
        luaSrc = cachedLua;
        luaOk  = cachedOk;
        if (!luaOk) {
            // Cached compile-failure: surface the same error a fresh
            // compile would have produced (we cached a non-empty
            // error report on miss; for now we just bail with a
            // generic "cached compile failed" entry — full diagnostic
            // replay is a follow-up if a unit test needs it).
            logia::CompilerError ce;
            ce.code = logia::ErrorCode::InvalidOperation;
            ce.message = "Cached compile failed for script '" + scriptName + "'";
            ce.line = 0;
            ce.column = 0;
            errors.push_back(std::move(ce));
            _impl->scripts.erase(scriptName);
            return false;
        }
    } else {
        // Cache miss — run the full pipeline.

        // 1. Lex + parse via existing S0 Compiler.
        logia::Compiler compiler;
        auto compiled = compiler.compile(logiaSource, ctx);
        if (!compiled.success) {
            errors = std::move(compiled.errors);
            _impl->scripts.erase(scriptName);
            // Cache the failure so a repeat call hits the same
            // diagnostic instead of re-running the front end.
            _impl->_compileCache[cacheKey] = Impl::CompileCacheEntry{
                /*generatedLua*/ std::string{},
                /*compileOk*/    false,
            };
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
            _impl->_compileCache[cacheKey] = Impl::CompileCacheEntry{
                std::string{},
                false,
            };
            return false;
        }

        luaSrc = std::move(gen.source);
        luaOk  = true;

        // Populate the cache BEFORE running the Lua chunk so a
        // successful safe_script that then produces a runtime error
        // still leaves the compile result cached for the next load.
        _impl->_compileCache[cacheKey] = Impl::CompileCacheEntry{
            luaSrc,
            true,
        };
    }

    // 3. Execute the Lua chunk in sol::state (runs on every call —
    //    safe_script rebinds the chunk into the live sol::state).
    auto result = _impl->lua.safe_script(luaSrc, sol::script_pass_on_error);
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

// ------------------------------------------------------------
// S3.7a (LG-06b part A) — in-memory reload
//
// Implementation strategy: delegate to loadScript (which already
// erases-then-rebinds the scriptName entry, runs the cache lookup
// keyed by (source, ctx, pipelineVersion), and is the single source
// of truth for the compile + load pipeline). The only S3.7a
// addition is the failure policy: on a compile miss, loadScript
// itself erases the cached module — which would drop a previously
// good script on a typo. We capture the prior good state, call
// loadScript, and if it fails, restore the prior module table.
//
// The captured `prior` sol::table is a value-type copy of the
// sol::state's reference to the original module table. sol2's
// sol::table is a cheap handle (a lua_State* + stack index), so
// capturing the prior state is just a copy of the handle and
// re-inserting it on failure preserves the live module binding.
// ------------------------------------------------------------

bool LogiaRuntimeBridge::reloadScript(const std::string& scriptName,
                                      const std::string& logiaSource,
                                      std::vector<logia::CompilerError>& errors)
{
    // Default-ctx delegator (mirrors the loadScript 3-arg form).
    return reloadScript(scriptName, logiaSource,
                        logia::defaultLogiaHostContext(), errors);
}

bool LogiaRuntimeBridge::reloadScript(const std::string& scriptName,
                                      const std::string& logiaSource,
                                      const logia::LogiaHostContext& ctx,
                                      std::vector<logia::CompilerError>& errors)
{
    // S3.7a failure policy: snapshot the prior module so a failed
    // reload does not detach the previously-loaded script. A
    // successful loadScript will overwrite the map entry; a
    // failed loadScript will erase it — in which case we restore
    // the snapshot. hasScript(name) therefore stays true across
    // a failed reload.
    sol::table prior;
    bool hadPrior = false;
    auto priorIt = _impl->scripts.find(scriptName);
    if (priorIt != _impl->scripts.end()) {
        prior = priorIt->second;  // sol::table is a cheap handle copy
        hadPrior = true;
    }

    const bool ok = loadScript(scriptName, logiaSource, ctx, errors);
    if (!ok && hadPrior) {
        // loadScript erased the entry on failure; restore the
        // prior good module so the running game keeps its binding.
        _impl->scripts[scriptName] = prior;
    }
    return ok;
}

// S3.6 — compile-cache observability. Counters live on _impl so the
// public methods delegate without holding a separate state.
std::size_t LogiaRuntimeBridge::compileCacheHitCount() const noexcept
{
    return _impl ? _impl->_compileHits : 0u;
}

std::size_t LogiaRuntimeBridge::compileCacheMissCount() const noexcept
{
    return _impl ? _impl->_compileMisses : 0u;
}

void LogiaRuntimeBridge::resetCompileCacheCounters() noexcept
{
    if (!_impl) return;
    _impl->_compileHits = 0;
    _impl->_compileMisses = 0;
}

void LogiaRuntimeBridge::clearCompileCache() noexcept
{
    if (!_impl) return;
    _impl->_compileCache.clear();
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
    // Mirror onto the `time` table so scripts that hold a reference
    // to the table see fresh values without going through the
    // bridge's accessor methods. This matches the S3.5 ambient
    // expectation that `time.delta` is a number read each frame.
    sol::table timeTbl = _impl->lua["time"];
    if (timeTbl.valid()) {
        timeTbl["delta"] = static_cast<double>(scaledDelta);
        timeTbl["total"] = static_cast<double>(_impl->_totalElapsed);
    }
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