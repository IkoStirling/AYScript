// Logia codegen unit tests (S1)
//
// Verifies AST → Lua source lowering for the constructs supported by
// LuaCodegen. Assertions are substring-based (with optional whitespace
// normalization) — keeps tests robust against indent/blank-line tweaks.

#include "AYScript.h"
#include "logia/AYLuaCodegen.h"
#include "AYTest.h"

#include <string>

using namespace ayt::script::logia;

namespace {

// Compile a Logia snippet end-to-end through Lexer+Parser, then run
// LuaCodegen. Returns the generated Lua source (empty on failure).
std::string compileToLua(const char* logiaSource)
{
    Compiler compiler;
    auto compiled = compiler.compile(logiaSource);
    if (!compiled.success) return {};

    LuaCodegenOptions opts;
    opts.scriptName = "test";
    LuaCodegen codegen(opts);
    auto gen = codegen.generate(*compiled.program);
    if (!gen.success) return {};
    return gen.source;
}

// Collapse runs of whitespace into single spaces — for fuzzy substring
// assertions that survive indent/newline refactors.
std::string flatten(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool inWs = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!inWs && !out.empty()) out += ' ';
            inWs = true;
        } else {
            out += c;
            inWs = false;
        }
    }
    // Trim trailing space
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

bool containsFlat(const std::string& haystack, const std::string& needle)
{
    return flatten(haystack).find(flatten(needle)) != std::string::npos;
}

} // namespace

TEST_SUITE(LogiaCodegenTests)

TEST_CASE(codegen_simple_component) {
    const char* src = R"(
component Empty {
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(contains(lua, "local M = {}"));
    CHECK(contains(lua, "return M"));
}

TEST_CASE(codegen_exported_var) {
    const char* src = R"(
component Foo {
    export var x: int = 5
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "M.x = 5"));
}

TEST_CASE(codegen_exported_var_with_float) {
    const char* src = R"(
component Foo {
    export var speed: float = 5.0
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "M.speed = 5"));
}

TEST_CASE(codegen_private_var) {
    const char* src = R"(
component Foo {
    var y: int = 7
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "local y = 7"));
}

TEST_CASE(codegen_lifecycle_method) {
    const char* src = R"(
component Foo {
    on_start(entity: Entity) {
        return
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "function M.on_start(self, entity)"));
    CHECK(containsFlat(lua, "return"));
}

TEST_CASE(codegen_on_update_signature) {
    const char* src = R"(
component Foo {
    on_update(dt: float) {
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "function M.on_update(self, dt)"));
}

TEST_CASE(codegen_if_statement) {
    const char* src = R"(
component Foo {
    on_update(dt: float) {
        if input.is_pressed("jump") {
            x = 1
        }
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    // No outer parens — CallExpr emits its own bare form. The condition is
    // a call, not a binary expression, so emitExpr does not wrap it.
    CHECK(containsFlat(lua, "if input.is_pressed(\"jump\") then"));
    CHECK(containsFlat(lua, "end"));
}

TEST_CASE(codegen_compound_assignment_chain) {
    // `a.b.c = expr` lowers to a __tmp temporary that holds the
    // chain prefix; the leaf field is then assigned via the temporary.
    const char* src = R"(
component Foo {
    on_update(dt: float) {
        a.b.c = a.b.c + 1
    }
}
)";
    // Note: Logia S0 has no `+=` token; this is a plain '=' whose RHS
    // re-reads a.b.c. Codegen lowers to:
    //   local __tmp_lhs_N = a.b
    //   __tmp_lhs_N.c = (a.b.c + 1)
    // The RHS uses the *original* chain on the read side; emitExpr wraps
    // the BinaryExpr in parens.
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(contains(lua, "__tmp_"));
    CHECK(contains(lua, "local __tmp_"));
    CHECK(contains(lua, " = a.b"));                  // prefix capture
    CHECK(containsFlat(lua, "= (a.b.c + 1)"));      // RHS is parenthesized
}

TEST_CASE(codegen_assignment_to_identifier) {
    const char* src = R"(
component Foo {
    on_update(dt: float) {
        speed = speed + 1
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "speed = (speed + 1)"));
}

// DISABLED: depends on S0 Lexer supporting string escape sequences
// (\", \\, \n). The current Lexer terminates the string at the first
// raw '"' inside the literal, so the source below fails to parse and
// compileToLua returns empty. Re-enable once Lexer string handling is
// fixed (tracked separately).
#if 0
TEST_CASE(codegen_string_literal_escape) {
    const char* src = R"(
component Foo {
    on_destroy() {
        log.info("hello \"world\"\n")
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "log.info(\\\"hello"));
    CHECK(containsFlat(lua, "\\\"world\\\""));
    CHECK(containsFlat(lua, "\\n\\\")"));
}
#endif

TEST_CASE(codegen_full_player_controller) {
    // The canonical example from examples/player_controller.logia.
    const char* src = R"(
component PlayerController {
    export var speed: float = 5.0
    export var jump_force: float = 8.0

    var transform: Transform

    on_start(entity: Entity) {
        transform = entity.get_component(Transform)
    }

    on_update(dt: float) {
        if input.is_pressed("jump") {
            transform.position.y = transform.position.y + jump_force * dt
        }
        transform.position.x = transform.position.x + speed * dt
    }

    on_destroy() {
        log.info("PlayerController destroyed")
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());

    // Header
    CHECK(contains(lua, "local M = {}"));
    // Exports
    CHECK(containsFlat(lua, "M.speed = 5"));
    CHECK(containsFlat(lua, "M.jump_force = 8"));
    // Private
    CHECK(containsFlat(lua, "local transform = nil"));
    // Lifecycle signatures — each method gets (self, ...)
    CHECK(containsFlat(lua, "function M.on_start(self, entity)"));
    CHECK(containsFlat(lua, "function M.on_update(self, dt)"));
    CHECK(containsFlat(lua, "function M.on_destroy(self)"));
    // if branch lowered to then/end
    CHECK(containsFlat(lua, "then"));
    CHECK(containsFlat(lua, "end"));
    // Member-call chains preserved
    CHECK(containsFlat(lua, "input.is_pressed(\"jump\")"));
    // Compound chain lowered via __tmp
    CHECK(contains(lua, "__tmp_"));
}

TEST_SUITE_END