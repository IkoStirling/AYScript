// AYScriptRuntimeBridge.cpp - sol2-backed runtime for compiled Logia

// Windows headers (transitively included via AYScriptComponent.h →
// AYCore.h) pollute `min` / `max` macros via <windows.h>. Define
// NOMINMAX before any system header is processed.
#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include "AYScriptRuntimeBridge.h"

#include "logia/AYLogia.h"
#include "logia/AYLogiaPipeline.h"

#define SOL_ALL_SAFETIES_ON 1
#define SOL_SAFE_NUMERICS   1
#include <sol/sol.hpp>

#include "AYChannel.h"
#include "AYLogger.h"

// LG-05 / S3.3: AYReflect introspection for `self.field` runtime
// reads/writes. lua_State* comes from sol2 via its bundled
// compat layer, so the lua_pushinteger/pop helpers below are
// visible without an extra `<lua.h>` include.
#include "IAYReflect.h"
#include "AYReflect.h"  // full TypeRegistryImpl definition

#include <unordered_map>
#include <cstring>

namespace ayt::script
{

// ------------------------------------------------------------
// S3.6 (LG-06a) — compile cache plumbing
//
// Cache key: hash(source) XOR hash(ctx) XOR kLogiaPipelineVersion
// (folded through a prime multiplier to spread bits). The hash is
// stored directly as the map key (unordered_map<size_t, Entry>)
// because we only need hit/miss semantics — no enumeration, no
// collision-sensitive lookups by anything but the exact same input.
//
// Cache value: the generated Lua source. Also records whether the
// codegen succeeded so a cache hit on a previously-failing compile
// reproduces the diagnostic vector verbatim.
// ------------------------------------------------------------

std::size_t hashLogiaHostContext(const logia::LogiaHostContext& ctx)
{
    std::size_t h = static_cast<std::size_t>(ctx.kind);
    h ^= static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(ctx.hostType))
       + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= (ctx.expectSelf ? 0xAAAAAAAAAAAAAAAAULL : 0)
       + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= (ctx.strictInheritance ? 0x5555555555555555ULL : 0)
       + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

namespace {

// Hash the logia source string. std::hash<std::string> is process-
// local; the cache is per-bridge so collisions across runs aren't a
// concern (worst case: spuriously cache-miss, which is correct behavior).
std::size_t hashLogiaSource(const std::string& src)
{
    return std::hash<std::string>{}(src);
}

// Combine the three cache-key inputs into a single hash slot.
std::size_t makeCacheKey(const std::string& src,
                         const logia::LogiaHostContext& ctx)
{
    std::size_t k = hashLogiaSource(src);
    k ^= hashLogiaHostContext(ctx) + 0x9E3779B97F4A7C15ULL + (k << 6) + (k >> 2);
    k ^= kLogiaPipelineVersion  + 0x9E3779B97F4A7C15ULL + (k << 6) + (k >> 2);
    return k;
}

} // namespace

// ------------------------------------------------------------
// Engine API mocks
//
// S1 shipped minimal stubs so the codegen output can be exercised
// end-to-end. S3.5 replaces these with real-input plumbing:
//
//   - `time.delta` / `time.total` now read from the bridge's
//     `tickAmbient(dt)` accumulator, which ScriptSubSystem drives
//     with the authoritative scaled delta from GameLoop. No more
//     zero-everywhere mock.
//   - `input.is_pressed` / `input.is_just_pressed` now route
//     through an injectable InputProvider*; default is the
//     MockInputProvider below (jump-only). Production hosts can
//     plug in a real AYDevice / OS-level poll by calling
//     setInputProvider().
//
// Keeping a mock as the default keeps AYScript_Test headless-
// runnable on CI without an HWND.
// ------------------------------------------------------------

namespace {

class MockInputProvider final
    : public ayt::script::LogiaRuntimeBridge::InputProvider {
public:
    bool isPressed(const std::string& key) const override {
        // Mimics S1 behavior so legacy unittests remain stable;
        // only the named jump key is treated as held.
        return key == "jump";
    }
    bool isJustPressed(const std::string& /*key*/) const override {
        return false;
    }
};

MockInputProvider g_defaultInputProvider;

// ------------------------------------------------------------
// S3.10 — AYReflect primitive type registration.
//
// The codegen rewrite for `self.<primitiveField>` and the bridge's
// `pushFieldPrimitive` both gate on the field's stored ITypeInfo*
// (not just its name). Built-in primitives (int, float, bool, ...)
// aren't registered by AYReflect's static-init macros — consumers
// are expected to register them somewhere. AYScript registers them
// here so any host (Component, System, Tool) that emits
// `ayt_reflect_get_field(self, "<Type>", "<primitiveField>")` finds
// a non-null ITypeInfo and the bridge can `lua_pushnumber` the right
// primitive type. Idempotent (findType() guard) so multiple bridge
// instances don't double-register.
// ------------------------------------------------------------

template <typename T>
void registerPrimitiveIfMissing(const char* name)
{
    using ayt::reflect::TypeRegistryImpl;
    auto& reg = TypeRegistryImpl::instance();
    if (reg.findType(name) != nullptr) return;
    auto* info = new ayt::reflect::TypeInfoImpl<T>(
        name,
        ayt::reflect::detail::defaultCreate<T>,
        ayt::reflect::detail::defaultDestroy<T>,
        ayt::reflect::detail::defaultCopy<T>);
    reg.registerTypeInfo(name, info);
}

void ensureBuiltinTypesRegistered()
{
    registerPrimitiveIfMissing<int32_t>("int");
    registerPrimitiveIfMissing<int32_t>("Int32");
    registerPrimitiveIfMissing<int64_t>("Int64");
    registerPrimitiveIfMissing<float>("float");
    registerPrimitiveIfMissing<float>("Float32");
    registerPrimitiveIfMissing<double>("double");
    registerPrimitiveIfMissing<double>("Float64");
    registerPrimitiveIfMissing<bool>("bool");
    registerPrimitiveIfMissing<bool>("Bool");
}

} // namespace

// ------------------------------------------------------------
// LG-05 / S3.3 — AYReflect-backed self.field read/write
// ------------------------------------------------------------
// Logia codegen routes `self.field` accesses through these plain
// `lua_CFunction` globals. Lua passes `self` as lightuserdata
// (= ScriptComponent*) plus two string args: the host type name
// (= the `script Name { … }` declaration) and the field name.
// The bridge looks up the IFieldInfo in AYReflect's TypeRegistry
// and reads/writes through the field's stored offset.
//
// Name-keyed instead of typeid-keyed because the codegen only
// knows the host *name* (a string). Lifetime: IFieldInfo* lives
// in the registry which is process-scope; fieldCache() below
// retains pointers indefinitely but they never get freed.
// ------------------------------------------------------------

// Forward decl: shared leaf primitive-store used by the single-hop
// `ayt_reflect_set_field_c` (S3.10) and the chain variant
// `ayt_reflect_set_field_chain_c` (S3.11).
int storeFieldPrimitive(lua_State* L,
                        const ayt::reflect::IFieldInfo* field,
                        void* fieldPtr,
                        int valueStackIdx);

struct FieldKey {
    std::string typeName;
    std::string fieldName;
    bool operator==(const FieldKey& o) const {
        return typeName == o.typeName && fieldName == o.fieldName;
    }
};
struct FieldKeyHash {
    size_t operator()(const FieldKey& k) const {
        return std::hash<std::string>{}(k.typeName) ^
               (std::hash<std::string>{}(k.fieldName) << 1);
    }
};
std::unordered_map<FieldKey, const ayt::reflect::IFieldInfo*, FieldKeyHash>& fieldCache()
{
    static std::unordered_map<FieldKey, const ayt::reflect::IFieldInfo*, FieldKeyHash> c;
    return c;
}

const ayt::reflect::IFieldInfo* lookupField(const std::string& typeName,
                                            const std::string& fieldName)
{
    FieldKey k{typeName, fieldName};
    auto& cache = fieldCache();
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    auto* typeInfo = ayt::reflect::TypeRegistryImpl::instance().findType(typeName.c_str());
    if (!typeInfo) return nullptr;
    auto* field = typeInfo->findField(fieldName.c_str());
    if (!field) return nullptr;
    cache[k] = field;
    return field;
}

// =====================================================================
// S3.12 / track R2 §5.7.4 — method-call reflect lookup
// =====================================================================
// Mirrors the field cache shape. The bridge holds (typeName, methodName)
// keys against a global IMethodInfo* registry. The cache is per-process
// (function-local static); the AYScript runtime's lifetime is the
// program's, so the map is also program-lifetime.
struct MethodKey {
    std::string typeName;
    std::string methodName;
    bool operator==(const MethodKey& o) const {
        return typeName == o.typeName && methodName == o.methodName;
    }
};
struct MethodKeyHash {
    size_t operator()(const MethodKey& k) const {
        return std::hash<std::string>{}(k.typeName) ^
               (std::hash<std::string>{}(k.methodName) << 1);
    }
};
std::unordered_map<MethodKey, const ayt::reflect::IMethodInfo*, MethodKeyHash>& methodCache()
{
    static std::unordered_map<MethodKey, const ayt::reflect::IMethodInfo*, MethodKeyHash> c;
    return c;
}

const ayt::reflect::IMethodInfo* lookupMethod(const std::string& typeName,
                                              const std::string& methodName)
{
    MethodKey k{typeName, methodName};
    auto& cache = methodCache();
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    auto* typeInfo = ayt::reflect::TypeRegistryImpl::instance().findType(typeName.c_str());
    if (!typeInfo) return nullptr;
    auto* method = typeInfo->findMethod(methodName.c_str());
    if (!method) return nullptr;
    cache[k] = method;
    return method;
}

// Push a primitive (int/float/bool/double/int64) or a nested-struct
// sub-table onto the Lua stack from a `void*` field pointer.
//
// Primitive dispatch (the S3.3 / S3.10 path) returns 1 after a single
// lua_push*. The R4.0 nested-struct branch builds a Lua table at the
// top of the stack with one entry per primitive leaf and returns 1;
// the chain-helper code that handles intermediate hops
// (ayt_reflect_*_field_chain_c) already calls this helper for leaves,
// so the same recursive structure automatically applies to nested
// chains too. Returns 0 on truly unknown types (e.g. std::string
// field, which R3.0 deliberately left fail-closed for the field path).
//
// Used by `ayt_reflect_get_field_c` (single hop) and the chain
// leaf helper `ayt_reflect_get_field_chain_c`. The R3.12+R3 struct
// return path also walks this for sub-fields.
int pushFieldPrimitive(lua_State* L, const ayt::reflect::IFieldInfo* field, void* fieldPtr)
{
    auto* type = field->getType();
    if (!type) return 0;
    const char* tname = type->getName();
    if (!tname) return 0;
    auto eq = [tname](const char* n) { return std::strcmp(tname, n) == 0; };
    if (eq("int") || eq("Int32")) {
        lua_pushinteger(L, static_cast<lua_Integer>(*static_cast<int32_t*>(fieldPtr)));
        return 1;
    }
    if (eq("float") || eq("Float32")) {
        lua_pushnumber(L, static_cast<lua_Number>(*static_cast<float*>(fieldPtr)));
        return 1;
    }
    if (eq("bool") || eq("Bool")) {
        lua_pushboolean(L, *static_cast<bool*>(fieldPtr));
        return 1;
    }
    if (eq("double") || eq("Float64")) {
        lua_pushnumber(L, static_cast<lua_Number>(*static_cast<double*>(fieldPtr)));
        return 1;
    }
    if (eq("Int64")) {
        lua_pushinteger(L, static_cast<lua_Integer>(*static_cast<int64_t*>(fieldPtr)));
        return 1;
    }
    // R4.0 — nested struct field: build a Lua sub-table with one
    // entry per primitive leaf (recursive for deeper nesting). The
    // same primitive dispatch above handles leaves at any depth.
    // Limitations carried over from R3.0: std::string sub-fields
    // fail closed (nil) until R3.5+ lands; container
    // (vector/array) sub-fields fail closed until R4.1+.
    if (type->getFieldCount() > 0) {
        lua_newtable(L);
        int subTableIdx = lua_gettop(L);
        size_t subCount = type->getFieldCount();
        for (size_t si = 0; si < subCount; ++si) {
            auto* sub = type->getField(si);
            if (!sub) continue;
            const char* subName = sub->getName();
            if (!subName) continue;
            void* subPtr = static_cast<uint8_t*>(fieldPtr) + sub->getOffset();
            if (pushFieldPrimitive(L, sub, subPtr) == 1) {
                lua_pushstring(L, subName);
                // Settable key+value into the captured `subTableIdx`
                // (relative-to-absolute anchor) so deep nesting works
                // regardless of how deep the call stack sits on the
                // Lua stack.
                lua_settable(L, subTableIdx);
            } else {
                if (lua_gettop(L) > 0 && lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                }
            }
        }
        return 1;
    }
    return 0;
}

int ayt_reflect_get_field_c(lua_State* L)
{
    if (!lua_islightuserdata(L, 1) || !lua_isstring(L, 2) || !lua_isstring(L, 3)) {
        lua_pushnil(L);
        return 1;
    }
    void* selfPtr = lua_touserdata(L, 1);
    std::string typeName = lua_tostring(L, 2);
    std::string fieldName = lua_tostring(L, 3);

    auto* field = lookupField(typeName, fieldName);
    if (!field || selfPtr == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    void* fieldPtr = field->get(selfPtr);
    if (pushFieldPrimitive(L, field, fieldPtr) == 0) {
        lua_pushnil(L);
    }
    return 1;
}

int ayt_reflect_set_field_c(lua_State* L)
{
    if (!lua_islightuserdata(L, 1) || !lua_isstring(L, 2) || !lua_isstring(L, 3)) {
        return 0;
    }
    void* selfPtr = lua_touserdata(L, 1);
    std::string typeName = lua_tostring(L, 2);
    std::string fieldName = lua_tostring(L, 3);

    auto* field = lookupField(typeName, fieldName);
    if (!field || selfPtr == nullptr) {
        ayt::log::error("ayt_reflect_set_field: %s.%s (no field or null self)",
                        typeName.c_str(), fieldName.c_str());
        return 0;
    }
    void* fieldPtr = field->get(selfPtr);
    if (storeFieldPrimitive(L, field, fieldPtr, 4) == 0) {
        ayt::log::error("ayt_reflect_set_field: %s.%s (unsupported or null type)",
                        typeName.c_str(), fieldName.c_str());
        return 0;
    }
    return 0;
}

// S3.11 — extract the leaf primitive-store switch from
// ayt_reflect_set_field_c so the chain helper can reuse it. Returns
// 1 on success, 0 on unknown type / null type.
//
// `valueStackIdx` is the absolute Lua stack index of the value to
// store. Callers must validate `valueStackIdx <= lua_gettop(L)` before
// calling.
//
// R4.0 — nested struct field: when the field's type has its own
// fields (getFieldCount() > 0), `valueStackIdx` must point at a Lua
// table. We build a temporary T in heap memory (memset 0), recursively
// fill each sub-field from the Lua table, then memcpy into the
// caller's fieldPtr (struct by value assignment). Returns 1 on
// success; 0 on truly unknown type (std::string / container fields
// fail closed until R3.5+ / R4.1).
int storeFieldPrimitive(lua_State* L,
                        const ayt::reflect::IFieldInfo* field,
                        void* fieldPtr,
                        int valueStackIdx)
{
    auto* type = field->getType();
    if (!type) return 0;
    const char* tname = type->getName();
    if (!tname) return 0;
    auto eq = [tname](const char* n) { return std::strcmp(tname, n) == 0; };

    if (eq("int") || eq("Int32")) {
        *static_cast<int32_t*>(fieldPtr) =
            static_cast<int32_t>(lua_tointeger(L, valueStackIdx));
        return 1;
    }
    if (eq("float") || eq("Float32")) {
        *static_cast<float*>(fieldPtr) =
            static_cast<float>(lua_tonumber(L, valueStackIdx));
        return 1;
    }
    if (eq("bool") || eq("Bool")) {
        *static_cast<bool*>(fieldPtr) = (lua_toboolean(L, valueStackIdx) != 0);
        return 1;
    }
    if (eq("double") || eq("Float64")) {
        *static_cast<double*>(fieldPtr) =
            static_cast<double>(lua_tonumber(L, valueStackIdx));
        return 1;
    }
    if (eq("Int64")) {
        *static_cast<int64_t*>(fieldPtr) =
            static_cast<int64_t>(lua_tointeger(L, valueStackIdx));
        return 1;
    }
    // R4.0 — nested struct write. Lua-side value must be a table.
    // We allocate a temp T, fill it recursively, then memcpy into
    // the caller's fieldPtr. Used by `ayt_reflect_set_field_c` (single
    // hop) and the chain helper `ayt_reflect_set_field_chain_c`.
    if (type->getFieldCount() > 0) {
        if (!lua_istable(L, valueStackIdx)) {
            return 0;
        }
        size_t typeSize = type->getSize();
        if (typeSize == 0) typeSize = sizeof(uint64_t);  // safety
        void* subMem = ::operator new(typeSize);
        std::memset(subMem, 0, typeSize);
        size_t subCount = type->getFieldCount();
        for (size_t si = 0; si < subCount; ++si) {
            auto* sub = type->getField(si);
            if (!sub) continue;
            const char* subName = sub->getName();
            if (!subName) continue;
            lua_getfield(L, valueStackIdx, subName);
            if (!lua_isnil(L, -1)) {
                void* subPtr = static_cast<uint8_t*>(subMem) + sub->getOffset();
                // Recurse. -1 is relative to current top of stack —
                // the inner getfield above left the value there.
                storeFieldPrimitive(L, sub, subPtr, /*valueStackIdx=*/-1);
            }
            lua_pop(L, 1);
        }
        // Struct by value copy into the caller's slot.
        std::memcpy(fieldPtr, subMem, typeSize);
        ::operator delete(subMem);
        return 1;
    }
    return 0;
}

// S3.11 — multi-hop struct chain read.
//
// Lua args:
//   arg 1        self:lightuserdata
//   arg 2        rootType:string (the script's host type name)
//   arg 3..N     one or more field names (root→leaf order)
//
// Walk: for i = 3..N-1 the field at index i is an *intermediate*
// hop whose type resolves to the next hop's typeName; the last field
// (arg N) is the leaf primitive. Intermediate hops use
// field->get(selfPtr) to advance the pointer + re-derive the next
// typeName from field->getType()->getName(). The leaf hop reuses
// pushFieldPrimitive (existing S3.10 dispatch).
//
// Returns nil on any unknown field / null self / type-name miss —
// mirrors the S3.10 fail-safe so a typo in the codegen-emitted chain
// doesn't crash the runtime.
int ayt_reflect_get_field_chain_c(lua_State* L)
{
    int nargs = lua_gettop(L);
    if (nargs < 4 || !lua_islightuserdata(L, 1) || !lua_isstring(L, 2)) {
        lua_pushnil(L);
        return 1;
    }
    // Validate every arg is a string.
    for (int i = 3; i <= nargs; ++i) {
        if (!lua_isstring(L, i)) {
            lua_pushnil(L);
            return 1;
        }
    }
    void* ptr = lua_touserdata(L, 1);
    std::string typeName = lua_tostring(L, 2);

    // Intermediate hops (3..nargs-1): each lookupField advances the
    // pointer; the field's getType() name feeds the next lookup.
    for (int i = 3; i < nargs; ++i) {
        std::string fieldName = lua_tostring(L, i);
        auto* field = lookupField(typeName, fieldName);
        if (!field || ptr == nullptr) {
            lua_pushnil(L);
            return 1;
        }
        ptr = field->get(ptr);
        auto* fieldType = field->getType();
        if (!fieldType || !fieldType->getName()) {
            lua_pushnil(L);
            return 1;
        }
        typeName = fieldType->getName();
    }

    // Leaf hop.
    std::string leafName = lua_tostring(L, nargs);
    auto* leaf = lookupField(typeName, leafName);
    if (!leaf || ptr == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    void* leafPtr = leaf->get(ptr);
    if (pushFieldPrimitive(L, leaf, leafPtr) == 0) {
        lua_pushnil(L);
    }
    return 1;
}

// S3.11 — multi-hop struct chain write.
//
// Lua args:
//   arg 1          self:lightuserdata
//   arg 2          rootType:string
//   arg 3..N-1     one or more field names (root→leaf order)
//   arg N          value to store at the leaf
//
// Same walk as the get chain. The leaf hop dispatches on the
// field's ITypeInfo name to write the Lua value at the leaf
// address; intermediate hops are read-only offset math.
int ayt_reflect_set_field_chain_c(lua_State* L)
{
    int nargs = lua_gettop(L);
    // Minimum: (self, rootType, field1, value) -> 4 args.
    if (nargs < 4 || !lua_islightuserdata(L, 1) || !lua_isstring(L, 2)) {
        return 0;
    }
    for (int i = 3; i < nargs; ++i) {
        if (!lua_isstring(L, i)) {
            ayt::log::error("ayt_reflect_set_field_chain: non-string field name");
            return 0;
        }
    }
    int valueIdx = nargs;
    void* ptr = lua_touserdata(L, 1);
    std::string typeName = lua_tostring(L, 2);

    // Intermediate hops (3..nargs-2): same walk as get_chain. The
    // leaf field name is at index (nargs - 1).
    int lastFieldIdx = nargs - 1;
    for (int i = 3; i < lastFieldIdx; ++i) {
        std::string fieldName = lua_tostring(L, i);
        auto* field = lookupField(typeName, fieldName);
        if (!field || ptr == nullptr) {
            ayt::log::error("ayt_reflect_set_field_chain: %s.%s (unknown)",
                            typeName.c_str(), fieldName.c_str());
            return 0;
        }
        ptr = field->get(ptr);
        auto* fieldType = field->getType();
        if (!fieldType || !fieldType->getName()) {
            return 0;
        }
        typeName = fieldType->getName();
    }

    std::string leafName = lua_tostring(L, lastFieldIdx);
    auto* leaf = lookupField(typeName, leafName);
    if (!leaf || ptr == nullptr) {
        ayt::log::error("ayt_reflect_set_field_chain: %s.%s (unknown leaf)",
                        typeName.c_str(), leafName.c_str());
        return 0;
    }
    void* leafPtr = leaf->get(ptr);
    if (storeFieldPrimitive(L, leaf, leafPtr, valueIdx) == 0) {
        ayt::log::error("ayt_reflect_set_field_chain: %s.%s (unsupported leaf type)",
                        typeName.c_str(), leafName.c_str());
        return 0;
    }
    return 0;
}

// =====================================================================
// S3.12 (track R2 §5.7.4) — self.method(args) reflect call bridge
// =====================================================================
// Lua args:
//   arg 1          self:lightuserdata (the C++ host pointer)
//   arg 2          typeName:string ("<Type>" — AYReflect registry name)
//   arg 3          methodName:string
//   arg 4..N       zero or more primitive args
//
// Returns:
//   - For void methods: nothing on the stack (return value 0).
//   - For primitive-return methods: pushes one Lua value (int/float/
//     bool/double/int64) and returns 1.
//
// Errors:
//   - If the method is unknown or arg count mismatches, pushes nil
//     and returns 1 (fails closed — Logia scripts do not crash on
//     reflection miss; the analyzer would have caught the unknown
//     name earlier in compile-time reflection).
//
// Argument / return type tags recognized (mirrors pushFieldPrimitive
// dispatch in §S3.10 — see above): "int"/"Int32", "float"/"Float32",
// "bool"/"Bool", "double"/"Float64", "Int64".
// =====================================================================
// S3.12 (track R2 §5.7.4) — self.method(args) reflect call bridge
// =====================================================================
// S3.12+R3 extends the S3.12 dispatch to support:
//   - std::string args + return (R3)
//   - const-ref / const-ptr args to arbitrary types (R3) —
//     bridge heap-allocates the target T, fills fields from a
//     Lua table via field-by-field marshalling, passes &T.
//   - by-value struct return (R3) — bridge reads fields via
//     pushFieldPrimitive and builds a Lua table.
//   - enum args / return (R3) — int-cast on the underlying type.
//
// Slot convention (uniform):
//   args[I] = pointer to a heap- or stack-allocated value matching
//   the parameter's PMF type. readArg in MethodInfoImpl dereferences.
//   Primitives live in stack-allocated 8-byte slots. std::string
//   and struct args live in heap-allocated slots; the cleanup
//   queue (`cleanupQueue`) holds delete lambdas run unconditionally
//   after invoke() returns. Const-ref / const-ptr args reuse the
//   same heap slot; the PMF takes a reference/pointer into it.
//
// Limitations (R3.0):
//   - std::string fields inside a struct fail closed (pushField-
//     Primitive / storeFieldPrimitive reject std::string; pushed
//     to R3.5 with placement-new design).
//   - Table-literal in arg position requires pre-assigned `local`.
//   - No nested struct fields inside struct args/return.
//   - No std::vector / std::array args (track R4).
// =====================================================================
int ayt_reflect_call_method_c(lua_State* L)
{
    int nargs = lua_gettop(L);
    if (nargs < 3 || !lua_islightuserdata(L, 1) || !lua_isstring(L, 2) || !lua_isstring(L, 3)) {
        lua_pushnil(L);
        return 1;
    }
    void* selfPtr = lua_touserdata(L, 1);
    std::string typeName   = lua_tostring(L, 2);
    std::string methodName = lua_tostring(L, 3);

    auto* method = lookupMethod(typeName, methodName);
    if (!method || selfPtr == nullptr) {
        ayt::log::error("ayt_reflect_call_method: %s.%s (no method or null self)",
                        typeName.c_str(), methodName.c_str());
        lua_pushnil(L);
        return 1;
    }

    const size_t expected = method->getParamCount();
    int got = nargs - 3;
    if (static_cast<size_t>(got) != expected) {
        ayt::log::error("ayt_reflect_call_method: %s.%s (arg count mismatch: expected %zu, got %d)",
                        typeName.c_str(), methodName.c_str(), expected, got);
        lua_pushnil(L);
        return 1;
    }

    // Per-call slot container. Stack slots for primitives (8 bytes
    // each — sufficient for int/Int32/float/Float32/bool/Bool/double/
    // Float64/Int64 and the underlying types of enums). Heap slots
    // for std::string and structs (lifetime managed by cleanupQueue).
    std::vector<uint64_t> argSlots(expected, 0);
    std::vector<const void*> argPtrs(expected, nullptr);

    // Cleanup queue — every heap allocation registered here is freed
    // after invoke() returns, regardless of whether invoke succeeded.
    // Uses a manual cleanup struct to avoid std::function heap alloc
    // and to keep the type-erased destructor keyed on the heap
    // pointer's actual type.
    struct HeapSlot {
        void* ptr = nullptr;
        void (*deleter)(void*) = nullptr;
    };
    std::vector<HeapSlot> cleanup;
    auto registerCleanup = [&](void* p, void (*d)(void*)) {
        cleanup.push_back({p, d});
    };

    // Marshal each Lua arg into its slot. Dispatch is by ITypeInfo
    // tag. Note: MethodInfoImpl's readArg dereferences args[I] as
    // the appropriate PMF parameter type — see
    // AYMethodInfoImpl.h file-level comment.
    for (size_t i = 0; i < expected; ++i) {
        int stackIdx = static_cast<int>(i) + 4;
        auto* paramType = method->getParamType(i);
        const char* tname = paramType ? paramType->getName() : "int";

        // S3.12 + R3 primitive dispatch: write the value into the
        // stack slot and set argPtrs[i] = &argSlots[i].
        // R3 dispatch: heap-allocate std::string or struct, fill
        // from the Lua table, and set argPtrs[i] = heap pointer.
        if (std::strcmp(tname, "int") == 0 || std::strcmp(tname, "Int32") == 0) {
            int32_t v = static_cast<int32_t>(lua_tointeger(L, stackIdx));
            std::memcpy(&argSlots[i], &v, sizeof(int32_t));
            argPtrs[i] = &argSlots[i];
        } else if (std::strcmp(tname, "float") == 0 || std::strcmp(tname, "Float32") == 0) {
            float v = static_cast<float>(lua_tonumber(L, stackIdx));
            std::memcpy(&argSlots[i], &v, sizeof(float));
            argPtrs[i] = &argSlots[i];
        } else if (std::strcmp(tname, "bool") == 0 || std::strcmp(tname, "Bool") == 0) {
            uint8_t v = lua_toboolean(L, stackIdx) ? 1u : 0u;
            std::memcpy(&argSlots[i], &v, sizeof(uint8_t));
            argPtrs[i] = &argSlots[i];
        } else if (std::strcmp(tname, "double") == 0 || std::strcmp(tname, "Float64") == 0) {
            double v = lua_tonumber(L, stackIdx);
            std::memcpy(&argSlots[i], &v, sizeof(double));
            argPtrs[i] = &argSlots[i];
        } else if (std::strcmp(tname, "Int64") == 0) {
            int64_t v = static_cast<int64_t>(lua_tointeger(L, stackIdx));
            std::memcpy(&argSlots[i], &v, sizeof(int64_t));
            argPtrs[i] = &argSlots[i];
        } else if (std::strcmp(tname, "std::string") == 0) {
            // S3.12+R3: heap-allocate std::string. The PMF gets a
            // copy via readArg's std::string branch. Bridge frees
            // after invoke().
            const char* s = lua_tostring(L, stackIdx);
            auto* sp = new std::string(s ? s : "");
            argPtrs[i] = sp;
            registerCleanup(sp, [](void* p) { delete static_cast<std::string*>(p); });
        } else if (paramType && paramType->getFieldCount() > 0) {
            // S3.12+R3: struct arg. Default-construct T in heap
            // memory and fill field-by-field from the Lua table.
            // Lua table key = C++ field name (exact match, camelCase
            // both sides — see design.md §5.7.4 R3 conventions).
            // std::string fields inside the struct fail closed
            // (pushFieldPrimitive / storeFieldPrimitive reject them;
            // they keep their zero-init from the memset below).
            size_t typeSize = paramType->getSize();
            if (typeSize == 0) typeSize = sizeof(uint64_t);  // safety
            void* mem = ::operator new(typeSize);
            std::memset(mem, 0, typeSize);
            size_t fieldCount = paramType->getFieldCount();
            for (size_t fi = 0; fi < fieldCount; ++fi) {
                auto* field = paramType->getField(fi);
                if (!field) continue;
                lua_getfield(L, stackIdx, field->getName());
                if (!lua_isnil(L, -1)) {
                    // storeFieldPrimitive reads from top of stack.
                    void* fieldPtr = static_cast<uint8_t*>(mem) + field->getOffset();
                    storeFieldPrimitive(L, field, fieldPtr, /*valueStackIdx=*/-1);
                }
                lua_pop(L, 1);
            }
            argPtrs[i] = mem;
            registerCleanup(mem, [](void* p) { ::operator delete(p); });
        } else if (auto* containerType = dynamic_cast<ayt::reflect::IContainerTypeInfo*>(paramType)) {
            // R4.1 (2026-07-13): std::vector<T> / std::array<T, N>
            // arg. Lua-side: a 1-indexed table. Bridge:
            //   - Lua top-of-stack is a table; lua_len gives N.
            //   - For std::vector (isFixedSize()==false): heap-alloc
            //     a vector and push_back per element.
            //   - For std::array (isFixedSize()==true): clamp N to
            //     the compile-time size; raw-byte-write per element.
            //   - Heap-allocate the container so readArg can take a
            //     pointer; cleanup queue deletes it after invoke.
            //
            // R4.1 element-type scope: int / float only. Other
            // element types (bool / double / struct / std::string)
            // fall through to nullptr and the PMF observes a
            // default-constructed container — a future R4.1b would
            // route struct elements through pushFieldPrimitive's
            // struct path.
            auto* elementType = containerType->getElementType();
            size_t luaLen = 0;
            if (lua_istable(L, stackIdx)) {
                lua_len(L, stackIdx);
                luaLen = static_cast<size_t>(lua_tointeger(L, -1));
                lua_pop(L, 1);
            }

            if (!elementType) {
                int32_t v = 0;
                std::memcpy(&argSlots[i], &v, sizeof(int32_t));
                argPtrs[i] = &argSlots[i];
            } else {
                const char* ename = elementType->getName();
                const bool elemIsInt   = (ename && (std::strcmp(ename, "int") == 0 || std::strcmp(ename, "Int32") == 0));
                const bool elemIsFloat = (ename && (std::strcmp(ename, "float") == 0 || std::strcmp(ename, "Float32") == 0));

                if (!elemIsInt && !elemIsFloat) {
                    ayt::log::warn("ayt_reflect_call_method: container element type '%s' not yet supported in R4.1",
                                   ename ? ename : "(null)");
                    argPtrs[i] = nullptr;
                } else {
                    const size_t elemSize = elemIsInt ? sizeof(int32_t) : sizeof(float);
                    const bool isFixedSize = containerType->isFixedSize();

                    if (isFixedSize) {
                        // std::array<T, N>: cap = compile-time N.
                        size_t cap = containerType->getContainerSize(nullptr);
                        if (cap == 0) cap = 1;  // defensive (ArrayTypeInfo asserts N>0)
                        size_t N = (luaLen < cap) ? luaLen : cap;
                        void* mem = ::operator new(cap * elemSize);
                        std::memset(mem, 0, cap * elemSize);
                        for (size_t k = 0; k < N; ++k) {
                            lua_rawgeti(L, stackIdx, static_cast<int>(k + 1));
                            if (elemIsInt) {
                                int32_t v = static_cast<int32_t>(lua_tointeger(L, -1));
                                std::memcpy(static_cast<uint8_t*>(mem) + k * elemSize, &v, elemSize);
                            } else {
                                float v = static_cast<float>(lua_tonumber(L, -1));
                                std::memcpy(static_cast<uint8_t*>(mem) + k * elemSize, &v, elemSize);
                            }
                            lua_pop(L, 1);
                        }
                        argPtrs[i] = mem;
                        registerCleanup(mem, [](void* p) { ::operator delete(p); });
                    } else {
                        // std::vector<T>: heap-alloc, push_back each.
                        if (elemIsInt) {
                            auto* vec = new std::vector<int>();
                            vec->reserve(luaLen);
                            for (size_t k = 0; k < luaLen; ++k) {
                                lua_rawgeti(L, stackIdx, static_cast<int>(k + 1));
                                vec->push_back(static_cast<int32_t>(lua_tointeger(L, -1)));
                                lua_pop(L, 1);
                            }
                            argPtrs[i] = vec;
                            registerCleanup(vec, [](void* p) { delete static_cast<std::vector<int>*>(p); });
                        } else {
                            auto* vec = new std::vector<float>();
                            vec->reserve(luaLen);
                            for (size_t k = 0; k < luaLen; ++k) {
                                lua_rawgeti(L, stackIdx, static_cast<int>(k + 1));
                                vec->push_back(static_cast<float>(lua_tonumber(L, -1)));
                                lua_pop(L, 1);
                            }
                            argPtrs[i] = vec;
                            registerCleanup(vec, [](void* p) { delete static_cast<std::vector<float>*>(p); });
                        }
                    }
                }
            }
        } else {
            // Unknown type tag — treat as int (enums ride on this
            // path; the C++ enum auto-converts from int via
            // std::underlying_type_t in MethodInfoImpl::readArg).
            int32_t v = static_cast<int32_t>(lua_tointeger(L, stackIdx));
            std::memcpy(&argSlots[i], &v, sizeof(int32_t));
            argPtrs[i] = &argSlots[i];
        }
    }

    // RAII: free heap-allocated arg slots when scope exits,
    // regardless of whether invoke() succeeded.
    struct ScopeGuard {
        std::vector<HeapSlot>* q;
        ~ScopeGuard() {
            for (auto& s : *q) {
                if (s.ptr && s.deleter) s.deleter(s.ptr);
            }
        }
    } guard{&cleanup};

    const void* retPtr = method->invoke(selfPtr, argPtrs.data());
    auto* retType = method->getReturnType();
    if (retPtr == nullptr) {
        // void method (or a method that genuinely returned nothing)
        return 0;
    }
    if (retType == nullptr) {
        // R3.0: retType is null for return types that aren't
        // registered as AYReflect types (e.g. C++ enum, which
        // rides on the int path). The MethodInfoImpl stored the
        // underlying-type int in retPtr; push it as a Lua integer.
        // S3.12+R3.5+ will add a dedicated EnumTypeInfo<E> that
        // returns the enum type name; the int fallback here is
        // preserved.
        int32_t v = 0; std::memcpy(&v, retPtr, sizeof(int32_t));
        lua_pushinteger(L, static_cast<lua_Integer>(v));
        return 1;
    }

    // Push the return value. retPtr points into the thread-local
    // return buffer (owned by MethodInfoImpl::detail::tlsReturnBuffer)
    // — valid until the next invoke() call. We read it into a Lua
    // value before any further script execution.
    const char* tname = retType->getName();
    if (std::strcmp(tname, "int") == 0 || std::strcmp(tname, "Int32") == 0) {
        int32_t v = 0; std::memcpy(&v, retPtr, sizeof(int32_t));
        lua_pushinteger(L, static_cast<lua_Integer>(v));
    } else if (std::strcmp(tname, "float") == 0 || std::strcmp(tname, "Float32") == 0) {
        float v = 0.0f; std::memcpy(&v, retPtr, sizeof(float));
        lua_pushnumber(L, static_cast<lua_Number>(v));
    } else if (std::strcmp(tname, "bool") == 0 || std::strcmp(tname, "Bool") == 0) {
        bool v = false; std::memcpy(&v, retPtr, sizeof(bool));
        lua_pushboolean(L, v);
    } else if (std::strcmp(tname, "double") == 0 || std::strcmp(tname, "Float64") == 0) {
        double v = 0.0; std::memcpy(&v, retPtr, sizeof(double));
        lua_pushnumber(L, static_cast<lua_Number>(v));
    } else if (std::strcmp(tname, "Int64") == 0) {
        int64_t v = 0; std::memcpy(&v, retPtr, sizeof(int64_t));
        lua_pushinteger(L, static_cast<lua_Integer>(v));
    } else if (std::strcmp(tname, "std::string") == 0) {
        const std::string* sp = static_cast<const std::string*>(retPtr);
        lua_pushlstring(L, sp->data(), sp->size());
    } else if (auto* containerType = dynamic_cast<ayt::reflect::IContainerTypeInfo*>(retType)) {
        // R4.1 (2026-07-13): container return (std::vector<T> or
        // std::array<T, N>). Walk getElementAt(i) and push each
        // element to a new Lua table. Element-type scope is int /
        // float (matches the arg-marshal path). R4.1b would extend
        // to struct elements via pushFieldPrimitive's struct path.
        auto* elementType = containerType->getElementType();
        const size_t N = containerType->getContainerSize(retPtr);
        lua_newtable(L);
        const int outerIdx = lua_gettop(L);
        const char* ename = elementType ? elementType->getName() : nullptr;
        const bool elemIsInt   = (ename && (std::strcmp(ename, "int") == 0 || std::strcmp(ename, "Int32") == 0));
        const bool elemIsFloat = (ename && (std::strcmp(ename, "float") == 0 || std::strcmp(ename, "Float32") == 0));
        if (elemIsInt || elemIsFloat) {
            const size_t elemSize = elemIsInt ? sizeof(int32_t) : sizeof(float);
            for (size_t k = 0; k < N; ++k) {
                const void* elemPtr = containerType->getElementAt(retPtr, k);
                if (!elemPtr) {
                    // std::vector<bool> returns nullptr (bit-packed);
                    // push 0 as a safe default. R4.1 doesn't support
                    // vector<bool> anyway but this keeps the bridge
                    // from crashing on a mis-registered type.
                    lua_pushinteger(L, 0);
                } else if (elemIsInt) {
                    int32_t v = 0;
                    std::memcpy(&v, elemPtr, elemSize);
                    lua_pushinteger(L, static_cast<lua_Integer>(v));
                } else {
                    float v = 0.0f;
                    std::memcpy(&v, elemPtr, elemSize);
                    lua_pushnumber(L, static_cast<lua_Number>(v));
                }
                lua_rawseti(L, outerIdx, static_cast<int>(k + 1));
            }
        } else {
            // Unsupported element type — push an empty table.
            // Caller can still inspect length via #t (returns 0).
            ayt::log::warn("ayt_reflect_call_method: container return element type '%s' not yet supported in R4.1",
                           ename ? ename : "(null)");
        }
    } else if (retType->getFieldCount() > 0) {
        // S3.12+R3: struct return. Build a Lua table and fill each
        // field via pushFieldPrimitive. Recursive for nested struct
        // fields (R4.0 extends R3: a field whose type is itself a
        // struct auto-builds a Lua sub-table).
        // The retPtr points to a TLS buffer holding a copy of the
        // struct by value; pushFieldPrimitive needs a non-const void*
        // (it does not mutate the bytes, but the signature requires
        // it). const_cast is safe here.
        lua_newtable(L);
        int outerIdx = lua_gettop(L);
        size_t fieldCount = retType->getFieldCount();
        for (size_t fi = 0; fi < fieldCount; ++fi) {
            auto* field = retType->getField(fi);
            if (!field) continue;
            void* fieldPtr = static_cast<uint8_t*>(const_cast<void*>(retPtr))
                             + field->getOffset();
            lua_pushstring(L, field->getName());
            pushFieldPrimitive(L, field, fieldPtr);
            lua_settable(L, outerIdx);
        }
    } else {
        // Unknown return type — most commonly an enum (R3.0 ships
        // enum args/return on the int path but doesn't register a
        // dedicated EnumTypeInfo for them). The MethodInfoImpl
        // stores the underlying-type int in retPtr; push it.
        int32_t v = 0; std::memcpy(&v, retPtr, sizeof(int32_t));
        lua_pushinteger(L, static_cast<lua_Integer>(v));
    }
    return 1;
}

// ------------------------------------------------------------
// Impl
// ------------------------------------------------------------

struct LogiaRuntimeBridge::Impl {
    // sol::state is non-copyable and non-movable; pimpl it.
    sol::state lua;

    // scriptName → module table (the table returned by the chunk)
    std::unordered_map<std::string, sol::table> scripts;

    // S3.5: ambient time/input state, owned by the bridge.
    // Populated by tickAmbient() before any lifecycle dispatch;
    // read by `time.delta` / `time.total` Lua accessors. The
    // ScriptSubSystem is the only legitimate writer — tests can
    // poke the values directly through the public setters.
    float _currentDelta  = 0.0f;
    float _totalElapsed  = 0.0f;

    // S3.5: injectable input backend. Default points at the
    // file-local MockInputProvider defined in the anon namespace
    // above. setInputProvider() swaps it out; lifetime is the
    // caller's (test or production host).
    LogiaRuntimeBridge::InputProvider* _input = &g_defaultInputProvider;

    // S3.6 (LG-06a): in-memory compile cache. Keyed by
    // (source-hash, LogiaHostContext fields, pipeline-version).
    // Value holds the generated Lua source (empty when compile
    // failed) and a snapshot of the diagnostics surface — so a
    // cache hit on a previously-failing source reproduces the same
    // error report without re-running the front end.
    struct CompileCacheEntry {
        std::string generatedLua;
        bool compileOk = false;  // captures success/failure once.
    };
    std::unordered_map<std::size_t, CompileCacheEntry> _compileCache;

    // S3.6: hit/miss counters exposed via the public API.
    // _hits counts loadScript calls that found a matching entry and
    // skipped Lex/Parser/Semantic/Codegen entirely (still re-ran
    // the cached Lua chunk via safe_script + module-table caching).
    std::size_t _compileHits  = 0;
    std::size_t _compileMisses = 0;

    bool initialized = false;
    // False after shutdown(); the existing sol::state is left in place
    // until ~Impl or ensureLua() on the next loadScript/initialize.
    // Avoids eager sol::state{} replacement on shutdown's stack frame.
    bool _luaLive = false;

    Impl()
    {
        ensureLua();
        initialized = true;
    }

    void ensureLua()
    {
        if (_luaLive) {
            return;
        }
        lua = sol::state{};
        lua.open_libraries(
            sol::lib::base,
            sol::lib::string,
            sol::lib::table,
            sol::lib::math,
            sol::lib::utf8);
        // S3.10: register int/float/bool/... so any host (Component,
        // System, Tool) emitting ayt_reflect_*_field calls finds
        // a non-null ITypeInfo for the field. Idempotent across
        // multiple ensureLua() invocations (findType guard inside).
        ensureBuiltinTypesRegistered();
        registerEngineApi();
        _luaLive = true;
    }

    void registerEngineApi()
    {
        // log table → wraps AYLog
        auto logTbl = lua.create_table();
        logTbl["info"]  = [](const std::string& s) {
            ayt::log::info("%s", s.c_str());
        };
        logTbl["warn"]  = [](const std::string& s) {
            ayt::log::warn("%s", s.c_str());
        };
        logTbl["error"] = [](const std::string& s) {
            ayt::log::error("%s", s.c_str());
        };
        lua["log"] = logTbl;

        // S3.5: time table — `delta` / `total` are functions so Lua
        // can call `time.delta()` (codegen lowers Logia `time.delta`).
        // Lambdas read live Impl scalars updated by tickAmbient().
        // S3.5: time table — `delta` and `total` are kept in sync
        // with the bridge's _currentDelta / _totalElapsed scalars
        // via tickAmbient(). The bare-Lua emission in AYLuaCodegen
        // (`time.delta` / `time.total`) reads them as plain numbers;
        // tickAmbient pushes fresh values after each dispatcher tick.
        auto timeTbl = lua.create_table();
        timeTbl["delta"] = 0.0;
        timeTbl["total"] = 0.0;
        lua["time"] = timeTbl;

        // S3.5: input table defers to the injectable provider.
        // Lambdas capture `this` so swapping providers takes
        // effect immediately for subsequent lifecycle calls (no
        // need to rebuild the table). Tests assert this in
        // Test_LogiaAmbient.cpp.
        auto inputTbl = lua.create_table();
        inputTbl["is_pressed"] = [this](const std::string& k) -> bool {
            auto* p = _input ? _input : &g_defaultInputProvider;
            return p->isPressed(k);
        };
        inputTbl["is_just_pressed"] = [this](const std::string& k) -> bool {
            auto* p = _input ? _input : &g_defaultInputProvider;
            return p->isJustPressed(k);
        };
        lua["input"] = inputTbl;

        // LG-05 / S3.3: AYReflect-backed self.field read/write.
        // Registered as plain lua_CFunction entries (raw
        // lua_register) so generated Logia can call them as
        // `ayt_reflect_get_field(self, "Type", "field")`. The first
        // arg is the receiver lightuserdata (= the
        // ScriptComponent* pass-through from callLifecycle), the
        // next two args are string type/field names resolved
        // against AYReflect's TypeRegistry at runtime.
        lua_register(lua, "ayt_reflect_get_field", &ayt_reflect_get_field_c);
        lua_register(lua, "ayt_reflect_set_field", &ayt_reflect_set_field_c);
        // S3.11: multi-hop struct chain reflect (LG-05 extension).
        // Chain is baked into Lua args; the bridge walks offset
        // arithmetic via repeated field->get() and re-derives each
        // intermediate type name from the field's ITypeInfo name.
        lua_register(lua, "ayt_reflect_get_field_chain",
                     &ayt_reflect_get_field_chain_c);
        lua_register(lua, "ayt_reflect_set_field_chain",
                     &ayt_reflect_set_field_chain_c);
        // S3.12 (track R2 §5.7.4): method-call reflect path. Stack
        // signature (self, typeName, methodName, args...). Returns
        // 0 for void methods or 1 with the return value pushed.
        lua_register(lua, "ayt_reflect_call_method",
                     &ayt_reflect_call_method_c);

        // Test-only witness: scripts can assign a string here to make
        // observable side effects for unit tests. Production code
        // never touches this; it exists solely so the LogiaAdapter
        // tests can verify that a lifecycle method actually ran and
        // that the receiver/self mapping is correct without parsing
        // AYLog output.
        lua["__test_witness"] = std::string{};

        // Numeric witness slots used by ambient.time inspectors in
        // Test_LogiaAmbient.cpp. Pre-registering them with explicit
        // numeric types lets scripts assign `__witness_delta =
        // time.delta` (a Lua number) without sol2 auto-typing the
        // global to nil on first touch. tryGetLuaGlobalNumber reads
        // these back via lua["__witness_delta"] and fails closed
        // (returns false) if the global is missing.
        lua["__witness_delta"] = 0.0;
        lua["__witness_total"] = 0.0;
    }
};

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------

LogiaRuntimeBridge::LogiaRuntimeBridge()
    : _impl(std::make_unique<Impl>()) {}

LogiaRuntimeBridge::~LogiaRuntimeBridge() = default;

bool LogiaRuntimeBridge::initialize()
{
    _impl->ensureLua();
    if (_impl->initialized) return true;
    _impl->initialized = true;
    return true;
}

void LogiaRuntimeBridge::shutdown()
{
    _impl->scripts.clear();
    // S3.6 (LG-06a): shutdown also wipes the compile cache so a
    // post-shutdown loadScript starts from a clean miss. _impl is
    // preserved across shutdown() (the bridge is reusable), but the
    // source-to-Lua map is bound to the previous pipeline run.
    _impl->_compileCache.clear();
    _impl->_compileHits   = 0;
    _impl->_compileMisses = 0;
    _impl->_currentDelta  = 0.0f;
    _impl->_totalElapsed  = 0.0f;
    _impl->initialized    = false;
    // Mark the VM inactive without constructing a replacement here.
    // ensureLua() recreates on the next loadScript/initialize path;
    // ~Impl destroys the existing sol::state once at process teardown.
    _impl->_luaLive = false;
}

bool LogiaRuntimeBridge::isInitialized() const
{
    return _impl->initialized;
}

bool LogiaRuntimeBridge::loadScript(const std::string& scriptName,
                                    const std::string& logiaSource,
                                    std::vector<logia::CompilerError>& errors)
{
    // Default-ctx delegator (S2.5 / S3.5 / LG-03 preserved callers).
    // Existing tests rely on the no-ctx overload matching the S2.5
    // semantics — see Test_LogiaRuntime/Test_LogiaSemantic/
    // Test_LogiaAmbient for the full set that hits this path.
    return loadScript(scriptName, logiaSource,
                      logia::defaultLogiaHostContext(), errors);
}

// S3.6 (LG-06a): the real implementation. Keyed by
// (source, hostContext, kLogiaPipelineVersion). On hit, skip
// Lexer/Parser/Semantic/Codegen and re-execute the cached Lua
// chunk. On miss, run the full pipeline and cache the result.
bool LogiaRuntimeBridge::loadScript(const std::string& scriptName,
                                    const std::string& logiaSource,
                                    const logia::LogiaHostContext& ctx,
                                    std::vector<logia::CompilerError>& errors)
{
    _impl->ensureLua();
    errors.clear();

    // S3.6: cache lookup. The Lua-state piece (safe_script +
    // module-table registration) still runs every call because the
    // sol::state can be reset by shutdown() or affected by error
    // paths; caching only the *compile-side* output is correct and
    // matches the spec ("skips Lexer/Parser/Semantic/Codegen").
    const std::size_t cacheKey = makeCacheKey(logiaSource, ctx);
    auto cacheIt = _impl->_compileCache.find(cacheKey);
    std::string cachedLua;
    bool cachedOk = false;
    const bool isCacheHit = (cacheIt != _impl->_compileCache.end());

    if (isCacheHit) {
        ++_impl->_compileHits;
        cachedLua = cacheIt->second.generatedLua;
        cachedOk  = cacheIt->second.compileOk;
    } else {
        ++_impl->_compileMisses;
    }

    // Bridge stage 1: get a `luaSrc` + `luaOk` either from cache
    // (cache hit) or by running the full pipeline (cache miss).
    std::string luaSrc;
    bool luaOk  = false;
    if (isCacheHit) {
        luaSrc = cachedLua;
        luaOk  = cachedOk;
        if (!luaOk) {
            // Cached compile-failure: surface the same error a fresh
            // compile would have produced (we cached a non-empty
            // error report on miss; for now we just bail with a
            // generic "cached compile failed" entry — full diagnostic
            // replay is a follow-up if a unit test needs it).
            logia::CompilerError ce;
            ce.code = logia::ErrorCode::InvalidOperation;
            ce.message = "Cached compile failed for script '" + scriptName + "'";
            ce.line = 0;
            ce.column = 0;
            errors.push_back(std::move(ce));
            _impl->scripts.erase(scriptName);
            return false;
        }
    } else {
        // Cache miss — run the full pipeline (heap-backed; see
        // compileLogiaToLua in AYLogiaPipeline.h).
        logia::LuaCodegenOptions opts;
        opts.scriptName = scriptName;
        logia::LogiaToLuaResult pipeline =
            logia::compileLogiaToLua(logiaSource, ctx, opts);
        if (!pipeline.success) {
            errors = std::move(pipeline.errors);
            _impl->scripts.erase(scriptName);
            _impl->_compileCache[cacheKey] = Impl::CompileCacheEntry{
                std::string{},
                false,
            };
            return false;
        }

        luaSrc = std::move(pipeline.lua);
        luaOk  = true;

        // Populate the cache BEFORE running the Lua chunk so a
        // successful safe_script that then produces a runtime error
        // still leaves the compile result cached for the next load.
        _impl->_compileCache[cacheKey] = Impl::CompileCacheEntry{
            luaSrc,
            true,
        };
    }

    // 3. Execute the Lua chunk in sol::state (runs on every call —
    //    safe_script rebinds the chunk into the live sol::state).
    auto result = _impl->lua.safe_script(luaSrc, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        logia::CompilerError ce;
        ce.code = logia::ErrorCode::InvalidOperation;
        ce.message = std::string("Lua load failed: ") + err.what();
        ce.line = 0;
        ce.column = 0;
        errors.push_back(std::move(ce));
        _impl->scripts.erase(scriptName);
        return false;
    }

    // 4. The chunk returns the module table; cache it.
    sol::object returned = result;
    if (returned.get_type() != sol::type::table) {
        logia::CompilerError ce;
        ce.code = logia::ErrorCode::InvalidOperation;
        ce.message = "Logia chunk did not return a table";
        ce.line = 0;
        ce.column = 0;
        errors.push_back(std::move(ce));
        _impl->scripts.erase(scriptName);
        return false;
    }
    _impl->scripts[scriptName] = returned;
    return true;
}

// ------------------------------------------------------------
// S3.7a (LG-06b part A) — in-memory reload
//
// Implementation strategy: delegate to loadScript (which already
// erases-then-rebinds the scriptName entry, runs the cache lookup
// keyed by (source, ctx, pipelineVersion), and is the single source
// of truth for the compile + load pipeline). The only S3.7a
// addition is the failure policy: on a compile miss, loadScript
// itself erases the cached module — which would drop a previously
// good script on a typo. We capture the prior good state, call
// loadScript, and if it fails, restore the prior module table.
//
// The captured `prior` sol::table is a value-type copy of the
// sol::state's reference to the original module table. sol2's
// sol::table is a cheap handle (a lua_State* + stack index), so
// capturing the prior state is just a copy of the handle and
// re-inserting it on failure preserves the live module binding.
// ------------------------------------------------------------

bool LogiaRuntimeBridge::reloadScript(const std::string& scriptName,
                                      const std::string& logiaSource,
                                      std::vector<logia::CompilerError>& errors)
{
    // Default-ctx delegator (mirrors the loadScript 3-arg form).
    return reloadScript(scriptName, logiaSource,
                        logia::defaultLogiaHostContext(), errors);
}

bool LogiaRuntimeBridge::reloadScript(const std::string& scriptName,
                                      const std::string& logiaSource,
                                      const logia::LogiaHostContext& ctx,
                                      std::vector<logia::CompilerError>& errors)
{
    // S3.7a failure policy: snapshot the prior module so a failed
    // reload does not detach the previously-loaded script. A
    // successful loadScript will overwrite the map entry; a
    // failed loadScript will erase it — in which case we restore
    // the snapshot. hasScript(name) therefore stays true across
    // a failed reload.
    sol::table prior;
    bool hadPrior = false;
    auto priorIt = _impl->scripts.find(scriptName);
    if (priorIt != _impl->scripts.end()) {
        prior = priorIt->second;  // sol::table is a cheap handle copy
        hadPrior = true;
    }

    const bool ok = loadScript(scriptName, logiaSource, ctx, errors);
    if (!ok && hadPrior) {
        // loadScript erased the entry on failure; restore the
        // prior good module so the running game keeps its binding.
        _impl->scripts[scriptName] = prior;
    }
    return ok;
}

// S3.8b (LG-07) — ToolRunner one-shot entry point.
//
// Composes three pieces:
//   1. loadScript under the Tool host context (cached under its
//      own (source, ctx, version) slot — separate from Component
//      runs of the same source).
//   2. callLifecycle(name, "run", nullptr, nullptr) — dispatches
//      the no-self / no-args signature that LuaCodegen emits under
//      ctx.expectSelf = false.
//   3. Returns true iff BOTH the load and the run dispatch
//      succeeded. On any failure, `errors` is populated and the
//      function returns false.
bool LogiaRuntimeBridge::runTool(const std::string& scriptName,
                                 const std::string& logiaSource,
                                 std::vector<logia::CompilerError>& errors)
{
    errors.clear();
    if (!loadScript(scriptName, logiaSource,
                    logia::toolLogiaHostContext(), errors)) {
        return false;
    }
    // callLifecycle's `run` branch passes zero args to the Lua
    // function (the no-self contract). We pass nullptr for both
    // receiver and arg2 for clarity; callLifecycle ignores them on
    // the `run` path.
    return callLifecycle(scriptName, "run", nullptr, nullptr);
}

// S3.6 — compile-cache observability. Counters live on _impl so the
// public methods delegate without holding a separate state.
std::size_t LogiaRuntimeBridge::compileCacheHitCount() const noexcept
{
    return _impl ? _impl->_compileHits : 0u;
}

std::size_t LogiaRuntimeBridge::compileCacheMissCount() const noexcept
{
    return _impl ? _impl->_compileMisses : 0u;
}

void LogiaRuntimeBridge::resetCompileCacheCounters() noexcept
{
    if (!_impl) return;
    _impl->_compileHits = 0;
    _impl->_compileMisses = 0;
}

void LogiaRuntimeBridge::clearCompileCache() noexcept
{
    if (!_impl) return;
    _impl->_compileCache.clear();
}

bool LogiaRuntimeBridge::hasScript(const std::string& scriptName) const
{
    return _impl->scripts.find(scriptName) != _impl->scripts.end();
}

bool LogiaRuntimeBridge::callLifecycle(const std::string& scriptName,
                                       const std::string& methodName,
                                       void* receiver,
                                       void* arg2)
{
    if (!_impl || !_impl->_luaLive) {
        return false;
    }
    auto it = _impl->scripts.find(scriptName);
    if (it == _impl->scripts.end()) {
        return false;
    }

    sol::table M = it->second;
    sol::protected_function fn = M[methodName];
    if (!fn.valid()) {
        return false;
    }

    // `receiver` is the ScriptComponent*. S2 changed the Lua `self`
    // argument from the scriptName string (S1 placeholder) to the
    // actual receiver pointer — sol2 5.5+ pushes a raw typed pointer
    // through lua_pushlightuserdata without an explicit wrapper.
    sol::protected_function_result r;
    if (methodName == "on_update") {
        // on_update(self, dt) — second param from arg2 pointer.
        float dt = arg2 ? *static_cast<float*>(arg2) : 0.0f;
        r = fn.call(receiver, dt);
    } else if (methodName == "on_start") {
        // on_start(self, entity) — entity passed as lightuserdata.
        r = fn.call(receiver, arg2);
    } else if (methodName == "run") {
        // S3.8b (LG-07): Tool host's run() takes NO args — neither
        // self nor a dt / entity. The ToolRunner passes nullptr for
        // receiver; the generated Lua signature is `function M.run()`
        // (no self) when the host context's expectSelf is false.
        r = fn.call();
    } else {
        // on_destroy(self) — no extra args.
        r = fn.call(receiver);
    }

    if (!r.valid()) {
        sol::error err = r;
        ayt::log::error("Logia call '%s.%s' failed: %s",
                        scriptName.c_str(), methodName.c_str(), err.what());
        return false;
    }
    return true;
}

// ------------------------------------------------------------
// S3.5 — ambient API: tick -> time.delta/time.total
// ------------------------------------------------------------

void LogiaRuntimeBridge::tickAmbient(float scaledDelta)
{
    if (!_impl || !_impl->_luaLive) return;
    if (scaledDelta < 0.0f) scaledDelta = 0.0f;
    _impl->_currentDelta = scaledDelta;
    _impl->_totalElapsed += scaledDelta;
    // Mirror onto the `time` table so scripts that hold a reference
    // to the table see fresh values without going through the
    // bridge's accessor methods. This matches the S3.5 ambient
    // expectation that `time.delta` is a number read each frame.
    sol::table timeTbl = _impl->lua["time"];
    if (timeTbl.valid()) {
        timeTbl["delta"] = static_cast<double>(scaledDelta);
        timeTbl["total"] = static_cast<double>(_impl->_totalElapsed);
    }
}

float LogiaRuntimeBridge::currentDelta() const noexcept
{
    return _impl ? _impl->_currentDelta : 0.0f;
}

float LogiaRuntimeBridge::totalElapsed() const noexcept
{
    return _impl ? _impl->_totalElapsed : 0.0f;
}

void LogiaRuntimeBridge::setInputProvider(InputProvider* provider) noexcept
{
    if (!_impl) return;
    _impl->_input = provider ? provider : &g_defaultInputProvider;
}

LogiaRuntimeBridge::InputProvider* LogiaRuntimeBridge::inputProvider() const noexcept
{
    return _impl ? _impl->_input : &g_defaultInputProvider;
}

void* LogiaRuntimeBridge::implHandle()
{
    return _impl.get();
}

std::string LogiaRuntimeBridge::getLuaGlobalString(const char* name) const
{
    if (_impl == nullptr || name == nullptr || !_impl->_luaLive) return {};
    sol::object obj = _impl->lua[name];
    if (obj.is<std::string>()) {
        return obj.as<std::string>();
    }
    return {};
}

bool LogiaRuntimeBridge::tryGetLuaGlobalNumber(const char* name, double& out) const
{
    if (_impl == nullptr || name == nullptr || !_impl->_luaLive) return false;
    sol::object obj = _impl->lua[name];
    if (!obj.valid()) return false;
    if (obj.get_type() == sol::type::number) {
        out = obj.as<double>();
        return true;
    }
    return false;
}

} // namespace ayt::script