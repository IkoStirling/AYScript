// Test_LogiaCli.cpp - S3.9 CLI tests.
//
// Two layers of coverage:
//   1. Direct: call compileFromCli() with in-memory sources + a small
//      range of CLI flag combinations. No filesystem, no fork — fast
//      and hermetic.
//   2. End-to-end: fork the actual `ays-logia` executable against a
//      temporary .logia file and check exit code / stdout / stderr.
//      This guards against argv parsing, output redirection, and the
//      "real" CliCompileRequest → CliCompileResult wiring in main().
//
// End-to-end tests use Win32 CreateProcess on Windows so the true
// exit code of the ays-logia binary reaches the test (std::system on
// Windows always goes through cmd.exe and reports cmd's exit code,
// not the inner program's).

#include "AYTest.h"

#include "CliCompile.h"
#include "logia/AYCompilerError.h"
#include "logia/AYLogia.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace ayt::script::logia;

namespace
{

// ---- helpers -----------------------------------------------------------

bool hasErrorWithMessage(const CliCompileResult& r, const std::string& needle)
{
    for (const auto& d : r.diagnostics) {
        if (d.severity == DiagnosticSeverity::Error
            && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasWarningWithMessage(const CliCompileResult& r, const std::string& needle)
{
    for (const auto& d : r.diagnostics) {
        if (d.severity == DiagnosticSeverity::Warning
            && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

// Locate the ays-logia binary. CMake's add_test knows the path; the
// binary lives at <build>/AYRuntime/AYScript/cli/ays-logia.exe. The
// unittest binary runs from <build>/AYRuntime/AYScript/unittest/, so
// the relative path is ../cli/ays-logia.exe. Try a few candidates so
// the same test works whether the cwd is the unittest dir or a
// parent.
std::string locateBinary()
{
    const std::vector<std::string> candidates = {
        "../cli/ays-logia.exe",
        "cli/ays-logia.exe",
        "../../cli/ays-logia.exe",
        "ays-logia.exe",
        "Release/ays-logia.exe",
        "Debug/ays-logia.exe",
    };
    for (const auto& c : candidates) {
        std::ifstream f(c, std::ios::binary);
        if (f.good()) return c;
    }
    return {};
}

struct ExecResult {
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

// Run the ays-logia binary with `cmd` (argv-style space-separated
// string) and return (exitCode, stdout, stderr). stdout / stderr are
// captured via Win32 pipes; the inner program's exit code is read
// directly via GetExitCodeProcess.
//
// Why this is non-trivial on Windows:
//   - std::system() always goes through cmd.exe; cmd's exit code is
//     0 when redirection succeeded, so the inner program's code is
//     lost.
//   - STARTF_USESTDHANDLES with file handles doesn't work for CRT-
//     based programs (printf writes to the CRT's cached stdout
//     handle, which was captured at startup; redirecting via
//     hStdOutput doesn't update the CRT's view).
//   - Piping stdout/stderr via CreatePipe + read loop IS the
//     canonical Win32 way; the CRT picks up the new handle because
//     _get_osfhandle() reads the OS handle table when the CRT first
//     opens a fd.
//
// We use CreatePipe for stdout / stderr. Two anonymous pipes are
// created; the child gets the write ends as its stdout / stderr
// (inheritable), and the parent reads from the read ends until EOF.
// Then we wait for the child process and GetExitCodeProcess for the
// real exit code.
ExecResult runCommand(const std::string& cmd)
{
    ExecResult out;
#ifdef _WIN32
    // Build the child command line as-is; no redirection.
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    HANDLE hStdoutRead  = nullptr;
    HANDLE hStdoutWrite = nullptr;
    HANDLE hStderrRead  = nullptr;
    HANDLE hStderrWrite = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0)) {
        out.exitCode = static_cast<int>(GetLastError());
        return out;
    }
    if (!CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0)) {
        CloseHandle(hStdoutRead); CloseHandle(hStdoutWrite);
        out.exitCode = static_cast<int>(GetLastError());
        return out;
    }
    // The read ends must NOT be inherited by the child, or the
    // child's pipe won't ever reach EOF (the child would still hold
    // the read end open).
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hStdoutWrite;
    si.hStdError  = hStderrWrite;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(
        nullptr,                       // app name (use command line)
        cmdBuf.data(),                 // command line
        nullptr, nullptr,              // process / thread attrs
        TRUE,                          // inherit handles
        0,                             // creation flags
        nullptr, nullptr,              // env, cwd
        &si, &pi);
    if (!ok) {
        CloseHandle(hStdoutRead); CloseHandle(hStdoutWrite);
        CloseHandle(hStderrRead); CloseHandle(hStderrWrite);
        out.exitCode = static_cast<int>(GetLastError());
        return out;
    }

    // Parent doesn't need the write ends.
    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);

    // Read stdout / stderr to completion (both will EOF when the
    // child closes them, which happens on process exit).
    auto drainPipe = [](HANDLE h) -> std::string {
        std::string result;
        char buf[4096];
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(h, buf, sizeof(buf), &got, nullptr)) {
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE) return result;
                return result;
            }
            if (got == 0) return result;
            result.append(buf, buf + got);
        }
    };
    out.stdoutText = drainPipe(hStdoutRead);
    out.stderrText = drainPipe(hStderrRead);
    CloseHandle(hStdoutRead);
    CloseHandle(hStderrRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    out.exitCode = static_cast<int>(code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    // POSIX path — std::system + shell redirection is the simplest.
    const std::string outPath = "_cli_stdout.tmp";
    const std::string errPath = "_cli_stderr.tmp";
    std::remove(outPath.c_str());
    std::remove(errPath.c_str());
    std::string full = cmd + " 1> " + outPath + " 2> " + errPath;
    out.exitCode = std::system(full.c_str());
    out.stdoutText = readFile(outPath);
    out.stderrText = readFile(errPath);
    std::remove(outPath.c_str());
    std::remove(errPath.c_str());
#endif
    return out;
}

// Bail-out macro for end-to-end tests when the ays-logia binary
// cannot be located. Returns from the test function so subsequent
// CHECKs aren't evaluated against garbage data.
#define REQUIRE_CLI_BIN(bin)                                                    \
    do {                                                                        \
        if ((bin).empty()) {                                                    \
            fprintf(stderr, "[skip] ays-logia binary not found\n");             \
            return;                                                             \
        }                                                                       \
    } while (0)

} // namespace

TEST_SUITE(LogiaCliTests)

// =====================================================================
// 1. Direct compileFromCli() coverage
// =====================================================================

TEST_CASE(cli_compile_inline_source_component_succeeds) {
    CliCompileRequest req;
    req.sourceText = R"(
script Foo {
    on_start() { log.info("ok") }
}
)";
    auto r = compileFromCli(req);
    CHECK(r.success);
    CHECK(!r.lua.empty());
    CHECK(r.lua.find("function M.on_start(self)") != std::string::npos);
}

TEST_CASE(cli_compile_tool_host_emits_run_without_self) {
    CliCompileRequest req;
    req.sourceText = R"(
script BuildTool {
    run() { log.info("ok") }
}
)";
    req.ctx = toolLogiaHostContext();
    auto r = compileFromCli(req);
    CHECK(r.success);
    CHECK(r.lua.find("function M.run()") != std::string::npos);
    CHECK(r.lua.find("function M.run(self)") == std::string::npos);
}

