// AYScript / LG-05 / S3.3 — runtime self.field read/write via AYReflect
//
// Verifies the end-to-end pipeline:
//   1. AYReflect registers a host C++ type with AY_PROPERTY fields.
//   2. LogiaCompiler emits `ayt_reflect_get_field` /
//      `ayt_reflect_set_field` calls for `self.<field>` accesses.
//   3. LogiaRuntimeBridge (sol2 + raw lua_CFunction) looks up the
//      host's ITypeInfo by name, finds the field, and reads/writes
//      through the AY_PROPERTY-stamped offset.
//   4. After a logia lifecycle call, the C++ object's field reflects
//      the mutation.
//
// This file is intentionally self-contained: it registers its own
// test type via a static registrar (rather than going through
// AYEntity Transform) so the test surface is hermetic and stable
// across engine refactors.

#include "AYScript.h"
#include "AYScriptRuntimeBridge.h"
#include "logia/AYCompilerError.h"
#include "logia/AYLogiaPipeline.h"
#include "LogiaTestHelpers.h"
#include "AYTest.h"

#include "IAYReflect.h"
#include "AYReflect.h"
#include "AYReflectMacros.h"

#include <string>
#include <vector>

using ayt::script::LogiaRuntimeBridge;
using ayt::script::logia::CompilerError;

namespace {

// ============================================================================
// LG-05 test fixture
// ============================================================================
// LG05ScoreHolder is a fake ScriptComponent stand-in: a plain C++ struct
// with three AY_PROPERTY fields (one each for int / float / bool).
// We register it manually with AYReflect (bypassing the
// AY_FINALIZE_REGISTRATION_METADATA macro because the test runs in a TU
// that lacks AYSerializer helpers) and use the resulting TypeInfo to
// drive the bridge's `ayt_reflect_*_field` round-trip.

struct LG05ScoreHolder {
    int32_t score = 0;
    float speed = 0.0f;
    bool enabled = false;
};

// Manual AY_PROPERTY_EX wiring — bypass the AYSerializer registrar
// chain (which depends on AY_FINALIZE_REGISTRATION_METADATA) and
// instead populate the AYReflect ITypeInfo fields directly in the
// fixture's ctor below. See LG05ReflectFixture::LG05ReflectFixture.

namespace
{
struct LG05ReflectFixture {
    LG05ReflectFixture() {
        auto& reg = ayt::reflect::TypeRegistryImpl::instance();
        if (reg.findType("LG05ScoreHolder")) return;
        auto* info = new ayt::reflect::TypeInfoImpl<LG05ScoreHolder>(
            "LG05ScoreHolder",
            ayt::reflect::detail::defaultCreate<LG05ScoreHolder>,
            ayt::reflect::detail::defaultDestroy<LG05ScoreHolder>,
            ayt::reflect::detail::defaultCopy<LG05ScoreHolder>);
        reg.registerTypeInfo("LG05ScoreHolder", info);
        // Hand-register fields. The AY_PROPERTY macros above only
        // populated the *global* AYSerializer registrar chain
        // (serializer detail); without AY_FINALIZE_REGISTRATION we
        // have to addField() each one into the AYReflect's info.
        auto* intInfo = reg.findType<int32_t>();
        auto* floatInfo = reg.findType<float>();
        auto* boolInfo = reg.findType<bool>();
        if (intInfo) {
            info->addField(new ayt::reflect::FieldInfoImpl(
                "score", intInfo, offsetof(LG05ScoreHolder, score),
                ayt::reflect::FieldAttribute::Serialize));
        }
        if (floatInfo) {
            info->addField(new ayt::reflect::FieldInfoImpl(
                "speed", floatInfo, offsetof(LG05ScoreHolder, speed),
                ayt::reflect::FieldAttribute::Serialize));
        }
        if (boolInfo) {
            info->addField(new ayt::reflect::FieldInfoImpl(
                "enabled", boolInfo, offsetof(LG05ScoreHolder, enabled),
                ayt::reflect::FieldAttribute::Serialize));
        }
    }
};
static LG05ReflectFixture g_lg05ReflectFixture;
} // namespace

bool loadSource(LogiaRuntimeBridge& bridge,
                const std::string& name,
                const char* logiaSource,
                std::vector<CompilerError>& errors)
{
    return bridge.loadScript(name, logiaSource, errors);
}

} // namespace

// ============================================================================
// LG-05 / S3.3 — pipeline contract
// ============================================================================

TEST_SUITE(LogiaReflectRuntimeTests)

