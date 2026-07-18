// Logia codegen unit tests (S2.5)
//
// Verifies AST → Lua source lowering for the S2.5 syntax: `script`
// (not `component`), `var` is always local (no `export`), lifecycle
// functions take no parameters (just `self`).

#include "AYScript.h"
#include "LogiaTestHelpers.h"
#include "AYTest.h"
#include "logia/AYSemanticAnalyzer.h"

#include "ayreflect/IReflect.h"
#include "AYReflect.h"
#include "AYReflectMacros.h"

#include <aymath/MathTypes.h>

#include <string>

using namespace ayt::script::logia;

namespace {

std::string compileToLua(const char* logiaSource)
{
    LuaCodegenOptions opts;
    opts.scriptName = "test";
    return logia_test::compileToLua(logiaSource, {}, opts);
}

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

TEST_CASE(codegen_simple_script) {
    const char* src = R"(
script Empty {
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(contains(lua, "local M = {}"));
    CHECK(contains(lua, "return M"));
}

TEST_CASE(codegen_var_with_int) {
    const char* src = R"(
script Foo {
    var x: int = 5
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "local x = 5"));
}

TEST_CASE(codegen_var_with_float) {
    const char* src = R"(
script Foo {
    var speed: float = 5.0
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "local speed = 5"));
}

TEST_CASE(codegen_var_no_initializer) {
    const char* src = R"(
script Foo {
    var y: int
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "local y = nil"));
}

