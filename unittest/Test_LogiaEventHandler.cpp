// Test_LogiaEventHandler.cpp — INT-04b: LogiaHostKind::EventHandler

#include "AYScript/ScriptRuntimeBridge.h"
#include "AYScript/logia/CompilerError.h"
#include "AYScript/logia/Logia.h"
#include "LogiaTestHelpers.h"
#include "AYTest.h"

#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/DeviceEvents.h>

#include <string>
#include <vector>

using ayt::script::LogiaRuntimeBridge;
using ayt::script::logia::CompileResult;
using ayt::script::logia::Compiler;
using ayt::script::logia::CompilerError;
using ayt::script::logia::LogiaHostContext;
using ayt::script::logia::LogiaHostKind;
using ayt::event::DeviceActionEvent;
using ayt::event::EventBus;

namespace {

bool hasWarningWithMessage(const CompileResult& r, const std::string& needle)
{
    for (const auto& d : r.diagnostics) {
        if (d.severity == ayt::script::logia::DiagnosticSeverity::Warning
            && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void resetSingletonBus()
{
    EventBus::instance().unsubscribeAll();
    EventBus::instance().resetCounters();
}

} // namespace

TEST_SUITE(Logia_INT04b_EventHandler)

TEST_CASE(int04b_factory_fields)
{
    LogiaHostContext ctx = ayt::script::logia::eventHandlerLogiaHostContext();
    CHECK(ctx.kind == LogiaHostKind::EventHandler);
    CHECK(ctx.hostType == nullptr);
    CHECK_FALSE(ctx.expectSelf);
    CHECK_FALSE(ctx.strictInheritance);
}

TEST_CASE(int04b_on_update_soft_warns_run_ok)
{
    LogiaHostContext ctx = ayt::script::logia::eventHandlerLogiaHostContext();
    Compiler c;
    auto r = c.compile(R"(
script DamageHandler {
    run() {
        log.info("bind")
    }
    on_update() {
        log.info("never")
    }
}
)",
                       ctx);
    CHECK(r.success);
    CHECK(hasWarningWithMessage(r, "on_update is not invoked on EventHandler host scripts"));
    CHECK_FALSE(hasWarningWithMessage(r, "run() is a Tool / EventHandler lifecycle"));
}

TEST_CASE(int04b_loadEventHandler_subscribes_device_action)
{
    resetSingletonBus();

    LogiaRuntimeBridge bridge;
    std::vector<CompilerError> errors;
    constexpr const char* kSrc = R"(
script ActionHandler {
    function on_action(id: int, pressed: bool) {
        __witness_action_id = id
        __witness_pressed = pressed and 1 or 0
    }
    run() {
        event.subscribe("device_action", on_action)
    }
}
)";
    CHECK(bridge.loadEventHandler("ActionHandler", kSrc, errors));
    CHECK(errors.empty());

    CHECK(EventBus::instance().emitByAlias<DeviceActionEvent>(
        "device_action", DeviceActionEvent{12345, true}));

    double id = -1.0;
    double pressed = -1.0;
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_action_id", id));
    CHECK(bridge.tryGetLuaGlobalNumber("__witness_pressed", pressed));
    CHECK_INT_EQ(static_cast<int>(id), 12345);
    CHECK_INT_EQ(static_cast<int>(pressed), 1);

    resetSingletonBus();
}

TEST_SUITE_END
