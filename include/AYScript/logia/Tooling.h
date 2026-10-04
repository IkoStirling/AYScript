#pragma once
#include "AYScript/logia/Logia.h"
#include <optional>
namespace ayt::script::logia {
struct LogiaSymbol {
    std::string name, type, kind;
    SourceLocation definition;
    int scopeBegin = 0, scopeEnd = 0; // Inclusive source lines; 0 means module scope.
};
struct LogiaReference { SourceLocation location; std::size_t symbol = 0; };
struct LogiaAnalysis {
    bool success = false;
    std::vector<LogiaDiagnostic> diagnostics;
    std::vector<std::string> dependencies;
    std::vector<LogiaSymbol> symbols;
    std::vector<LogiaReference> references;
    /// Prefix may include self., a facade namespace, or a local vector name.
    /// file+line restrict locals to their lexical block; module helpers are visible throughout.
    std::vector<LogiaSymbol> complete(const std::string& prefix, const std::string& file = {}, int line = 0) const;
    /// Resolve a symbol reference at its token location. Returns no target for unresolved input.
    std::optional<SourceLocation> definitionAt(const SourceLocation& location) const;
};
/// Read-only analysis uses the checked compiler and preserves imported file locations.
/// Schema suggestions remain available for incomplete/invalid source.
LogiaAnalysis analyzeLogia(const std::string& source, LogiaHostContext context);
}
