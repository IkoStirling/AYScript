#pragma once
// AYLogiaPipeline.h - compile + Lua codegen entry (S3.8+)
//
// Prefer compileLogiaToLua() whenever both Compiler and LuaCodegen are
// needed. On MSVC Debug (/GS), stack-allocating Compiler + LuaCodegen
// (and SemanticAnalyzer inside compile()) in the same call frame can
// corrupt stack cookies in deep test or bridge frames. This pipeline
// keeps the heavy pipeline objects on the heap in one .cpp TU.

#include "AYLogia.h"
#include "AYLuaCodegen.h"

#include <string>
#include <vector>

namespace ayt::script::logia
{

struct LogiaToLuaResult {
    bool success = false;
    std::vector<CompilerError> errors;
    std::vector<LogiaDiagnostic> diagnostics;
    std::string lua;
    // S5 ED-03 (2026-07-15): per-line Lua-line → Logia-source-location
    // map produced alongside `lua`. Same lifetime semantics (populated
    // when success == true). The runtime bridge stores this in its
    // per-script cache so `callLifecycle` failures can be translated
    // back to the originating Logia source line.
    LogiaSourceMap sourceMap;
};

// Full front-end + codegen pipeline. Heap-backed; safe to call from
// deep stack frames (unit tests, ScriptSubSystem, bridge reload).
[[nodiscard]] LogiaToLuaResult compileLogiaToLua(
    const std::string& source,
    LuaCodegenOptions codegenOpts = {});

[[nodiscard]] LogiaToLuaResult compileLogiaToLua(
    const std::string& source,
    const LogiaHostContext& ctx,
    LuaCodegenOptions codegenOpts = {});

} // namespace ayt::script::logia
