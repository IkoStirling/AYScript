#pragma once
// Logia compiler entry: host-aware parsing and semantic validation.
//
// S3.0 (LG-03): `LogiaHostContext` is the public handle that tells the
// compiler which C++ host kind a `script Foo { ... }` is being bound to
// at load time. The Logia surface syntax is unchanged across host kinds;
// the context selects validation, exposed capabilities and receiver emission.
//
// S2.5 callers (and existing tests) can ignore `LogiaHostContext`
// entirely — the no-arg `Compiler::compile(source)` overload still
// constructs an implicit Component-host context (matching the S2.5
// semantics described in design.md §1.6 / §5.6).

#include "AYScript/logia/Ast.h"
#include "AYScript/logia/CompilerError.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace ayt::reflect
{
class ITypeInfo;
}

namespace ayt::script::logia
{

// What kind of C++ host a `script Name { ... }` is being bound to.
// Each host kind has runtime dispatch; checked contexts validate its lifecycle.
enum class LogiaHostKind {
    Component,    // S2.5 / LG-03 — ScriptComponent on Entity
    System,       // S3.1 (LG-04) — ISystem tick
    Tool,         // S3+    — editor/CLI one-shot
    EventHandler, // S3+    — event-bus callback object
    Actor,        // Actor class — per-instance Lua self table, no C++ host type
};

// Caller-supplied description of the C++ host a `script` block binds to.
//
// Legacy contexts preserve their historical validation. checked contexts
// validate scopes, types, receiver schema and declared host signatures.
//
// S3.2 (LG-04b, B-min): when `kind == Component` AND `hostType != nullptr`
// AND `strictInheritance == true`, the analyzer will hard-reject any
// `script Name` whose Reflect entry is found but `!isDerivedFrom(Name,
// hostType)`. The B-min scope is single-chain parent walks only — no
// full derived-type graph (see design.md §5.6 S3.2).
struct HostFunctionSignature {
    std::vector<std::string> parameters;
    std::string result = "void";
    bool operator==(const HostFunctionSignature&) const = default;
};

struct LogiaHostContext {
    LogiaHostKind kind = LogiaHostKind::Component;
    // Expected base type for `script Name` binding. nullptr disables
    // the subclass check (LG-03 / S3.1). When non-null AND
    // strictInheritance is true AND kind is Component, the analyzer
    // will hard-reject scripts that are not derived from this type.
    const ayt::reflect::ITypeInfo* hostType = nullptr;
    // Whether the host dispatch will pass a `self` lightuserdata.
    // S2.5 / LG-03 always true (ScriptComponent*). S3.1+ Tool hosts
    // pass false to allow scripts that omit `self.field` access.
    bool expectSelf = true;
    // S3.2 (LG-04b) B-min: when true, hard-reject script names whose
    // Reflect type is not derived from `hostType` (Component host only).
    // Default false to preserve S2.5 / LG-03 / LG-04 behavior.
    bool strictInheritance = false;
    /// Enable the production type/scope contract. Legacy callers retain their
    /// previous permissive semantics until explicitly migrated.
    bool strictSemantics = false;
    /// Actor property schema supplied by the resolved .act asset, never inferred
    /// from arbitrary assignments. Keys use author-visible names.
    std::unordered_map<std::string, std::string> actorProperties;
    /// Explicit host facade signatures; the runtime adds registered providers.
    std::unordered_map<std::string, HostFunctionSignature> hostFunctions;
    /// File-backed compilation resolves imports within this authoring root.
    /// Empty root defaults to sourcePath's directory; in-memory imports fail.
    std::string sourcePath;
    std::string assetRoot;
};

/// Recommended context for newly authored scripts; rejects implicit globals,
/// incompatible values, invalid callback signatures and escaping local names.
inline LogiaHostContext checkedLogiaHostContext(LogiaHostContext ctx = {})
{
    ctx.strictSemantics = true;
    return ctx;
}

// S2.5 / LG-03 default context: Component host, no subclass check, with self.
inline LogiaHostContext defaultLogiaHostContext()
{
    return LogiaHostContext{
        LogiaHostKind::Component,
        nullptr,
        true,
        false,
    };
}

/// Actor class host: `self` is an instance table backed by ECS state.
inline LogiaHostContext actorLogiaHostContext()
{
    return LogiaHostContext{LogiaHostKind::Actor, nullptr, true, false};
}

// S3.8b: Tool host context — one-shot editor / CLI scripts.
//   kind = Tool          → lifecycle whitelist is `run()` only;
//                          on_start / on_update / on_destroy are soft
//                          warnings (mirrors the S3.1 System-on_destroy
//                          policy).
//   expectSelf = false   → ToolRunner does not pass a receiver;
//                          codegen omits the `self` parameter and the
//                          semantic analyzer skips injecting `self`
//                          into the script's scope.
//   hostType = nullptr   → no subclass check (Tool hosts don't bind
//                          against a Reflect type).
//   strictInheritance = false — same reason.
inline LogiaHostContext toolLogiaHostContext()
{
    return LogiaHostContext{
        LogiaHostKind::Tool,
        nullptr,
        false,
        false,
    };
}

// INT-04b: EventHandler host — pure EventBus script, no Component tick.
//   kind = EventHandler  → Component lifecycle (on_start/update/destroy)
//                          soft-warns; `run()` is the one-shot bind entry
//                          (typically event.subscribe wiring).
//   expectSelf = false   → no ScriptComponent* receiver; codegen omits self.
//   hostType / strictInheritance unused (no Reflect bind).
inline LogiaHostContext eventHandlerLogiaHostContext()
{
    return LogiaHostContext{
        LogiaHostKind::EventHandler,
        nullptr,
        false,
        false,
    };
}

struct CompileOptions {
    std::string fileName;
    bool useCompileTimeTypes = false;
};

struct CompileResult {
    bool success = false;
    std::vector<CompilerError> errors;          // legacy (S1) — parser + semantic errors
    std::vector<LogiaDiagnostic> diagnostics;   // S2 — full diagnostics w/ severity
    std::unique_ptr<Program> program;
    std::vector<std::string> dependencies;
};

void tokenize(const std::string& source, std::vector<Token>& out);

class Compiler {
public:
    Compiler() = default;
    explicit Compiler(CompileOptions options);

    // Compile-only entry points. When you also need Lua source, call
    // compileLogiaToLua() (AYScript/logia/LogiaPipeline.h) instead of stacking
    // Compiler + LuaCodegen in the same frame.

    // S2.5 entry — equivalent to `compile(source, defaultLogiaHostContext())`.
    // Preserved verbatim so the 169 existing tests do not need editing.
    CompileResult compile(const std::string& source);

    /// Validate against the selected host/language contract. Imports resolve
    /// from sourcePath within assetRoot; entry text may be unsaved in memory.
    CompileResult compile(const std::string& source, const LogiaHostContext& ctx);

    // Read-only access to the last context the compiler was invoked
    // with. Defaults to `defaultLogiaHostContext()` if `compile` has
    // never been called. Exposed for unit tests; engine code should
    // treat the host context as a per-call input, not compiler state.
    const LogiaHostContext& lastHostContext() const { return _lastCtx; }

private:
    CompileOptions _options;
    LogiaHostContext _lastCtx = defaultLogiaHostContext();
};

} // namespace ayt::script::logia
