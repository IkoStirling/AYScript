#pragma once
// AYLogia.h - Logia compiler entry (S0: tokenize + parse)

#include "AYAst.h"
#include "AYCompilerError.h"
#include <memory>
#include <string>
#include <vector>

namespace ayt::script::logia
{

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

    CompileResult compile(const std::string& source);

private:
    CompileOptions _options;
};

} // namespace ayt::script::logia
