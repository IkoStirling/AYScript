// AYScriptBridgeAdapter.cpp

#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include "AYScript/ScriptBridgeAdapter.h"
#include "AYScript/ScriptRuntimeBridge.h"

// AYScriptBridgeAdapter.cpp - pimpl IScriptBridge implementation (S3.4)
//
// The public AYScript/ScriptBridgeAdapter.h forward-declares the class
// and an opaque `Impl`. The concrete `Impl` here derives from
// AYEntity's IScriptBridge (defined in AYEntity/components/AYEntity/components/AYEntity/components/AYEntity/components/ScriptComponent.h) and
// lives entirely in this TU so the AYEntity header doesn't leak
// through AYScript's public include surface.
//
// The adapter performs two responsibilities, both of which already
// existed in S2:
//   1. camelCase → snake_case method translation.
//   2. ScriptComponent* → receiver pointer pass-through (so the
//      Lua `self` and `ayt_reflect_*_field` (S3.3) see the real
//      ScriptComponent instance).

#include <AYEntity/World.h>
#include <AYEntity/components/ScriptComponent.h>

#include <cstring>

namespace ayt::script
{

struct LogiaScriptBridgeAdapter::Impl : public ayt::entity::IScriptBridge {
public:
    explicit Impl(LogiaRuntimeBridge* bridge)
        : _bridge(bridge) {}

    // AYEntity's IScriptBridge contract: `arg1` is the ScriptComponent*,
    // `arg2` is contextual (Entity* on onStart, float* on onUpdate,
    // nullptr on onDestroy). We forward `arg1` as the Lua lightuserdata
    // receiver so `self.<primitiveField>` reads/writes (S3.3) mutate
    // AY_PROPERTY fields on the real component.
    bool call(const char* method, void* arg1, void* arg2) override {
        if (_bridge == nullptr || method == nullptr) return false;
        auto* receiver = static_cast<ayt::entity::ScriptComponent*>(arg1);
        const char* scriptName = receiver ? receiver->getScriptName() : "";
        return _bridge->callLifecycle(scriptName, toLogiaName(method),
                                      /*receiver*/ arg1, arg2);
    }

    bool hasScript(const char* scriptName) const override {
        if (_bridge == nullptr) return false;
        return _bridge->hasScript(scriptName ? scriptName : "");
    }

    LogiaRuntimeBridge* bridge() const { return _bridge; }

private:
    static const char* toLogiaName(const char* m) {
        if (m == nullptr) return nullptr;
        if (std::strcmp(m, "onStart")   == 0) return "on_start";
        if (std::strcmp(m, "onUpdate")  == 0) return "on_update";
        if (std::strcmp(m, "onDestroy") == 0) return "on_destroy";
        return m; // unknown — bridge returns false (script not found)
    }

    LogiaRuntimeBridge* _bridge; // non-owning
};

// === Public surface (defined in the .h forward-decl) ===

LogiaScriptBridgeAdapter::LogiaScriptBridgeAdapter(LogiaRuntimeBridge* bridge)
    : _impl(new Impl(bridge))
{}

LogiaScriptBridgeAdapter::~LogiaScriptBridgeAdapter()
{
    delete _impl;
    _impl = nullptr;
}

bool LogiaScriptBridgeAdapter::call(const char* method, void* arg1, void* arg2)
{
    return _impl ? _impl->call(method, arg1, arg2) : false;
}

bool LogiaScriptBridgeAdapter::hasScript(const char* scriptName) const
{
    return _impl ? _impl->hasScript(scriptName) : false;
}

LogiaRuntimeBridge* LogiaScriptBridgeAdapter::bridgePtr() const
{
    return _impl ? _impl->bridge() : nullptr;
}

ayt::entity::IScriptBridge* LogiaScriptBridgeAdapter::asScriptBridge() const
{
    return _impl;
}

LogiaScriptBridgeAdapter* makeLogiaScriptBridgeAdapter(LogiaRuntimeBridge* bridge)
{
    return new LogiaScriptBridgeAdapter(bridge);
}

} // namespace ayt::script