// AYLogia.cpp

#include "logia/AYLogia.h"
#include "logia/AYLexer.h"
#include "logia/AYParser.h"

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
    result.success = !parser.hasErrors() && result.program != nullptr;
    (void)_options;
    return result;
}

} // namespace ayt::script::logia
