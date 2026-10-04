// AYLogia.cpp

#include "AYScript/logia/Logia.h"
#include "AYScript/logia/Lexer.h"
#include "AYScript/logia/Parser.h"
#include "AYScript/logia/SemanticAnalyzer.h"
#include "AYScript/logia/CheckedSemantics.h"
#include "AYScript/logia/Modules.h"

#include <algorithm>
#include <functional>
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
        auto moduleContext = ctx;
        if (moduleContext.sourcePath.empty()) moduleContext.sourcePath = _options.fileName;
        auto moduleDiagnostics = expandLogiaModules(*result.program, moduleContext, result.dependencies);
        for (auto& d : moduleDiagnostics) {
            result.errors.push_back(d.toCompilerError()); result.diagnostics.push_back(std::move(d));
        }
        if (ctx.strictSemantics) {
            auto diagnostics = checkLogiaSemantics(*result.program, ctx, _options.fileName);
            for (auto& d : diagnostics) {
                if (d.severity == DiagnosticSeverity::Error)
                    result.errors.push_back(d.toCompilerError());
                result.diagnostics.push_back(std::move(d));
            }
        } else {
        std::function<void(const std::vector<StmtPtr>&)> checkedSyntax;
        checkedSyntax = [&](const auto& statements) {
            for (const auto& item : statements) {
                bool checkedOnly=dynamic_cast<AwaitStmt*>(item.get())!=nullptr;
                if(auto* function=dynamic_cast<FunctionDeclStmt*>(item.get())){checkedOnly|=!function->returnType.empty();checkedSyntax(function->body);}
                if(auto* lifecycle=dynamic_cast<LifecycleFuncDecl*>(item.get())){checkedOnly|=lifecycle->kind==LifecycleKind::OnReload;checkedSyntax(lifecycle->body);}
                if(auto* branch=dynamic_cast<IfStmt*>(item.get())){checkedSyntax(branch->thenBranch);checkedSyntax(branch->elseBranch);}
                if(auto* loop=dynamic_cast<WhileStmt*>(item.get()))checkedSyntax(loop->body);
                if(auto* loop=dynamic_cast<ForStmt*>(item.get()))checkedSyntax(loop->body);
                if(auto* block=dynamic_cast<BlockStmt*>(item.get()))checkedSyntax(block->body);
                if(checkedOnly){LogiaDiagnostic d{DiagnosticSeverity::Error,ErrorCode::InvalidStatement,"this syntax requires the checked language contract",item->sourceLoc,{}};result.errors.push_back(d.toCompilerError());result.diagnostics.push_back(std::move(d));}
            }
        };
        for(const auto& script:result.program->scripts)checkedSyntax(script->members);
        for (const auto& script : result.program->scripts)
            for (const auto& member : script->members)
                if (auto* var = dynamic_cast<VarDeclStmt*>(member.get()); var && var->instanceState) {
                    LogiaDiagnostic d{DiagnosticSeverity::Error, ErrorCode::InvalidStatement,
                        "state declarations require the checked language contract", var->sourceLoc, {}};
                    result.errors.push_back(d.toCompilerError()); result.diagnostics.push_back(std::move(d));
                }
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
