// AYLogia.cpp

#include "logia/AYLogia.h"
#include "logia/AYLexer.h"
#include "logia/AYParser.h"
#include "logia/AYSemanticAnalyzer.h"

#include <algorithm>
#include <memory>

namespace ayt::script::logia
{

void tokenize(const std::string& source, std::vector<Token>& out)
{
    Lexer lexer(source);
    lexer.tokenize(out);
}

Compiler::Compiler(CompileOptions options)
    : _options(std::move(options)) {}

CompileResult Compiler::compile(const std::string& source)
{
    // S2.5 path — preserve the exact pre-S3.0 behavior. All existing
    // tests that use the no-ctx overload keep their previous diagnostics
    // verbatim.
    return compile(source, defaultLogiaHostContext());
}

CompileResult Compiler::compile(const std::string& source,
                                const LogiaHostContext& ctx)
{
    _lastCtx = ctx;

    CompileResult result;

    std::vector<Token> tokens;
    try {
        tokenize(source, tokens);
    } catch (const std::exception& e) {
        CompilerError err;
        err.code = ErrorCode::UnexpectedToken;
        err.message = e.what();
        result.errors.push_back(std::move(err));
        result.success = false;
        return result;
    }

    Parser parser(tokens);
    result.program = parser.parse();
    result.errors = parser.errors();
    // S3.9: also project parser errors into the LogiaDiagnostic stream
    // so the CLI / pipeline callers can iterate a single uniform
    // diagnostic list. Without this, parser errors live only in
    // `errors` (legacy projection) and callers that walk `diagnostics`
    // miss them. Severity is always Error for parser-side failures.
    for (const auto& e : result.errors) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = e.code;
        d.message = e.message;
        d.location.line = e.line;
        d.location.column = e.column;
        result.diagnostics.push_back(d);
    }
    const bool parserOk = !parser.hasErrors() && result.program != nullptr;

    // S3.0 (LG-03): thread the host context into the analyzer. The
    // analyzer stores it for future host-aware validation but does
    // NOT introduce new diagnostics in LG-03 — every existing test
    // that asserted on a S2.5 diagnostic must still pass unchanged.
    if (parserOk) {
        auto sem = std::make_unique<SemanticAnalyzer>(SemanticOptions{
            _options.useCompileTimeTypes,
            _options.fileName.c_str()
        }, ctx);
        SemanticResult semRes = sem->analyze(*result.program);
        for (auto& d : semRes.diagnostics) {
            result.diagnostics.push_back(d);
            if (d.severity == DiagnosticSeverity::Error) {
                result.errors.push_back(d.toCompilerError());
            }
        }
    }

    result.success = parserOk &&
        std::none_of(result.diagnostics.begin(),
                     result.diagnostics.end(),
                     [](const LogiaDiagnostic& d) {
                         return d.severity == DiagnosticSeverity::Error;
                     });
    return result;
}

} // namespace ayt::script::logia
