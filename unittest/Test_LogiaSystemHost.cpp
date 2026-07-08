// Test_LogiaSystemHost.cpp - System host (LG-04 / S3.1) tests
//
// Verifies:
//   1. `Compiler::compile(source, ctx)` with ctx.kind == System
//      succeeds for a script name whose AYReflect entry exists.
//   2. An unknown script name under the System host emits a soft
//      warning whose hint mentions `AY_SYSTEM` (host-kind-aware hint).
//   3. Runtime: World tick + ScriptSubSystem::update invoke the
//      Logia `on_update` exactly once per tick. The C++ ISystem
//      (MovementSystem fixture) is registered via AY_SYSTEM before
//      the tick; the Logia script is loaded via LogiaRuntimeBridge
//      with a name that matches; the dispatcher loops in the
//      subsystem picks the system up and forwards dt.
//   4. A `script Foo { on_destroy() { ... } }` under System host
//      compiles (parse OK) but emits a soft warning explaining
//      on_destroy is not invoked on System host.

#include "AYScript.h"
#include "logia/AYSemanticAnalyzer.h"
#include "AYScriptRuntimeBridge.h"
#include "AYScriptSubSystem.h"
#include "AYTest.h"

#include <IAYEntity.h>
#include <AYWorld.h>

#include "AYTestMovementSystem.h"

// AYReflect registration for the test fixture. We register the type
// under the explicit name "MovementSystem" (must match the Logia
// script name and ISystem::getName()). Done once per process via a
// static-init guard.
#include <IAYReflect.h>
#include <AYReflect.h>
#include <AYReflectMacros.h>

#include <string>
#include <vector>
#include <memory>

using namespace ayt::script::logia;

