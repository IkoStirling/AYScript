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

TEST_SUITE_END