TEST_CASE(cli_compile_system_host_emits_on_update_with_self) {
    CliCompileRequest req;
    req.sourceText = R"(
script MovementSystem {
    on_update() { x = 1 }
}
)";
    req.ctx.kind = LogiaHostKind::System;
    auto r = compileFromCli(req);
    CHECK(r.success);
    CHECK(r.lua.find("function M.on_update(self)") != std::string::npos);
}

TEST_CASE(cli_compile_syntax_error_returns_failure) {
    CliCompileRequest req;
    req.sourceText = R"(
script Bad {
    on_start() {
        log.info("missing brace"
)";
    auto r = compileFromCli(req);
    CHECK_FALSE(r.success);
    CHECK_FALSE(r.errors.empty());
}

TEST_CASE(cli_compile_unknown_type_returns_failure) {
    CliCompileRequest req;
    req.sourceText = R"(
script Bad {
    var x: NoSuchType
}
)";
    auto r = compileFromCli(req);
    CHECK_FALSE(r.success);
    CHECK(hasErrorWithMessage(r, "unknown type"));
}

TEST_CASE(cli_compile_warnings_do_not_flip_success) {
    CliCompileRequest req;
    req.sourceText = R"(
script HasWarn {
    run() { log.info("ok") }
    on_update() { log.info("warn") }
}
)";
    req.ctx = toolLogiaHostContext();
    auto r = compileFromCli(req);
    // Soft warning only — compile still produces Lua.
    CHECK(r.success);
    CHECK(hasWarningWithMessage(r, "on_update is not invoked on Tool host scripts"));
    CHECK(!r.lua.empty());
}