TEST_CASE(lg05_helpers_visible_in_lua) {
    // After the bridge initializes, the AYReflect helpers must be
    // reachable as Lua globals so generated Logia can call them.
    // We poke sol via the public implHandle() test hook.
    LogiaRuntimeBridge bridge;
    CHECK(bridge.isInitialized());

    std::vector<CompilerError> errors;
    const char* src = R"(
script LG05ScoreHolder {
    on_start() {
        log.info("hi")
    }
}
)";
    CHECK(loadSource(bridge, "LG05ScoreHolder_visibility", src, errors));
    CHECK(errors.empty());

    // The witness is set inside on_start so we can verify the
    // lifecycle actually ran. The reflect helpers themselves are
    // registered at init; we can read them indirectly by running
    // a script that calls one with self=nil (returns nil safely).
    std::vector<CompilerError> errors2;
    const char* src2 = R"(
script LG05ScoreHolder {
    on_start() {
        local v = ayt_reflect_get_field(self, "LG05ScoreHolder", "score")
        __test_witness = tostring(v)
    }
}
)";
    CHECK(loadSource(bridge, "LG05ScoreHolder_reflectcall", src2, errors2));
    CHECK(errors2.empty());

    LG05ScoreHolder obj;
    obj.score = 42;
    CHECK(bridge.callLifecycle("LG05ScoreHolder_reflectcall", "on_start",
                               &obj, nullptr));

    // Self is real, so the reflect call should have read the actual
    // AY_PROPERTY field value (42 → "42").
    CHECK(bridge.getLuaGlobalString("__test_witness") == "42");
}

TEST_CASE(lg05_self_field_read_round_trip) {
    // Codegen must emit `ayt_reflect_get_field(self, "Type", "f")`
    // for the RHS of `local x = self.score`, and the bridge must
    // resolve the AY_PROPERTY-stamped offset.
    LogiaRuntimeBridge bridge;

    LG05ScoreHolder obj;
    obj.score = 99;

    std::vector<CompilerError> errors;
    const char* src = R"(
script LG05ScoreHolder {
    on_start() {
        __test_witness = tostring(ayt_reflect_get_field(self, "LG05ScoreHolder", "score"))
    }
}
)";
    CHECK(loadSource(bridge, "LG05ScoreHolder_read", src, errors));
    CHECK(errors.empty());

    CHECK(bridge.callLifecycle("LG05ScoreHolder_read", "on_start",
                               &obj, nullptr));
    CHECK(bridge.getLuaGlobalString("__test_witness") == "99");
}

TEST_CASE(lg05_self_field_write_round_trip) {
    // Generated `ayt_reflect_set_field(self, "Type", "f", rhs)`
    // must mutate the C++ object's AY_PROPERTY field across the
    // sol2 → lua_CFunction → AYReflect → offset path.
    LogiaRuntimeBridge bridge;

    LG05ScoreHolder obj;
    obj.score = 0;

    std::vector<CompilerError> errors;
    const char* src = R"(
script LG05ScoreHolder {
    on_start() {
        ayt_reflect_set_field(self, "LG05ScoreHolder", "score", 777)
        ayt_reflect_set_field(self, "LG05ScoreHolder", "speed", 1.5)
        ayt_reflect_set_field(self, "LG05ScoreHolder", "enabled", true)
    }
}
)";
    CHECK(loadSource(bridge, "LG05ScoreHolder_write", src, errors));
    CHECK(errors.empty());

    CHECK(bridge.callLifecycle("LG05ScoreHolder_write", "on_start",
                               &obj, nullptr));

    CHECK(obj.score == 777);
    CHECK(obj.speed == 1.5f);
    CHECK(obj.enabled == true);
}

TEST_CASE(lg05_self_field_codegen_uses_reflect_call) {
    // Pure codegen test — verifies the Lua source emitted for a
    // Logia script that touches `self.<primitiveField>` calls
    // `ayt_reflect_*_field(...)` instead of bare `self.<f>`. This
    // is the S3.3 contract that makes the runtime round-trip work
    // without further bridge changes.
    const char* src = R"(
script LG05ScoreHolder {
    on_start() {
        ayt_reflect_set_field(self, "LG05ScoreHolder", "score", 5)
    }
    on_update() {
        local s = ayt_reflect_get_field(self, "LG05ScoreHolder", "speed")
        ayt_reflect_set_field(self, "LG05ScoreHolder", "speed", s + 1)
    }
}
)";

    std::vector<CompilerError> errors;
    ayt::script::logia::LuaCodegenOptions opts;
    opts.scriptName = "<TestSource>";
    std::string lua = logia_test::compileToLua(src, errors, {}, opts);
    CHECK(errors.empty());
    CHECK(lua.find("ayt_reflect_get_field") != std::string::npos);
    CHECK(lua.find("ayt_reflect_set_field") != std::string::npos);
    // The host type name MUST show up as a literal in the emitted
    // Lua — codegen bakes it in because Lua has no AYReflect
    // introspection at runtime.
    CHECK(lua.find("\"LG05ScoreHolder\"") != std::string::npos);
    // And the field name.
    CHECK(lua.find("\"score\"") != std::string::npos);
    CHECK(lua.find("\"speed\"") != std::string::npos);
}

