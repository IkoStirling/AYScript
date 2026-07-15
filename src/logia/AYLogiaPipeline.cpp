// AYLogiaPipeline.cpp - heap-backed compile + Lua codegen pipeline

#include "logia/AYLogiaPipeline.h"

#include <memory>
#include <utility>

namespace ayt::script::logia
{

LogiaToLuaResult compileLogiaToLua(const std::string& source,
                                   LuaCodegenOptions codegenOpts)
{
    return compileLogiaToLua(source, defaultLogiaHostContext(),
                             std::move(codegenOpts));
}

LogiaToLuaResult compileLogiaToLua(const std::string& source,
                                   const LogiaHostContext& ctx,
                                   LuaCodegenOptions codegenOpts)
{
    LogiaToLuaResult result;

    auto compiler = std::make_unique<Compiler>();
    CompileResult compiled = compiler->compile(source, ctx);
    result.diagnostics = std::move(compiled.diagnostics);
    if (!compiled.success || !compiled.program) {
        result.errors = std::move(compiled.errors);
        return result;
    }

    // S3.8b: always forward the host context into the codegen options
    // so LuaCodegen can see `expectSelf` (Tool host emits
    // `function M.run()` without `self`; Component / System keep the
    // S2.5 `function M.on_*(self)` shape). `hostContext` is internal
    // pipeline plumbing — not a public override knob — so the
    // caller's `ctx` argument is authoritative.
    codegenOpts.hostContext = ctx;

    auto codegen = std::make_unique<LuaCodegen>(std::move(codegenOpts));
    LuaCodegenResult generated = codegen->generate(*compiled.program);
    if (!generated.success) {
        result.errors = std::move(generated.errors);
        return result;
    }

    result.lua = std::move(generated.source);
    // S5 ED-03 (2026-07-15): forward the codegen-side per-line
    // Lua → Logia source map alongside the generated Lua source.
    // The runtime bridge reads `result.sourceMap` in `loadScript`
    // and caches it per scriptName so a runtime panic can be
    // translated back to the originating Logia line.
    result.sourceMap = std::move(generated.sourceMap);
    result.success = true;
    return result;
}

} // namespace ayt::script::logia