TEST_CASE(cli_compile_missing_file_synthesizes_diagnostic) {
    CliCompileRequest req;
    req.sourcePath = "definitely_does_not_exist_xyz.logia";
    auto r = compileFromCli(req);
    CHECK_FALSE(r.success);
    CHECK_FALSE(r.diagnostics.empty());
    CHECK(hasErrorWithMessage(r, "could not open file"));
}

// =====================================================================
// 2. End-to-end via the ays-logia executable
// =====================================================================

TEST_CASE(cli_binary_compile_player_controller_succeeds) {
    const std::string bin = locateBinary();
    REQUIRE_CLI_BIN(bin);
    const std::string srcPath = "_cli_player.logia";
    const std::string outPath = "_cli_player.out.lua";
    {
        std::ofstream f(srcPath, std::ios::binary | std::ios::trunc);
        f << R"(
script PlayerController {
    var n: int = 0
    on_start() { n = 0 }
    on_update() { n = n + 1 }
}
)";
    }

    ExecResult r = runCommand(bin + " compile " + srcPath + " -o " + outPath);
    CHECK(r.exitCode == 0);
    CHECK(r.stderrText.find("error:") == std::string::npos);
    const std::string lua = readFile(outPath);
    CHECK(!lua.empty());
    CHECK(lua.find("function M.on_update(self)") != std::string::npos);

    std::remove(srcPath.c_str());
    std::remove(outPath.c_str());
}

TEST_CASE(cli_binary_compile_tool_host_emits_run_no_self) {
    const std::string bin = locateBinary();
    REQUIRE_CLI_BIN(bin);
    const std::string srcPath = "_cli_tool.logia";
    const std::string outPath = "_cli_tool.out.lua";
    {
        std::ofstream f(srcPath, std::ios::binary | std::ios::trunc);
        f << R"(
script BuildTool {
    run() { log.info("ok") }
}
)";
    }

    ExecResult r = runCommand(bin + " compile " + srcPath +
                              " --host tool -o " + outPath);
    CHECK(r.exitCode == 0);
    const std::string lua = readFile(outPath);
    CHECK(lua.find("function M.run()") != std::string::npos);
    CHECK(lua.find("function M.run(self)") == std::string::npos);

    std::remove(srcPath.c_str());
    std::remove(outPath.c_str());
}

TEST_CASE(cli_binary_compile_syntax_error_exits_nonzero) {
    const std::string bin = locateBinary();
    REQUIRE_CLI_BIN(bin);
    const std::string srcPath = "_cli_bad.logia";
    {
        std::ofstream f(srcPath, std::ios::binary | std::ios::trunc);
        f << "script Bad { on_start() { log.info(\"x\"";  // missing braces
    }

    ExecResult r = runCommand(bin + " compile " + srcPath);
    CHECK(r.exitCode != 0);
    // Diagnostic must mention `error:` on stderr for grep-friendliness.
    CHECK(r.stderrText.find("error:") != std::string::npos);
    CHECK(r.stderrText.find(srcPath) != std::string::npos);

    std::remove(srcPath.c_str());
}

TEST_CASE(cli_binary_unknown_flag_exits_with_usage) {
    const std::string bin = locateBinary();
    REQUIRE_CLI_BIN(bin);
    ExecResult r = runCommand(bin + " compile --no-such-flag");
    CHECK(r.exitCode == 2);  // bad usage
    CHECK(r.stderrText.find("Usage") != std::string::npos);
}

TEST_CASE(cli_binary_missing_subcommand_exits_with_usage) {
    const std::string bin = locateBinary();
    REQUIRE_CLI_BIN(bin);
    ExecResult r = runCommand(bin);
    CHECK(r.exitCode == 2);
    CHECK(r.stderrText.find("Usage") != std::string::npos);
}

TEST_SUITE_END