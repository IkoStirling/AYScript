#pragma once
// LogiaTestHelpers.h — shared compile→Lua helpers for AYScript unit tests.
//
// Rule: do NOT stack-allocate Compiler + LuaCodegen in the same test
// helper or TEST_CASE frame. Use compileLogiaToLua() (library) or the
// wrappers below. Semantic-only tests may still use a lone `Compiler c`.

#include "logia/AYLogiaPipeline.h"

#include <string>
#include <vector>

namespace logia_test {

inline std::string compileToLua(
    const std::string& source,
    const ayt::script::logia::LogiaHostContext& ctx =
        ayt::script::logia::defaultLogiaHostContext(),
    ayt::script::logia::LuaCodegenOptions opts = {})
{
    auto result = ayt::script::logia::compileLogiaToLua(source, ctx, opts);
    return result.success ? result.lua : std::string{};
}

inline std::string compileToLua(
    const char* source,
    const ayt::script::logia::LogiaHostContext& ctx =
        ayt::script::logia::defaultLogiaHostContext(),
    ayt::script::logia::LuaCodegenOptions opts = {})
{
    return compileToLua(std::string(source), ctx, opts);
}

inline std::string compileToLua(
    const std::string& source,
    std::vector<ayt::script::logia::CompilerError>& errors,
    const ayt::script::logia::LogiaHostContext& ctx =
        ayt::script::logia::defaultLogiaHostContext(),
    ayt::script::logia::LuaCodegenOptions opts = {})
{
    auto result = ayt::script::logia::compileLogiaToLua(source, ctx, opts);
    if (!result.success) {
        errors.insert(errors.end(), result.errors.begin(), result.errors.end());
    }
    return result.success ? result.lua : std::string{};
}

} // namespace logia_test
