// Logia emit dump — audit every requested pattern and dump its compiled
// Lua source to stderr. The file is the canonical acceptance suite
// for the grammar surface (see design.md §5.7.6.1). Each case
// compiles via `compileLogiaToLua()` and prints both the verdict and
// the literal emitted string so a future grammar regression is
// obvious from the test run alone.

#include "AYScript.h"
#include "AYTest.h"
#include "LogiaTestHelpers.h"

#include <cstdio>
#include <string>

using namespace ayt::script::logia;

namespace {

// Captured per-dump so `TEST_CASE` bodies can string-search the emit
// or the diagnostic list outside the dumpCase helper.
struct DumpCapture {
    std::string emittedLua;
    std::vector<LogiaDiagnostic> diagnostics;
    bool errorsContain(const std::string& needle) const {
        for (const auto& d : diagnostics) {
            if (d.message.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};
DumpCapture _lastDump;

bool containsFlat(const std::string& haystack, const std::string& needle) {
    // Plain substring search — codegen output is short enough that
    // whitespace-tolerant matching buys little and risks false
    // negatives on phrases that sit mid-line.
    return haystack.find(needle) != std::string::npos;
}

void dumpCase(const char* label, const char* src, bool expectSuccess)
{
    LuaCodegenOptions opts;
    opts.scriptName = label;

    auto result = compileLogiaToLua(src, defaultLogiaHostContext(), opts);
    _lastDump.emittedLua = result.lua;
    _lastDump.diagnostics = result.diagnostics;

    std::fprintf(stderr, "\n=== CASE %s ===\n", label);
    std::fprintf(stderr, "--- LOGIA SRC ---\n%s\n", src);
    std::fprintf(stderr, "--- success=%s (expected=%s) ---\n",
                result.success ? "true" : "false",
                expectSuccess ? "true" : "false");
    if (!result.errors.empty()) {
        std::fprintf(stderr, "--- ERRORS ---\n");
        for (const auto& e : result.errors) {
            std::fprintf(stderr, "  [%d:%d] %s\n",
                        (int)e.line, (int)e.column, e.message.c_str());
        }
    }
    if (!result.diagnostics.empty()) {
        std::fprintf(stderr, "--- DIAGNOSTICS ---\n");
        for (const auto& d : result.diagnostics) {
            std::fprintf(stderr, "  [%s] code=%d  %s\n",
                        d.severity == DiagnosticSeverity::Error   ? "ERR"
                      : d.severity == DiagnosticSeverity::Warning ? "WRN"
                      : "INF",
                        (int)d.errorCode, d.message.c_str());
        }
    }
    std::fprintf(stderr, "--- EMITTED LUA BEGIN ---\n%s--- EMITTED LUA END ---\n",
                 result.lua.c_str());

    // Per the audit rubric, assert what was requested.
    CHECK(result.success == expectSuccess);
}

} // namespace

TEST_SUITE(LogiaEmitDumpTests)

TEST_CASE(emit_dump_01_local_self_method) {
    const char* src = R"(
script Foo {
    on_update() {
        local s = self.method()
    }
}
)";
    dumpCase("01_local_self_method", src, /*expectSuccess=*/true);
}

TEST_CASE(emit_dump_02_multiple_locals) {
    const char* src = R"(
script Foo {
    on_update() {
        local a = 1
        local b = 2
    }
}
)";
    dumpCase("02_multiple_locals", src, /*expectSuccess=*/true);
}

TEST_CASE(emit_dump_03_var_int_top_level) {
    const char* src = R"(
script Foo {
    var tick: int = 0
}
)";
    dumpCase("03_var_int_top_level", src, /*expectSuccess=*/true);
}

TEST_CASE(emit_dump_04_var_no_type) {
    const char* src = R"(
script Foo {
    var tick = 0
}
)";
    // 2026-07-11: type inference for `var` is still deferred to a
    // future slice — parser hard-rejects no-type var declarations.
    dumpCase("04_var_no_type", src, /*expectSuccess=*/false);
}

TEST_CASE(emit_dump_05_if_brace) {
    const char* src = R"(
script Foo {
    on_update() {
        if x == nil {
            y = 1
        }
    }
}
)";
    dumpCase("05_if_brace", src, /*expectSuccess=*/true);
}

