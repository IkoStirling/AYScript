#pragma once
// AYLogia.h - Logia compiler entry (S0: tokenize + parse)
//
// S3.0 (LG-03): `LogiaHostContext` is the public handle that tells the
// compiler which C++ host kind a `script Foo { ... }` is being bound to
// at load time. The Logia surface syntax is unchanged across host kinds;
// the context only influences future validation and code emission paths.
//
// S2.5 callers (and existing tests) can ignore `LogiaHostContext`
// entirely — the no-arg `Compiler::compile(source)` overload still
// constructs an implicit Component-host context (matching the S2.5
// semantics described in design.md §1.6 / §5.6).

#include "AYAst.h"
#include "AYCompilerError.h"
#include <memory>
#include <string>
#include <vector>

namespace ayt::reflect
{
class ITypeInfo;
}

namespace ayt::script::logia
{

// What kind of C++ host a `script Name { ... }` is being bound to.
// S3.0 (LG-03) only `Component` has real runtime support; `System`,
// `Tool`, `EventHandler` are reserved for S3.1+ (see design.md §5.6).
enum class LogiaHostKind {
    Component,    // S2.5 / LG-03 — ScriptComponent on Entity
    System,       // S3.1 (LG-04) — ISystem tick
    Tool,         // S3+    — editor/CLI one-shot
    EventHandler, // S3+    — event-bus callback object
};

// Caller-supplied description of the C++ host a `script` block binds to.
//
// S3.0 (LG-03): the analyzer stores the context but does NOT enable
// new validation. All existing S2.5 diagnostics are preserved verbatim
// regardless of which fields are set. `hostType == nullptr` means
// "skip any future subclass check" (the LG-03 default).
struct LogiaHostContext {
    LogiaHostKind kind = LogiaHostKind::Component;
    // Expected base type for `script Name` binding. nullptr disables
    // the future subclass check (LG-03 / S3.1). When AYReflect grows
    // a host-kind-aware validation, the analyzer will consult this.
    const ayt::reflect::ITypeInfo* hostType = nullptr;
    // Whether the host dispatch will pass a `self` lightuserdata.
    // S2.5 / LG-03 always true (ScriptComponent*). S3.1+ Tool hosts
    // pass false to allow scripts that omit `self.field` access.
    bool expectSelf = true;
};

// S2.5 / LG-03 default context: Component host, no subclass check, with self.
inline LogiaHostContext defaultLogiaHostContext()
{
    return LogiaHostContext{
        LogiaHostKind::Component,
        nullptr,
        true,
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
};

void tokenize(const std::string& source, std::vector<Token>& out);

class Compiler {
public:
    Compiler() = default;
    explicit Compiler(CompileOptions options);

    // S2.5 entry — equivalent to `compile(source, defaultLogiaHostContext())`.
    // Preserved verbatim so the 169 existing tests do not need editing.
    CompileResult compile(const std::string& source);

    // S3.0 (LG-03) entry — `ctx` is stored in the analyzer for future
    // host-aware validation. S3.0 diagnostics remain identical to the
    // no-ctx overload (no new errors / warnings are introduced).
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
