#pragma once
// CliCompile.h - shared compile entry point used by both the ays-logia
// executable and the unit tests.
//
// Keeping this in a separate TU means the tests don't have to fork a
// child process to drive the CLI; they call compileFromCli() directly
// and inspect the result struct. The ays-logia executable is then a
// thin wrapper that translates argv into a CliCompileRequest and the
// result into stdout / stderr / exit code.

#include "logia/AYLogia.h"
#include "logia/AYLogiaPipeline.h"

#include <string>
#include <vector>

namespace ayt::script::logia
{

// One-shot request struct. Mirrors the CLI flag set; the executable
// fills it from argv and the test suite fills it from C++ literals.
struct CliCompileRequest {
    // Path to the .logia source file. Empty == read from `sourceText`.
    std::string sourcePath;
    // Inline source (only used when sourcePath is empty). Lets tests
    // skip the filesystem entirely.
    std::string sourceText;
    // Host context to compile under. CLI defaults this to
    // `defaultLogiaHostContext()` (Component) and lets `--host` /
    // `--strict-inheritance` flags override.
    LogiaHostContext ctx = defaultLogiaHostContext();
    // Optional output path. Empty == caller decides where to write.
    std::string outputPath;
};

struct CliCompileResult {
    bool success = false;
    // Generated Lua source. Empty on compile failure.
    std::string lua;
    // Full diagnostic stream (errors + warnings), in source order.
    // Errors are a subset; the CLI uses this to print file:line:col
    // formatted messages to stderr.
    std::vector<LogiaDiagnostic> diagnostics;
    // Legacy error projection (parser + semantic hard errors only).
    std::vector<CompilerError> errors;
    // Read-back of the source that was compiled. Used by the CLI to
    // emit diagnostics even when sourceText was inline.
    std::string resolvedSource;
};

// Compile a .logia source according to `req`. Returns a CliCompileResult;
// the caller decides what to do with stdout / stderr / exit code.
//
// Behavior:
//   - If `req.sourcePath` is non-empty, the file is read (failure to
//     open produces a single synthetic FileNotFound diagnostic and
//     `success == false`).
//   - `req.ctx` is forwarded to the Logia pipeline.
//   - On compile failure, `lua` is empty and `success` is false.
//   - Warnings do NOT cause `success == false`; callers wanting
//     warnings-as-errors should inspect `diagnostics` themselves.
CliCompileResult compileFromCli(const CliCompileRequest& req);

} // namespace ayt::script::logia