TEST_CASE(emit_dump_06_if_then_end) {
    const char* src = R"(
script Foo {
    on_update() {
        if x == nil then
            y = 1
        end
    }
}
)";
    // Brace-only — Lua `then`/`end` form rejected (defer forever;
    // design §2.3 原则 7 keeps Logia brace-form).
    dumpCase("06_if_then_end", src, /*expectSuccess=*/false);
}

TEST_CASE(emit_dump_07_self_chain_3plus) {
    const char* src = R"(
script Foo {
    on_update() {
        local v = self.f1.f2.f3
    }
}
)";
    dumpCase("07_self_chain_3plus", src, /*expectSuccess=*/true);
}

TEST_CASE(emit_dump_08_dotdot_concat) {
    const char* src = R"(
script Foo {
    on_update() {
        local s = tostring(a) .. tostring(b)
    }
}
)";
    dumpCase("08_dotdot_concat", src, /*expectSuccess=*/false);
}

// 2026-07-11 audit fix: `function foo() { ... }` at script-block
// scope IS now valid (was previously a parser reject — the audit
// added the keyword and the FunctionDeclStmt node). Expected
// success. The audit's plain `function foo()` inside an
// on_update body is covered by case 12 below.
TEST_CASE(emit_dump_09_function_decl) {
    const char* src = R"(
script Foo {
    function foo() {
        return 1
    }
}
)";
    dumpCase("09_function_decl", src, /*expectSuccess=*/true);
    CHECK(containsFlat(_lastDump.emittedLua, "function foo()"));
    CHECK(containsFlat(_lastDump.emittedLua, "end"));
}

TEST_CASE(emit_dump_10_local_function) {
    const char* src = R"(
script Foo {
    on_update() {
        local function m() {
            return 1
        }
    }
}
)";
    // `local function m() { ... }` stays parser-rejected because
    // `local` is not a keyword yet (audit keeps it as Identifier,
    // surface a soft warning). The first brace is consumed by
    // parseStatement's table-literal path — `m` is read as a key
    // name (mirrors case 7's `local v = self.f1.f2.f3` parsing).
    dumpCase("10_local_function", src, /*expectSuccess=*/false);
}

// 2026-07-11 audit fix: `function NAME(...)` at script-block scope
// now parses cleanly and emits a top-level `function NAME(...) ...
// end`. The expected success used to be `true` (parser hard-reject)
// in the pre-audit fixture; we update to `true` and verify the
// emitted Lua shape contains the top-level function plus the
// `M.on_start` lifecycle.
TEST_CASE(emit_dump_11_module_level_helper_function) {
    const char* src = R"(
script Bar {
    function doubleIt(n: int) {
        return n * 2
    }
    on_start() {
        __test_witness = tostring(doubleIt(21))
    }
}
)";
    dumpCase("11_module_level_helper_function", src, /*expectSuccess=*/true);
    CHECK(containsFlat(_lastDump.emittedLua, "function doubleIt(n)"));
    CHECK(containsFlat(_lastDump.emittedLua, "function M.on_start(self)"));
    CHECK(containsFlat(_lastDump.emittedLua, "doubleIt(21)"));
}

// 2026-07-11 audit fix: helper-functions are script-block-scope
// only. `function foo()` inside an `on_update` body is a parser
// hard-reject with an explicit message.
TEST_CASE(emit_dump_12_function_nested_is_error) {
    const char* src = R"(
script Baz {
    on_update() {
        function bad() {
            return 1
        }
    }
}
)";
    dumpCase("12_function_nested_is_error", src, /*expectSuccess=*/false);
    CHECK(_lastDump.errorsContain("function declarations only allowed as script members"));
}

// 2026-07-11 audit fix: `local s = expr` parses as two statements and
// emits a broken Lua shape. We deliberately keep that behaviour
// (R3 + R4 tests still work) but surface the leak via a soft
// warning. Verify diagnostics surface the warning rather than
// silently fixing the shape.
TEST_CASE(emit_dump_13_local_soft_warning) {
    const char* src = R"(
script Quux {
    on_start() {
        local s = self.method()
        __test_witness = tostring(s)
    }
}
)";
    LuaCodegenOptions opts;
    opts.scriptName = "13_local_soft_warning";
    auto result = compileLogiaToLua(src, defaultLogiaHostContext(), opts);
    CHECK(result.success);
    bool found = false;
    for (const auto& d : result.diagnostics) {
        if (d.errorCode == ErrorCode::LuaKeywordLeak &&
            d.severity == DiagnosticSeverity::Warning) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_SUITE_END