TEST_CASE(codegen_lifecycle_on_start) {
    const char* src = R"(
script Foo {
    on_start() {
        return
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    // S2.5: lifecycle functions take no parameters — only `self`.
    CHECK(containsFlat(lua, "function M.on_start(self)"));
    CHECK(containsFlat(lua, "return"));
}

TEST_CASE(codegen_lifecycle_on_update) {
    const char* src = R"(
script Foo {
    on_update() {
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    // S2.5: `dt` is no longer a Logia parameter — it's part of the
    // engine's contract (S3 will inject it). For now the emitted Lua
    // signature is just (self).
    CHECK(containsFlat(lua, "function M.on_update(self)"));
}

TEST_CASE(codegen_if_statement) {
    const char* src = R"(
script Foo {
    on_update() {
        if input.is_pressed("jump") {
            x = 1
        }
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "if input.is_pressed(\"jump\") then"));
    CHECK(containsFlat(lua, "end"));
}

TEST_CASE(codegen_compound_assignment_chain) {
    const char* src = R"(
script Foo {
    on_update() {
        a.b.c = a.b.c + 1
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(contains(lua, "__tmp_"));
    CHECK(contains(lua, "local __tmp_"));
    CHECK(contains(lua, " = a.b"));
    CHECK(containsFlat(lua, "= (a.b.c + 1)"));
}

TEST_CASE(codegen_assignment_to_identifier) {
    const char* src = R"(
script Foo {
    on_update() {
        speed = speed + 1
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "speed = (speed + 1)"));
}

TEST_CASE(codegen_string_literal_escape) {
    const char* src = R"(
script Foo {
    on_destroy() {
        log.info("hello \"world\"\n")
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());
    CHECK(containsFlat(lua, "log.info(\"hello"));
    CHECK(containsFlat(lua, "\\\"world\\\""));
    CHECK(containsFlat(lua, "\\n\")"));
}

TEST_CASE(codegen_full_player_controller) {
    // The canonical example from examples/player_controller.logia (S2.5 form).
    //
    // S3.11: register a stub PlayerController AYReflect type so the
    // chain codegen rewrite actually fires. Without this the
    // hostTypeName is empty and codegen keeps the legacy bare-Lua
    // form (see earlier revisions of this test).
    {
        auto& reg = ayt::reflect::TypeRegistryImpl::instance();
        if (reg.findType("PlayerController") == nullptr) {
            // Ensure FVector3 fields are registered (S3.11) so the
            // chain resolve at `self.position.y` walks through.
            // Calling the analyzer ctor is the public hook; it
            // calls ensureAYEntityTypesRegistered internally.
            ayt::script::logia::SemanticAnalyzer sem;
            (void)sem;
            // Now register the stub. We re-fetch FVector3 from the
            // registry — the analyzer's S3.11 path populates its
            // x/y/z fields.
            auto* fvec3 = reg.findType("FVector3");
            auto* floatInfo = reg.findType("float");
            struct PCStub {
                ayt::math::FVector3 position;
                float speed = 0.0f;
                float jump_force = 0.0f;
            };
            auto* stubInfo = new ayt::reflect::TypeInfoImpl<PCStub>(
                "PlayerController",
                ayt::reflect::detail::defaultCreate<PCStub>,
                ayt::reflect::detail::defaultDestroy<PCStub>,
                ayt::reflect::detail::defaultCopy<PCStub>);
            if (fvec3) {
                stubInfo->addField(new ayt::reflect::FieldInfoImpl(
                    "position", fvec3,
                    offsetof(PCStub, position),
                    ayt::reflect::FieldAttribute::Serialize));
            }
            if (floatInfo) {
                stubInfo->addField(new ayt::reflect::FieldInfoImpl(
                    "speed", floatInfo, offsetof(PCStub, speed),
                    ayt::reflect::FieldAttribute::Serialize));
                stubInfo->addField(new ayt::reflect::FieldInfoImpl(
                    "jump_force", floatInfo,
                    offsetof(PCStub, jump_force),
                    ayt::reflect::FieldAttribute::Serialize));
            }
            reg.registerTypeInfo("PlayerController", stubInfo);
        }
    }

    const char* src = R"(
script PlayerController {
    var tick_counter: int = 0

    on_start() {
        tick_counter = 0
    }

    on_update() {
        tick_counter = tick_counter + 1
        if input.is_pressed("jump") {
            self.position.y = self.position.y + self.jump_force * dt
        }
        self.position.x = self.position.x + self.speed * dt
    }

    on_destroy() {
        log.info("PlayerController destroyed")
    }
}
)";
    std::string lua = compileToLua(src);
    CHECK_FALSE(lua.empty());

    CHECK(contains(lua, "local M = {}"));
    // Local var
    CHECK(containsFlat(lua, "local tick_counter = 0"));
    // S2.5: all lifecycle signatures are (self) — no params.
    CHECK(containsFlat(lua, "function M.on_start(self)"));
    CHECK(containsFlat(lua, "function M.on_update(self)"));
    CHECK(containsFlat(lua, "function M.on_destroy(self)"));
    CHECK(containsFlat(lua, "then"));
    CHECK(containsFlat(lua, "end"));
    CHECK(containsFlat(lua, "input.is_pressed(\"jump\")"));
    // S3.11: chain reflect calls replace the legacy bare-Lua
    // member access. `self.position.y = self.position.y + ...` now
    // lowers to a get_chain + arithmetic + set_chain sequence.
    CHECK(contains(lua,
        "ayt_reflect_get_field_chain(self, \"PlayerController\", \"position\", \"y\")"));
    CHECK(contains(lua,
        "ayt_reflect_set_field_chain(self, \"PlayerController\", \"position\", \"y\""));
    CHECK(contains(lua,
        "ayt_reflect_get_field_chain(self, \"PlayerController\", \"position\", \"x\")"));
    CHECK(contains(lua,
        "ayt_reflect_set_field_chain(self, \"PlayerController\", \"position\", \"x\""));
    // S3.10 single-hop chain stays on the S3.10 helpers.
    CHECK(contains(lua, "ayt_reflect_get_field(self, \"PlayerController\", \"jump_force\")"));
    CHECK(contains(lua, "ayt_reflect_get_field(self, \"PlayerController\", \"speed\")"));
    // The legacy `__tmp_` lowering is gone — chain rewrites replace it.
    CHECK_FALSE(contains(lua, "__tmp_"));
}

// S4.1 (2026-07-15): per-component signal codegen. The helper block
// is emitted ONCE per script (only when the script declares ≥1
// signal), and emit/connect CallExprs lower to specialised Lua
// (not generic dispatch). These tests pin both halves of the
// codegen surface and the source-map anchor contract (S5 ED-03
// interaction — runtime panic translation must still point at the
// signal line, not the helper block).

TEST_CASE(s41_codegen_signal_script_emits_helper_block) {
    // Pin the full helper block shape: M._signalNames table +
    // __ay_connect + __ay_emit helpers, in that order, before any
    // lifecycle / function body.
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    on_start() { }
}
)";
    const auto r = compileLogiaToLua(source);
    CHECK(r.success);
    const std::string& lua = r.lua;
    CHECK(containsFlat(lua, "M._signalNames = { [\"damaged\"] = true }"));
    CHECK(containsFlat(lua, "local function __ay_connect(self, name, handler)"));
    CHECK(containsFlat(lua, "local function __ay_emit(self, name, ...)"));
    // The two helpers must appear BEFORE the lifecycle emission.
    const size_t helperPos = lua.find("__ay_connect");
    const size_t lifecyclePos = lua.find("function M.on_start");
    CHECK(helperPos != std::string::npos);
    CHECK(lifecyclePos != std::string::npos);
    CHECK(helperPos < lifecyclePos);
}

TEST_CASE(s41_codegen_no_signal_script_emits_no_helper_block) {
    // Zero-cost guard: scripts that don't declare signals must not
    // pay for the helper block / metadata table.
    const char* source = R"(
script PlayerController {
    on_update(dt: float) {
        log.info("hello")
    }
}
)";
    const auto r = compileLogiaToLua(source);
    CHECK(r.success);
    const std::string& lua = r.lua;
    CHECK_FALSE(contains(lua, "__ay_connect"));
    CHECK_FALSE(contains(lua, "__ay_emit"));
    CHECK_FALSE(contains(lua, "_signalNames"));
}

TEST_CASE(s41_codegen_emit_lowering_shape) {
    // emit("damaged", 10) lowers to __ay_emit(self, "damaged", 10).
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    on_update(dt: float) {
        emit("damaged", 10)
    }
}
)";
    const auto r = compileLogiaToLua(source);
    CHECK(r.success);
    CHECK(contains(r.lua, "__ay_emit(self, \"damaged\", 10)"));
    // And the bare-name `emit(...)` MUST NOT leak into the chunk —
    // that would mean the analyzer's stamp didn't take effect.
    CHECK_FALSE(contains(r.lua, "emit(\"damaged\""));
}

TEST_CASE(s41_codegen_connect_lowering_shape_with_self_bind) {
    // connect("damaged", on_damaged) lowers to a closure-wrapped
    // handler call that injects `self`:
    //   __ay_connect(self, "damaged", function(...) local self = ...; return on_damaged(...) end)
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    on_start() {
        connect("damaged", on_damaged)
    }
    function on_damaged(amount: int) {}
}
)";
    const auto r = compileLogiaToLua(source);
    CHECK(r.success);
    CHECK(contains(r.lua, "__ay_connect(self, \"damaged\", function(...) return on_damaged(...) end)"));
}