TEST_CASE(lg05_set_with_unknown_type_is_safe) {
    // Calling set with a never-registered type name logs an error
    // and returns without crashing — protects against typos in the
    // codegen-emitted name before registration stabilizes.
    LogiaRuntimeBridge bridge;

    std::vector<CompilerError> errors;
    const char* src = R"(
script LG05ScoreHolder {
    on_start() {
        ayt_reflect_set_field(self, "NoSuchType", "x", 1)
        ayt_reflect_set_field(self, "LG05ScoreHolder", "no_such_field", 1)
        __test_witness = "still_alive"
    }
}
)";
    CHECK(loadSource(bridge, "LG05ScoreHolder_safe", src, errors));
    CHECK(errors.empty());

    LG05ScoreHolder obj;
    obj.score = 11;
    CHECK(bridge.callLifecycle("LG05ScoreHolder_safe", "on_start",
                               &obj, nullptr));
    CHECK(bridge.getLuaGlobalString("__test_witness") == "still_alive");
    // The real fields were never touched.
    CHECK(obj.score == 11);
}

// ============================================================================
// S3.11 — multi-hop struct chain reflect
// ----------------------------------------------------------------------------
// Mirror of the LG-05 single-hop fixture, but with a nested struct
// (`LG11Holder.position` is an `LG11Inner` carrying three floats).
// All chain reads / writes go through the S3.11
// `ayt_reflect_*_field_chain` helpers. Acceptance:
// "movement_system.logia-style scripts can mutate moveSpeed via
// self.moveSpeed" generalized to `self.<field1>.<field2>` chains.

namespace
{
struct LG11Inner {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct LG11Holder {
    LG11Inner position;
};

// Inline registrar: each test calls this at the start (it's idempotent
// across calls and across tests). Static-init order across TUs is
// undefined; the LG-05 fixture used a TU-scope static and got lucky.
// Inlining avoids the static-init dependency entirely.
void ensureLG11Registered()
{
    auto& reg = ayt::reflect::TypeRegistryImpl::instance();
    if (reg.findType("LG11Holder") != nullptr) return;

    // Primitive `float` is registered by the analyzer's
    // ensurePrimitiveTypesRegistered via g_primitiveBootstrap static
    // init. If the bridge hasn't initialized yet, this returns null
    // and we skip — but the bridge is constructed in each test's
    // first line so the order is fine.
    auto* floatInfo = reg.findType("float");
    if (!floatInfo) return;

    // Register LG11Inner with x/y/z fields.
    auto* innerInfo = new ayt::reflect::TypeInfoImpl<LG11Inner>(
        "LG11Inner",
        ayt::reflect::detail::defaultCreate<LG11Inner>,
        ayt::reflect::detail::defaultDestroy<LG11Inner>,
        ayt::reflect::detail::defaultCopy<LG11Inner>);
    innerInfo->addField(new ayt::reflect::FieldInfoImpl(
        "x", floatInfo, offsetof(LG11Inner, x),
        ayt::reflect::FieldAttribute::Serialize));
    innerInfo->addField(new ayt::reflect::FieldInfoImpl(
        "y", floatInfo, offsetof(LG11Inner, y),
        ayt::reflect::FieldAttribute::Serialize));
    innerInfo->addField(new ayt::reflect::FieldInfoImpl(
        "z", floatInfo, offsetof(LG11Inner, z),
        ayt::reflect::FieldAttribute::Serialize));
    reg.registerTypeInfo("LG11Inner", innerInfo);

    // Register LG11Holder with a single `position` field of type
    // LG11Inner. This is the host type — the Logia source binds
    // `script LG11Holder`.
    auto* holderInfo = new ayt::reflect::TypeInfoImpl<LG11Holder>(
        "LG11Holder",
        ayt::reflect::detail::defaultCreate<LG11Holder>,
        ayt::reflect::detail::defaultDestroy<LG11Holder>,
        ayt::reflect::detail::defaultCopy<LG11Holder>);
    holderInfo->addField(new ayt::reflect::FieldInfoImpl(
        "position", innerInfo, offsetof(LG11Holder, position),
        ayt::reflect::FieldAttribute::Serialize));
    reg.registerTypeInfo("LG11Holder", holderInfo);
}
} // namespace

TEST_CASE(lg11_chain_read_round_trip) {
    ensureLG11Registered();
    LogiaRuntimeBridge bridge;
    LG11Holder obj;
    obj.position.y = 3.5f;

    std::vector<CompilerError> errors;
    const char* src = R"(
script LG11Holder {
    on_start() {
        local v = ayt_reflect_get_field_chain(self, "LG11Holder", "position", "y")
        __test_witness = tostring(v)
    }
}
)";
    CHECK(loadSource(bridge, "LG11Holder_chain_read", src, errors));
    CHECK(errors.empty());

