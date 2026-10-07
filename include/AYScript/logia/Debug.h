#pragma once
#include <AYScript/logia/CompilerError.h>
#include <cstdint>
#include <string>
#include <vector>
namespace ayt::script {
enum class LogiaDebugStatus { Idle, Paused, Completed, Error };
enum class LogiaDebugCommand { Continue, StepInto, StepOver, StepOut };
struct LogiaDebugBreakpoint { std::string file; int line = 0; };
struct LogiaDebugVariable { std::string name, type, value; };
struct LogiaDebugFrame { logia::SourceLocation location; std::string function; std::vector<LogiaDebugVariable> locals; };
struct LogiaTaskInspection { std::int64_t id = 0; std::string owner, wait, event; double remainingSeconds = 0; std::int64_t joinedTask = 0; double timeoutSeconds = -1; };
struct LogiaDebugSnapshot {
    LogiaDebugStatus status = LogiaDebugStatus::Idle;
    std::string script, error;
    std::vector<LogiaDebugFrame> frames;
    std::vector<LogiaDebugVariable> state;
    std::vector<LogiaTaskInspection> tasks;
};
}
