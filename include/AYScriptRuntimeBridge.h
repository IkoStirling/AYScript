#pragma once
// AYScriptRuntimeBridge.h - sol2-backed runtime for compiled Logia (S1)

#include <memory>
#include <string>
#include <vector>

#include "logia/AYCompilerError.h"

namespace ayt::script
{

// LogiaRuntimeBridge
//
// Owns a sol::state, registers a minimal engine API (log.*, input.*),
// compiles Logia sources into Lua chunks, and invokes lifecycle methods
// (on_start / on_update / on_destroy) on demand.
//
// S1 deliberately does NOT inherit ayt::entity::IScriptBridge — the
// signatures do not match (Logia uses snake_case methods; AYEntity's
// stub uses camelCase). S2/S3 will add a thin adapter.
class LogiaRuntimeBridge {
public:
    LogiaRuntimeBridge();
    ~LogiaRuntimeBridge();

    LogiaRuntimeBridge(const LogiaRuntimeBridge&) = delete;
    LogiaRuntimeBridge& operator=(const LogiaRuntimeBridge&) = delete;

    // Build the sol::state and register the engine API. Safe to call once.
    bool initialize();
    void shutdown();
    bool isInitialized() const;

    // Compile and load a Logia source string under a logical script name.
    // The compiled module table is cached by scriptName so subsequent
    // loadScript calls with the same name replace the cache entry.
    //
    // Returns true on success. On failure, `errors` is populated with the
    // Logia compiler errors and `hasScript(name)` returns false.
    bool loadScript(const std::string& scriptName,
                    const std::string& logiaSource,
                    std::vector<logia::CompilerError>& errors);

    // Look up a previously-loaded script.
    bool hasScript(const std::string& scriptName) const;

    // Invoke a lifecycle method on the named script.
    //
    //   methodName must be "on_start" / "on_update" / "on_destroy" —
    //   matching the names emitted by LuaCodegen.
    //
    //   arg1 — pointer to the receiver object (ScriptComponent* in real
    //   usage; any opaque pointer in tests). Passed to Lua as lightuserdata
    //   so the script can stash it for later use.
    //
    //   arg2 — for "on_update", points to a float (deltaTime); for other
    //   methods, ignored. May be nullptr to pass nil/0.
    //
    // Returns false if the script/method is missing or the Lua call raised
    // a Lua error (which is then logged).
    bool callLifecycle(const std::string& scriptName,
                       const std::string& methodName,
                       void* arg1 = nullptr,
                       void* arg2 = nullptr);

    // Internal access for tests.
    void* implHandle();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::script