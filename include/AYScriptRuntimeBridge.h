#pragma once
// AYScriptRuntimeBridge.h - sol2-backed runtime for compiled Logia (S1+)

#include <memory>
#include <string>
#include <vector>

#include "logia/AYCompilerError.h"
#include "logia/AYLogia.h"  // S3.6: LogiaHostContext for loadScript overload + cache key

namespace ayt::script
{

// S3.6 (LG-06a) — compile pipeline version stamp.
//
// Bump this constant whenever anything that affects the generated Lua
// output shape or the loadScript error surface changes — Lexer/Parser,
// SemanticAnalyzer diagnostics (Warning/Error code numbers or hint
// wording), LuaCodegen emission (snake_case method names, time.*,
// input.*, ayt_reflect_*_field). A bump invalidates every existing
// in-memory compile cache entry.
//
// The stamp is intentionally a raw constant (not a date string) so
// it's cheap to fold into the cache key hash.
//
// History:
//   1 — S3.6 baseline (LG-06a compile cache).
//   2 — S3.8b (LG-07): added `run` lifecycle keyword +
//       Tool host policy + Tool-kind codegen branch
//       (`function M.run()` without `self` when
//       ctx.expectSelf == false).
//   3 — S3.11: multi-hop struct-chain reflect
//       (`ayt_reflect_get_field_chain` / `set_field_chain`)
//       + FVector3 fields registered in AYReflect.
//   4 — S3.12 (track R2 §5.7.4): IMethodInfo + self.method(args)
//       reflect call path. Adds `ayt_reflect_call_method` C entry
//       and stamps CallExpr::resolvedMethod. MethodInfoImpl lives
//       in AYScript-private `logia/AYMethodInfoImpl.h` to keep the
//       variadic pack out of the foundation TU.
//   5 — S3.12+R3 (track R2 §5.7.4): extend self.method(args) to
//       support struct args (const T& / const T*), struct return,
//       enum args/return, std::string args/return. MethodInfoImpl
//       readArg / invokeImpl extended with a uniform slot convention
//       (args[i] = pointer to marshalled value; readArg dereferences
//       based on PMF parameter type). Bridge marshals Lua table → T
//       field-by-field and T → Lua table for return. Enum rides on
//       the int path with std::underlying_type_t cast in readArg.
//       Limitations: no nested struct fields, no table-literal in
//       arg position (require `local`), no std::vector/array args
//       (track R4). See design.md §5.7.4 R3.
//       2026-07-11 audit fix: bump 5 → 6. Codegen output shape
//       changes for script-block `function NAME(...)` helpers, and
//       SemanticAnalyzer may emit a new `LuaKeywordLeak` warning.
//       Both invalidate the compile cache.
//       2026-07-13 R5.0: bump 6 → 7. Codegen output shape changes
//       for `while (cond) { body }` and `for (var i : N) { body }`.
//       Both lower to Lua control-flow forms that were previously
//       unavailable from Logia source. No new diagnostic surface —
//       the only cache-invalidation trigger is the codegen shape
//       change.
//       2026-07-13 R5.0.1: bump 7 → 8. Three surface additions:
//       (1) `if cond { ... }` (bare form, no parens) accepted
//       alongside `if (cond) { ... }`; (2) same for `while cond` /
//       `for var i : N`; (3) `for (var i : start, end) { body }`
//       half-open range form emits `for i = start, (end) - 1 do`.
//       Codegen output shape changes only for the new range form.
//       2026-07-13 R5.1: bump 8 → 9. `break` and `continue` are
//       first-class keywords inside loop bodies. Parser enforces
//       "must be inside a loop" via `Parser::_loopDepth` gate; any
//       bare `break` / `continue` outside a loop is a hard error.
//       Codegen emits Lua 5.2+ native `break` / `continue` (no
//       runtime helper, no goto-juggling).
//       2026-07-13 R4.1: bump 9 → 10. Bridge runtime marshal path
//       extended for std::vector<T> / std::array<T, N> args and
//       returns (T = int / float). AYReflect adds ArrayTypeInfo +
//       IContainerTypeInfo::isFixedSize() virtual. Logia source
//       syntax unchanged (TableExpr was already in place from R3) —
//       the cache invalidation is the bridge code path itself, not
//       codegen emit shape.
//       2026-07-13 R4.2: bump 10 → 11. Bridge runtime extended for
//       T& / T* out-param (write-back) on int / float / struct.
//       AYReflect adds IMethodInfo::getParamIsOut() virtual (default
//       false); MethodInfoImpl/MethodInfoImplConst set true for
//       non-const T& / T* in the PMF signature. Logia source
//       unchanged — pattern is `self.fillInt(x)` where C++ writes
//       to self.foo and the script reads back. Compile cache
//       invalidated because bridge code path changed.
//       2026-07-13 R5.2-A: bump 15 → 16. New Logia surface:
//       `do { <stmts> } end` block-scope statement. Mirrors Lua's
//       `do ... end` exactly (Logia-side braces around body, plus
//       explicit `end` to close). Reuses new `do` + `end` keyword
//       tokens (TokenType::Do / TokenType::End). Codegen emits
//       Lua's native `do ... end` via LuaCodegen::emitBlockStmt.
//       Slice B (`break :L`, `continue :L`, `::L::` label decl) and
//       Slice C (bound type validation) are separate commits. Compile
//       cache invalidated because codegen output shape changed for
//       the new construct.
//       2026-07-13 R4.2b: bump 14 → 15. Bridge runtime out-param
//       marshal extended for `std::string` (T& / T* in PMF
//       signature). Heap-allocates a fresh std::string per call
//       (matches R3.0 std::string input pattern at runtime bridge
//       L737-744 — fixed-size std::string, no placement-new needed).
//       Seeds from lua_tostring if the user passed a string
//       (input + output idiom, mirrors R4.2 struct out-param at
//       L1042-1066); default-initialises to "" otherwise. The PMF
//       writes through readOutArg's std::string& / std::string*
//       overloads (AYMethodInfoImpl.h:303-329 / 589-615 — both
//       already work with std::string once a heap std::string* is
//       stored in the slot). After invoke, the bridge pushes the
//       new std::string contents via lua_pushlstring + lua_replace
//       on the original arg slot (local Lua variable still points
//       to pre-invoke value per Lua semantics; Logia users read
//       self.* via the R4.2 side-effect idiom). No AYReflect ABI
//       change, no MethodInfoImpl change — readOutArg already
//       supported std::string once a heap std::string* was
//       supplied. Logia source unchanged. Compile cache invalidated
//       because bridge code path changed.
//       2026-07-13 R4.1d: bump 13 → 14. Bridge runtime container
//       arg-marshal lifted from "trivially-destructible T only" to
//       any struct T whose fields are reflect-supported primitives.
//       vector<MyStruct> now allocates via ITypeInfo::create()
//       (delegates to VectorTypeInfo<T>::create() = `new
//       std::vector<T>()`) instead of the R4.1b vector<uint8_t>
//       reinterpret. Element-fill walks IContainerTypeInfo::resize
//       + getElementAt for typed access. Cleanup runs the typed
//       IContainerTypeInfo::destroy (= `delete vector<T>`) so each
//       element's dtor runs. array<MyStruct,N> gets the same
//       treatment. Return-side vector<MyStruct> / array<MyStruct,N>
//       was already safe (getElementAt returns &vec[k] of the real
//       typed vector). Logia source unchanged. Compile cache
//       invalidated because bridge code path changed. No AYReflect
//       ABI change — reuses the existing ITypeInfo::create() /
//       destroy() / IContainerTypeInfo::resize / getElementAt
//       vtable entries.
//       2026-07-13 R4.1c: bump 12 → 13. Bridge runtime container
//       arg-marshal extended for `std::array<std::string, N>`:
//       placement-new N strings on a heap block, fill from Lua
//       via lua_tostring, explicit per-element ~std::string()
//       walk on cleanup. (Return-side array<std::string,N> was
//       already covered by R4.1b's elemIsString return path —
//       ArrayTypeInfo::getElementAt hands back std::string* for
//       any in-bounds k, identical to vector.) Logia source
//       unchanged. Compile cache invalidated because bridge
//       code path changed.
//       2026-07-13 R4.1b: bump 11 → 12. Bridge runtime container
//       arg/return marshal extended for struct / std::string
//       elements: `std::vector<MyStruct>` / `std::array<MyStruct,N>`
//       (raw-byte buffer + per-element storeFieldPrimitive, with
//       the trivially-destructible T restriction); `std::vector<std::string>`
//       (push_back each lua_tostring); container return builds a Lua
//       sub-table per struct element via pushFieldPrimitive or pushes
//       each std::string via lua_pushlstring. Logia source unchanged.
//       Compile cache invalidated because bridge code path changed.
//       2026-07-14 R5.2-B: bump 16 → 17. New Logia surface:
//       `::LABEL::` label declaration (loop-scoped, hard error on
//       duplicate in same loop), `break :L` / `continue :L` for
//       jumping to an outer loop. Codegen emits Lua 5.2+ `goto L`
//       and `::L::` (Lua 5.5 runtime bridge, native support).
//       Analyzer adds `_labelStack` (vector<unordered_set<string>>)
//       pushed in analyzeWhileStmt / analyzeForStmt / analyzeLifecycle.
//       Lexer adds 2-char peek in `case ':'` for the `::` token
//       (single-`:` colon usage preserved). New AST nodes:
//       LabelDeclStmt; BreakStmt/ContinueStmt gain `std::string label`
//       field. R5.2-C (bound type validation) is a separate commit.
//       Compile cache invalidated because codegen output shape changed.
//       2026-07-14 R5.2-C: bump 17 → 18. New Logia validation:
//       `for (var i : N)` / `for (var i : start, end)` bound
//       expressions must statically reduce to int. Anything else
//       (float/string/bool literals; non-int-typed identifiers,
//       self.<field>, or self.<method>() returns; BinaryExpr with
//       any non-int-literal operand) emits ErrorCode::TypeMismatch
//       as a hard error and the compile fails. Static-only — no
//       runtime defense added. Step argument (`for (var i :
//       start, end, step)`) remains out of scope; parser still
//       only accepts two expressions in the for-header. Codegen
//       unchanged — by the time emitForStmt runs, every bound
//       has been statically validated. Compile cache invalidated
//       because the analyzer now hard-errors on previously-silent
//       bad-bound cases (e.g. `for (var i : 3.14)` would have
//       cached OK at v17 and now refuses to compile).
constexpr std::size_t kLogiaPipelineVersion = 18u;

// S3.6 — fold LogiaHostContext fields into the compile cache key so
// that the same source compiled under different host kinds (Component
// vs System vs Tool) does not collide on the cache. `hostType` is a
// pointer in the S3.0 API; we mix its address in rather than the
// underlying type name because the cache key is value-level only.
// Future S3.x may rekey on type id if two callers end up with the
// same name but different ITypeInfo*.
std::size_t hashLogiaHostContext(const logia::LogiaHostContext& ctx);

// LogiaRuntimeBridge
//
// Owns a sol::state, registers a minimal engine API (log.*, input.*),
// compiles Logia sources into Lua chunks, and invokes lifecycle methods
// (on_start / on_update / on_destroy) on demand.
//
// S1 deliberately does NOT inherit ayt::entity::IScriptBridge — the
// signatures do not match (Logia uses snake_case methods; AYEntity's
// stub uses camelCase). S2/S3 will add a thin adapter.
class LogiaRuntimeBridge {
public:
    LogiaRuntimeBridge();
    ~LogiaRuntimeBridge();

