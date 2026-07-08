#pragma once
// AYScriptBridgeAdapter.h - AYEntity IScriptBridge adapter for Logia (S3.4)
//
// Bridges AYEntity's `IScriptBridge::call(method, arg1, arg2)` calling
// convention to AYScript's `LogiaRuntimeBridge::callLifecycle(scriptName,
// methodName, receiver, arg2)`. Responsibilities:
//
//   1. camelCase method names ("onStart" / "onUpdate" / "onDestroy")
//      → snake_case ("on_start" / "on_update" / "on_destroy").
//   2. `arg1` (the ScriptComponent* receiver, per IScriptBridge contract)
//      → the `receiver` slot on callLifecycle, so the Lua `self` arg
//      receives the real ScriptComponent lightuserdata (S1 used the
//      script-name string as a placeholder).
//
// S3.4: the adapter is declared in this header (forwarding
// IScriptBridge and LogiaRuntimeBridge only — no AYEntity or
// AYScriptComponent full include — to keep AYScript's compiled
// surface free of those recompilation triggers). The cpp file
// pulls in <components/AYScriptComponent.h> + <AYWorld.h> and
// instantiates the inheritance there via the concrete bridge
// declaration. Consumers wanting `LogiaScriptBridgeAdapter*` to
// outlive the include of this header (e.g. for setBridge()) must
// already have AYScriptComponent.h visible transitively.
//
// Why this split: the previous S2 layout dropped IScriptBridge
// inheritance entirely; the new S3.4 layout forwards the interface
// shape but defers the inheritance to the .cpp. Tests and consumers
// that include AYScriptComponent.h + AYWorld.h can use the
// adapter directly. The S2 manual "AdapterCall" forwarding struct
// in Test_LogiaAdapter.cpp is now unnecessary for new code paths.

namespace ayt::entity
{
class IScriptBridge;
class ScriptComponent;
}

namespace ayt::script
{

class LogiaRuntimeBridge;

// Forward declaration of the bridge adapter. The cpp declares
// the concrete derived-from-IScriptBridge class behind the same
// name; consumers should never need to redeclare it.
//
// S3.4 keeps the declaration here minimal: constructor + dtor are
// part of the public surface. The dtor must be declared in the
// header so `std::unique_ptr<LogiaScriptBridgeAdapter>` (used in
// ScriptSubSystem) can instantiate. Its definition lives in the
// cpp where the full class is visible.
class LogiaScriptBridgeAdapter {
public:
    explicit LogiaScriptBridgeAdapter(LogiaRuntimeBridge* bridge);
    ~LogiaScriptBridgeAdapter();

    // The adapter is movable so the unique_ptr reset / swap work
    // as expected; we don't need it copyable.
    LogiaScriptBridgeAdapter(const LogiaScriptBridgeAdapter&) = delete;
    LogiaScriptBridgeAdapter& operator=(const LogiaScriptBridgeAdapter&) = delete;

    // IScriptBridge interface — these forward through the cpp's
    // `Impl` table; declared here so consumers who only include
    // this header can still call/hasScript through the adapter.
    bool call(const char* method, void* arg1, void* arg2);
    bool hasScript(const char* scriptName) const;

    // Test/introspection hook — return the underlying bridge
    // pointer so callers can call loadScript/hasScript on the
    // bridge without keeping a separate handle.
    LogiaRuntimeBridge* bridgePtr() const;

    // AYEntity integration — return the IScriptBridge view so
    // ScriptComponent::setBridge(IScriptBridge*) accepts the
    // adapter without exposing the inheritance in this header.
    ayt::entity::IScriptBridge* asScriptBridge() const;

private:
    // Opaque forward-declared impl in the cpp; the pimpl
    // indirection lets us keep AYEntity's IScriptBridge header
    // out of this public surface.
    struct Impl;
    Impl* _impl;
};

LogiaScriptBridgeAdapter* makeLogiaScriptBridgeAdapter(LogiaRuntimeBridge* bridge);

} // namespace ayt::script