namespace {

bool hasWarningWithMessage(const CompileResult& r, const std::string& needle)
{
    for (const auto& d : r.diagnostics) {
        if (d.severity == DiagnosticSeverity::Warning
            && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasHintContaining(const CompileResult& r, const std::string& needle)
{
    for (const auto& d : r.diagnostics) {
        if (d.hint.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// One-shot static-init helper: register the test fixture type in
// AYReflect under the bare name "MovementSystem". The C++ type lives
// in ayt::script::test::MovementSystem; the registry name is the
// un-namespaced short name so it matches the Logia script and the
// ISystem::getName() string.
struct MovementSystemRegistrar {
    MovementSystemRegistrar() {
        using T = ayt::script::test::MovementSystem;
        auto& reg = ayt::reflect::TypeRegistryImpl::instance();
        if (reg.findType("MovementSystem") != nullptr) return;
        auto* info = new ayt::reflect::TypeInfoImpl<T>(
            "MovementSystem",
            ayt::reflect::detail::defaultCreate<T>,
            ayt::reflect::detail::defaultDestroy<T>,
            ayt::reflect::detail::defaultCopy<T>);
        // AY_PROPERTY(float, moveSpeed) is collected via AY_INHERITS-like
        // static-init chain only when the finalize macro is used; we
        // add the field by hand here to keep the registry entry in
        // sync with the macro-decorated header.
        info->addField(new ayt::reflect::FieldInfoImpl(
            "moveSpeed",
            ayt::reflect::TypeRegistryImpl::instance().findType("float"),
            offsetof(T, moveSpeed),
            ayt::reflect::FieldAttribute::None));
        reg.registerTypeInfo("MovementSystem", info);
    }
};
static MovementSystemRegistrar g_movementSystemRegistrar;

} // namespace

TEST_SUITE(LogiaSystemHostTests)

TEST_CASE(lg04_system_ctx_compile_succeeds_for_registered_type) {
    // The fixture header AY_FINALIZE_REGISTRATION_METADATA registers
    // MovementSystem in AYReflect at static init. A System-host
    // compile under that name must succeed without diagnostics.
    const char* src = R"(
script MovementSystem {
    var tick_counter: int = 0
    on_update() {
        tick_counter = tick_counter + 1
    }
}
)";
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::System;
    ctx.hostType = nullptr;  // no subclass check in LG-04
    ctx.expectSelf = true;   // ISystem* lightuserdata

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK(r.success);
    // MovementSystem is now in the registry — no unknown-name warning.
    CHECK_FALSE(hasWarningWithMessage(r, "no matching registered type"));
}

TEST_CASE(lg04_system_ctx_unknown_name_emits_hint_with_ay_system) {
    // An unknown script name under System host must produce a soft
    // warning whose hint mentions `AY_SYSTEM` (host-kind-aware text).
    const char* src = R"(
script NoSuchSystem {
    on_update() {
        x = 1
    }
}
)";
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::System;

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK(r.success);  // soft warning, not hard error
    CHECK(hasWarningWithMessage(r, "NoSuchSystem"));
    CHECK(hasHintContaining(r, "AY_SYSTEM"));
}

TEST_CASE(lg04_system_host_on_destroy_emits_warning) {
    // `on_destroy` is meaningless on an ISystem (no destruction event).
    // SemanticAnalyzer should soft-warn — still parses, still compiles.
    const char* src = R"(
script MovementSystem {
    on_update() { x = 1 }
    on_destroy() { log.info("never reached") }
}
)";
    LogiaHostContext ctx;
    ctx.kind = LogiaHostKind::System;

    Compiler c;
    auto r = c.compile(src, ctx);
    CHECK(r.success);  // soft warning only
    CHECK(hasWarningWithMessage(r, "on_destroy is not invoked on System host scripts"));
}

TEST_CASE(lg04_runtime_on_update_invoked_per_tick) {
    // End-to-end: World + AY_SYSTEM-registered MovementSystem +
    // Logia script loaded under the same name + ScriptSubSystem::update.
    // The Lua on_update must run once per tick.
    using ayt::script::test::MovementSystem;

    auto& world = ayt::entity::World::instance();
    world.shutdown();  // isolate singleton state from any prior test run
    world.initialize();

    // Register the C++ host (use AY_SYSTEM-equivalent explicit call to
    // avoid pulling in a static-init cycle with the test executable).
    world.registerSystem<MovementSystem>(/* priority */ 200);
    ayt::entity::ISystem* sys = world.findSystemByName("MovementSystem");
    CHECK(sys != nullptr);

    // Set up a Logia script under the same name and load it.
    // Use a var for the local state (counter) and a string for the
    // observable side effect (tostring).
    const char* src = R"(
script MovementSystem {
    var n: int = 0
    on_start() {
        n = 0
    }
    on_update() {
        n = n + 1
        __lg04_counter = tostring(n)
    }
}
)";
    auto sub = std::make_unique<ayt::script::ScriptSubSystem>();
    CHECK(sub->initialize());
    auto& bridge = sub->bridge();  // shared with the subsystem

    std::vector<ayt::script::logia::CompilerError> errs;
    bool loaded = bridge.loadScript("MovementSystem", src, errs);
    if (!loaded) {
        for (const auto& e : errs) {
            fprintf(stderr, "[loadScript err] %d:%d %s\n",
                    e.line, e.column, e.message.c_str());
        }
    }
    CHECK(loaded);
    CHECK(errs.empty());

    // Initial counter: not set yet.
    CHECK(bridge.getLuaGlobalString("__lg04_counter").empty());

    // Tick the subsystem manually (the GameLoop run() is heavier than
    // the test needs). Each tick should call callLifecycle once.
    // Direct manual call as a sanity check — pass the registered ISystem*
    // as receiver (System host convention).
    {
        float dt = 1.0f;
        bool ok = bridge.callLifecycle("MovementSystem", "on_update",
                                        static_cast<void*>(sys), &dt);
        CHECK(ok);
    }
    sub->update(0.016f);
    sub->update(0.016f);
    sub->update(0.016f);

    // The Lua on_update ran 3 times after the manual call (which
    // already brought n to 1) → counter == "4". Verify BEFORE
    // shutdown — shutdown marks the Lua VM inactive and globals
    // become unreadable.
    std::string counter = bridge.getLuaGlobalString("__lg04_counter");
    CHECK(counter == "4");
    sub->shutdown();
    world.shutdown();
}

TEST_SUITE_END