    LogiaRuntimeBridge(const LogiaRuntimeBridge&) = delete;
    LogiaRuntimeBridge& operator=(const LogiaRuntimeBridge&) = delete;

    // Build the sol::state and register the engine API. Safe to call once.
    bool initialize();
    void shutdown();
    [[nodiscard]] bool isInitialized() const;

    // Compile and load a Logia source string under a logical script name.
    // The compiled module table is cached by scriptName so subsequent
    // loadScript calls with the same name replace the cache entry.
    //
    // Returns true on success. On failure, `errors` is populated with the
    // Logia compiler errors and `hasScript(name)` returns false.
    //
    // S3.6 (LG-06a): when the cached Lua source for
    // (source, hostContext, pipelineVersion) matches, this skips
    // Lexer/Parser/Semantic/Codegen and re-executes the cached Lua
    // chunk (Lua state must be re-bound after sol::state lifecycle
    // changes; the chunk itself is text and survives). The host
    // context is the bridge's default (LogiaHostKind::Component) — for
    // non-default contexts use the overload below.
    bool loadScript(const std::string& scriptName,
                    const std::string& logiaSource,
                    std::vector<logia::CompilerError>& errors);

    // S3.6 (LG-06a) — host-context-explicit overload. Production
    // hosts that bind to S3.1+ script kinds (System / Tool /
    // EventHandler) pass their LogiaHostContext here so the cached
    // Lua source is keyed by the host kind plus strictInheritance
    // bit. Same return contract as the 3-arg overload.
    bool loadScript(const std::string& scriptName,
                    const std::string& logiaSource,
                    const logia::LogiaHostContext& ctx,
                    std::vector<logia::CompilerError>& errors);

