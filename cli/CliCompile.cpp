// CliCompile.cpp - shared compile entry for the ays-logia CLI.

#include "CliCompile.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace ayt::script::logia
{

namespace {

// Read an entire text file into a string. Returns false on open /
// read failure; on failure the out string is empty and `err` carries
// a human-readable reason.
bool readEntireFile(const std::string& path, std::string& out, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        err = "could not open file '" + path + "'";
        return false;
    }
    std::ostringstream oss;
    oss << f.rdbuf();
    out = oss.str();
    return true;
}

} // namespace

CliCompileResult compileFromCli(const CliCompileRequest& req)
{
    CliCompileResult out;

    // Resolve the source. Filesystem path wins; inline sourceText is
    // the test-only fallback.
    std::string source;
    if (!req.sourcePath.empty()) {
        std::string err;
        if (!readEntireFile(req.sourcePath, source, err)) {
            LogiaDiagnostic d;
            d.severity = DiagnosticSeverity::Error;
            d.errorCode = ErrorCode::InvalidOperation;
            d.message = err;
            d.location.file = req.sourcePath;
            out.diagnostics.push_back(std::move(d));
            out.success = false;
            return out;
        }
    } else {
        source = req.sourceText;
    }
    out.resolvedSource = source;

    // Run the full front-end + codegen pipeline. The pipeline returns
    // diagnostics for both parser (errors) and semantic (errors +
    // warnings) on every path; success is false iff any error was
    // produced (warnings alone do not flip success).
    LuaCodegenOptions codegenOpts;
    codegenOpts.scriptName = req.sourcePath.empty()
                                 ? std::string{"<inline>"}
                                 : req.sourcePath;
    LogiaToLuaResult pipeline = compileLogiaToLua(source, req.ctx, codegenOpts);
    out.diagnostics = std::move(pipeline.diagnostics);
    out.errors = std::move(pipeline.errors);
    out.lua = std::move(pipeline.lua);
    out.success = pipeline.success;

    // Stamp the source path on diagnostics so callers can format
    // `file:line:col:` strings without knowing the original request.
    if (!req.sourcePath.empty()) {
        for (auto& d : out.diagnostics) {
            if (d.location.file.empty()) {
                d.location.file = req.sourcePath;
            }
        }
    }

    return out;
}

} // namespace ayt::script::logia