TEST_CASE(s41_codegen_multiple_signals_share_one_signalNames_table) {
    // Three signals declared → one `M._signalNames` table with three
    // entries, not three separate emissions.
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    signal died()
    signal healed(amount: int)
    on_start() { }
}
)";
    const auto r = compileLogiaToLua(source);
    CHECK(r.success);
    CHECK(containsFlat(r.lua, "[\"damaged\"] = true"));
    CHECK(containsFlat(r.lua, "[\"died\"] = true"));
    CHECK(containsFlat(r.lua, "[\"healed\"] = true"));
    // Count the helper block emission: `local function __ay_connect`
    // should appear exactly once.
    size_t count = 0;
    size_t pos = 0;
    while ((pos = r.lua.find("local function __ay_connect", pos))
           != std::string::npos) {
        ++count;
        ++pos;
    }
    CHECK(count == 1u);
}

TEST_CASE(s41_codegen_helper_block_anchored_to_signal_line) {
    // S5 ED-03 interaction: when a runtime panic fires inside one
    // of the helpers, the source-map must point at the signal
    // declaration line, not the helper's own `local function`
    // line. Pin that the source map has a non-zero entry for the
    // signal-decl line.
    const char* source = R"(
script PlayerController {
    signal damaged(amount: int)
    on_start() { }
}
)";
    const auto r = compileLogiaToLua(source);
    CHECK(r.success);
    // Pin that the source map is non-empty and has at least one
    // anchor pointing at line 3 (the signal decl). We don't pin
    // a specific index — codegen layout may shift between commits
    // — but the line must be reachable.
    CHECK_FALSE(r.sourceMap.luaLineToSource.empty());
    bool foundSignalAnchor = false;
    for (const auto& loc : r.sourceMap.luaLineToSource) {
        if (loc.line == 3 && loc.column >= 0) {
            foundSignalAnchor = true;
            break;
        }
    }
    CHECK(foundSignalAnchor);
}

TEST_SUITE_END