    // S3.7a (LG-06b part A) — in-memory reload.
    //
    // Replaces the cached module table for `scriptName` with a fresh
    // compile of `logiaSource` under the same host-context semantics
    // as loadScript(). The compile cache is *not* wiped — the
    // (source, ctx, pipelineVersion) key is recomputed, so a reload
    // with the same source hits the same cache entry (no front-end
    // re-run). A reload with a *different* source (the typical
    // hot-reload case) misses the cache and runs the full pipeline,
    // stamping a new entry; the prior source's entry remains in the
    // cache for the next call to revert.
    //
    // Failure policy (locked): if the new source fails to compile,
    // `errors` is populated, `hasScript(name)` STAYS true (the prior
    // good module remains bound), and the function returns false.
    // This matches the "never break the running game" expectation
    // of an editor hot-reload: a typo in the .logia file surfaces a
    // diagnostic to the user but does not detach the component.
    //
    // S3.7a is pure-memory: callers supply the new source string.
    // S3.7b will add `reloadScriptFromFile(path)` that reads from
    // disk and a FileWatcher-driven coordinator in ScriptSubSystem.
    bool reloadScript(const std::string& scriptName,
                      const std::string& logiaSource,
                      std::vector<logia::CompilerError>& errors);

    // S3.7a — host-context-explicit reload overload. Same semantics
    // as the 3-arg version, with the same host-context-keyed cache
    // behavior as the S3.6 4-arg loadScript.
    bool reloadScript(const std::string& scriptName,
                      const std::string& logiaSource,
                      const logia::LogiaHostContext& ctx,
                      std::vector<logia::CompilerError>& errors);

