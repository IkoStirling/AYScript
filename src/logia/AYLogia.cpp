// AYLogia.cpp

#include "logia/AYLogia.h"
#include "logia/AYLexer.h"
#include "logia/AYParser.h"
#include "logia/AYSemanticAnalyzer.h"

#include <algorithm>

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
    const bool parserOk = !parser.hasErrors() && result.program != nullptr;

    // S2: run semantic analysis on the parsed AST.
    if (parserOk) {
        SemanticAnalyzer sem(SemanticOptions{
            _options.useCompileTimeTypes,
            _options.fileName.c_str()
        });
        SemanticResult semRes = sem.analyze(*result.program);
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