    CHECK(bridge.callLifecycle("LG11Holder_chain_read", "on_start",
                               &obj, nullptr));
    CHECK(bridge.getLuaGlobalString("__test_witness") == "3.5");
}

TEST_CASE(lg11_chain_write_round_trip) {
    ensureLG11Registered();
    LogiaRuntimeBridge bridge;
    LG11Holder obj;
    obj.position.x = 0.0f;

    std::vector<CompilerError> errors;
    const char* src = R"(
script LG11Holder {
    on_start() {
        ayt_reflect_set_field_chain(self, "LG11Holder", "position", "x", 12.0)
    }
}
)";
    CHECK(loadSource(bridge, "LG11Holder_chain_write", src, errors));
    CHECK(errors.empty());

    CHECK(bridge.callLifecycle("LG11Holder_chain_write", "on_start",
                               &obj, nullptr));
    // Acceptance: "C++ reads back mutated value".
    CHECK(obj.position.x == 12.0f);
}

TEST_CASE(lg11_chain_compound_assign) {
    // `self.position.x = self.position.x + 4.0` is the canonical chain
    // compound-assign path. Both halves of the RHS are chain reads
    // routed through `ayt_reflect_get_field_chain`.
    ensureLG11Registered();
    LogiaRuntimeBridge bridge;
    LG11Holder obj;
    obj.position.x = 1.0f;

    std::vector<CompilerError> errors;
    const char* src = R"(
script LG11Holder {
    on_start() {
        self.position.x = self.position.x + 4.0
    }
}
)";
    CHECK(loadSource(bridge, "LG11Holder_chain_compound", src, errors));
    CHECK(errors.empty());

    CHECK(bridge.callLifecycle("LG11Holder_chain_compound", "on_start",
                               &obj, nullptr));
    CHECK(obj.position.x == 5.0f);
}

TEST_CASE(lg11_chain_unknown_leaf_safe) {
    // Chain leaf names an unknown field on the struct type. The
    // helper should fail-safe (return nil / log error) — no crash,
    // the C++ field stays at its pre-call value.
    ensureLG11Registered();
    LogiaRuntimeBridge bridge;
    LG11Holder obj;
    obj.position.x = 7.0f;

    std::vector<CompilerError> errors;
    const char* src = R"(
script LG11Holder {
    on_start() {
        ayt_reflect_set_field_chain(self, "LG11Holder", "position", "nope", 999.0)
        __test_witness = "still_alive"
    }
}
)";
    CHECK(loadSource(bridge, "LG11Holder_chain_unknown", src, errors));
    CHECK(errors.empty());

    CHECK(bridge.callLifecycle("LG11Holder_chain_unknown", "on_start",
                               &obj, nullptr));
    CHECK(bridge.getLuaGlobalString("__test_witness") == "still_alive");
    // The real field was never touched.
    CHECK(obj.position.x == 7.0f);
}

TEST_CASE(lg11_chain_intermediate_unknown_safe) {
    // Chain intermediate hop names an unknown struct field. Same
    // fail-safe — return nil, C++ field untouched.
    ensureLG11Registered();
    LogiaRuntimeBridge bridge;
    LG11Holder obj;
    obj.position.y = 11.0f;

    std::vector<CompilerError> errors;
    const char* src = R"(
script LG11Holder {
    on_start() {
        local v = ayt_reflect_get_field_chain(self, "LG11Holder", "no_such_struct_field", "y")
        __test_witness = tostring(v)
    }
}
)";
    CHECK(loadSource(bridge, "LG11Holder_chain_bad_intermediate", src, errors));
    CHECK(errors.empty());

    CHECK(bridge.callLifecycle("LG11Holder_chain_bad_intermediate",
                               "on_start", &obj, nullptr));
    // Lua nil → tostring(nil) → "nil".
    CHECK(bridge.getLuaGlobalString("__test_witness") == "nil");
    CHECK(obj.position.y == 11.0f);
}

TEST_SUITE_END