    // S3.8b (LG-07) — ToolRunner one-shot entry point.
    //
    // Convenience wrapper used by editor / CLI hosts that want to
    // "run a tool" without spelling out the Tool host context + the
    // `run()` lifecycle call. Compiles `logiaSource` under
    // `toolLogiaHostContext()` (kind=Tool, expectSelf=false), loads
    // it, and invokes the `run()` method exactly once. No receiver
    // is passed (Tool scripts don't bind to a host instance).
    //
    // Failure policy: a compile error returns false and leaves
    // `errors` populated; `hasScript(name)` reflects whatever the
    // load step produced (false on compile failure, true on success).
    // The `run()` call's own failure (missing method, Lua runtime
    // error) is also returned as false; the bridge logs the Lua
    // error per the S1 contract.
    //
    // Repeat calls reuse the S3.6 compile cache when the source +
    // pipeline version are unchanged — a Tool invocation is cheap
    // after the first.
    bool runTool(const std::string& scriptName,
                 const std::string& logiaSource,
                 std::vector<logia::CompilerError>& errors);

    // === S3.6 (LG-06a) — compile cache observability ===

    // Number of loadScript calls that hit the in-memory compile cache
    // (skipped Lexer/Parser/Semantic/Codegen and used the cached Lua
    // source). Reset by shutdown() / clearCompileCache(). Exposed so
    // unit tests can assert cache-hit behavior without reaching into
    // private state.
    [[nodiscard]] std::size_t compileCacheHitCount() const noexcept;

