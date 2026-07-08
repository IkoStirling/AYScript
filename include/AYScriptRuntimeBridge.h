#pragma once
// AYScriptRuntimeBridge.h - sol2-backed runtime for compiled Logia (S1+)

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
    [[nodiscard]] bool isInitialized() const;

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
    [[nodiscard]] bool hasScript(const std::string& scriptName) const;

    // Invoke a lifecycle method on the named script.
    //
    //   methodName must be "on_start" / "on_update" / "on_destroy" —
    //   matching the names emitted by LuaCodegen.
    //
    //   receiver — pointer to the ScriptComponent instance. Passed to
    //   Lua as lightuserdata so the script's `self` parameter receives
    //   the actual component (was: a placeholder scriptName string in
    //   S1). Pass nullptr in tests that don't care about the receiver.
    //   Typed as void* so AYScript doesn't need to include
    //   AYScriptComponent.h (which has a static-init side effect that
    //   requires World to be fully defined). The adapter casts.
    //
    //   arg2 — for "on_start", points to the owning Entity*; for
    //   "on_update", points to a float (deltaTime); for "on_destroy",
    //   ignored. May be nullptr to pass nil.
    //
    // Returns false if the script/method is missing or the Lua call raised
    // a Lua error (which is then logged).
    bool callLifecycle(const std::string& scriptName,
                       const std::string& methodName,
                       void* receiver = nullptr,
                       void* arg2 = nullptr);

    // === S3.5 — ambient API real-time bindings (was: mock in S1) ===

    // Plumb the per-tick scaled delta (and accumulate elapsed time)
    // before any lifecycle call that should observe them.
    // ScriptSubSystem::update/fixedUpdate must call this so the
    // `time.delta` / `time.total` ambient table reads from the
    // tick's authoritative source rather than a global mock.
    void tickAmbient(float scaledDelta);

    // Current scaled delta in seconds. Reflects the most recent
    // tickAmbient() call. Reads are cheap (plain float load).
    [[nodiscard]] float currentDelta() const noexcept;
    // Accumulated scaled time across all tickAmbient() calls.
    [[nodiscard]] float totalElapsed() const noexcept;

    // Injectable input backend. S3.5 ships a default mock so the
    // unittests can run without an HWND — but production hosts can
    // plug in a real AYDevice / OS-level poll by registering a
    // different provider. Provider ownership: bridge holds a raw
    // pointer; lifetime is the caller's responsibility.
    class InputProvider {
    public:
        virtual ~InputProvider() = default;
        virtual bool isPressed(const std::string& key) const = 0;
        virtual bool isJustPressed(const std::string& key) const = 0;
    };
    void setInputProvider(InputProvider* provider) noexcept;
    [[nodiscard]] InputProvider* inputProvider() const noexcept;

    // Internal access for tests.
    void* implHandle();

    // Test hook: read a top-level Lua string global by name. Returns
    // empty string if the global is missing or not a string. The
    // implementation forwards to the underlying sol::state. Tests
    // use this to verify that a Lua script wrote to a known global
    // (e.g. `__test_witness`).
    std::string getLuaGlobalString(const char* name) const;

    // Test hook: read a top-level Lua number global. Returns false if
    // missing or not numeric.
    bool tryGetLuaGlobalNumber(const char* name, double& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::script