    // Number of loadScript calls that missed the cache (full pipeline
    // ran; result cached for next time).
    [[nodiscard]] std::size_t compileCacheMissCount() const noexcept;

    // Reset all per-instance counters. Entries themselves stay — useful
    // for distinguishing "cached source never ran" from "ran N times".
    void resetCompileCacheCounters() noexcept;

    // Drop every cached compilation. Module tables in `_scripts` are
    // preserved (the bridge's runtime contract: hasScript stays true
    // for whatever is already loaded). Called by shutdown().
    void clearCompileCache() noexcept;

    // Look up a previously-loaded script.
    [[nodiscard]] bool hasScript(const std::string& scriptName) const;

    // Invoke a lifecycle method on the named script.
    //
    //   methodName must be "on_start" / "on_update" / "on_destroy" —
    //   matching the names emitted by LuaCodegen.
    //
    //   receiver — pointer to the ScriptComponent instance. Passed to
    //   Lua as lightuserdata so the script's `self` parameter receives
    //   the actual component (was: a placeholder scriptName string in
    //   S1). Pass nullptr in tests that don't care about the receiver.
    //   Typed as void* so AYScript doesn't need to include
    //   AYScriptComponent.h (which has a static-init side effect that
    //   requires World to be fully defined). The adapter casts.
    //
    //   arg2 — for "on_start", points to the owning Entity*; for
    //   "on_update", points to a float (deltaTime); for "on_destroy",
    //   ignored. May be nullptr to pass nil.
    //
    // Returns false if the script/method is missing or the Lua call raised
    // a Lua error (which is then logged).
    bool callLifecycle(const std::string& scriptName,
                       const std::string& methodName,
                       void* receiver = nullptr,
                       void* arg2 = nullptr);

    // === S3.5 — ambient API real-time bindings (was: mock in S1) ===

    // Plumb the per-tick scaled delta (and accumulate elapsed time)
    // before any lifecycle call that should observe them.
    // ScriptSubSystem::update/fixedUpdate must call this so the
    // `time.delta` / `time.total` ambient table reads from the
    // tick's authoritative source rather than a global mock.
    void tickAmbient(float scaledDelta);

    // Current scaled delta in seconds. Reflects the most recent
    // tickAmbient() call. Reads are cheap (plain float load).
    [[nodiscard]] float currentDelta() const noexcept;
    // Accumulated scaled time across all tickAmbient() calls.
    [[nodiscard]] float totalElapsed() const noexcept;

    // Injectable input backend. S3.5 ships a default mock so the
    // unittests can run without an HWND — but production hosts can
    // plug in a real AYDevice / OS-level poll by registering a
    // different provider. Provider ownership: bridge holds a raw
    // pointer; lifetime is the caller's responsibility.
    class InputProvider {
    public:
        virtual ~InputProvider() = default;
        virtual bool isPressed(const std::string& key) const = 0;
        virtual bool isJustPressed(const std::string& key) const = 0;
    };
    void setInputProvider(InputProvider* provider) noexcept;
    [[nodiscard]] InputProvider* inputProvider() const noexcept;

    // Internal access for tests.
    void* implHandle();

    // Test hook: read a top-level Lua string global by name. Returns
    // empty string if the global is missing or not a string. The
    // implementation forwards to the underlying sol::state. Tests
    // use this to verify that a Lua script wrote to a known global
    // (e.g. `__test_witness`).
    std::string getLuaGlobalString(const char* name) const;

    // Test hook: read a top-level Lua number global. Returns false if
    // missing or not numeric.
    bool tryGetLuaGlobalNumber(const char* name, double& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::script