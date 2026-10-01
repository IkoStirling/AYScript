// main.cpp - ays-logia CLI executable entry point.
//
// Usage:
//   ays-logia compile <file.logia> [--host component|actor|system|tool|eventhandler]
//                                [--strict-inheritance]
//                                [-o out.lua]
//
// On success: writes generated Lua to <out.lua> (or stdout if -o is
// omitted). On compile failure: prints diagnostic lines of the form
//   <path>:<line>:<col>: error: <message>
//   <path>:<line>:<col>: note:  <hint>            (when hint is set)
//   <path>:<line>:<col>: warning: <message>       (for soft warnings)
// to stderr and exits with non-zero status. Exit codes:
//   0  success (warnings allowed).
//   1  compile / CLI error (unknown flag, missing file, parse /
//      semantic errors).
//   2  bad usage.
//
// The CLI is deliberately tiny — the heavy lifting lives in
// CliCompile.{h,cpp} so unit tests can drive it directly without
// forking a child process.

#include "CliCompile.h"
#include "AYScript/logia/CompilerError.h"
#include "AYScript/logia/Logia.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{

const char* kUsage =
    "Usage: ays-logia compile <file.logia> [--host component|actor|system|tool|eventhandler]\n"
    "                                [--strict-inheritance]\n"
    "                                [-o out.lua]\n"
    "\n"
    "Compiles a Logia source file into Lua and emits either to stdout\n"
    "(default) or to a file (-o). Exits non-zero on any compile error.\n"
    "Warnings are surfaced on stderr but do not flip the exit code.\n";

struct Args {
    std::string sourcePath;
    std::string outputPath;
    ayt::script::logia::LogiaHostKind hostKind =
        ayt::script::logia::LogiaHostKind::Component;
    bool strictInheritance = false;
};

void printUsage(FILE* out)
{
    std::fputs(kUsage, out);
}

// Map a `--host <kind>` argument string to the enum. Returns true
// on success; on failure, writes a usage hint to `err` and returns
// false.
bool parseHostKind(const std::string& s,
                   ayt::script::logia::LogiaHostKind& out,
                   std::string& err)
{
    using ayt::script::logia::LogiaHostKind;
    if (s == "component") { out = LogiaHostKind::Component; return true; }
    if (s == "actor")     { out = LogiaHostKind::Actor;     return true; }
    if (s == "system")    { out = LogiaHostKind::System;    return true; }
    if (s == "tool")      { out = LogiaHostKind::Tool;      return true; }
    if (s == "eventhandler") {
        out = LogiaHostKind::EventHandler;
        return true;
    }
    err = "unknown --host kind '" + s
          + "' (expected component|actor|system|tool|eventhandler)";
    return false;
}

// Tiny argv parser. Recognized shape:
//   ays-logia compile <path> [--host <kind>] [--strict-inheritance] [-o <path>]
// Anything else prints usage to stderr and returns false.
bool parseArgs(int argc, char** argv, Args& out, std::string& err)
{
    if (argc < 2) {
        err = "missing subcommand";
        return false;
    }
    if (std::strcmp(argv[1], "compile") != 0) {
        err = std::string("unknown subcommand '") + argv[1] + "'";
        return false;
    }
    if (argc < 3) {
        err = "missing <file>";
        return false;
    }
    // The positional <file> must not look like a flag — otherwise a
    // typo like `compile --help` (which exits via -h/--help) or
    // `compile --no-such-flag` would silently be treated as the
    // source path and surface a much less helpful "could not open
    // file" diagnostic. We catch these here so they exit with code 2
    // + Usage instead of code 1.
    {
        const std::string first = argv[2];
        if (!first.empty() && first[0] == '-'
            && first != "-h" && first != "--help") {
            err = "unknown flag '" + first + "' (or missing <file> before it)";
            return false;
        }
    }
    out.sourcePath = argv[2];

    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--host") {
            if (i + 1 >= argc) { err = "--host needs a value"; return false; }
            if (!parseHostKind(argv[++i], out.hostKind, err)) return false;
        } else if (a == "--strict-inheritance") {
            out.strictInheritance = true;
        } else if (a == "-o") {
            if (i + 1 >= argc) { err = "-o needs a value"; return false; }
            out.outputPath = argv[++i];
        } else if (a == "-h" || a == "--help") {
            printUsage(stdout);
            std::exit(0);
        } else if (!a.empty() && a[0] == '-') {
            // Anything starting with '-' that's not a known flag is
            // a usage error. The check must happen before we treat
            // argv[i] as a positional argument (otherwise a stray
            // `--foo` would be parsed as the source path and we'd
            // exit with the much-less-helpful code 1 / "could not
            // open file" diagnostic instead of code 2 / Usage).
            err = "unknown flag '" + a + "'";
            return false;
        }
    }
    return true;
}

const char* severityTag(ayt::script::logia::DiagnosticSeverity s)
{
    using ayt::script::logia::DiagnosticSeverity;
    switch (s) {
    case DiagnosticSeverity::Error:   return "error";
    case DiagnosticSeverity::Warning: return "warning";
    case DiagnosticSeverity::Info:    return "note";
    }
    return "diagnostic";
}

// Render a single diagnostic as a one-line `file:line:col: tag: msg`
// string. Hint, when present, is rendered as a follow-up `note:` line
// at the same location (keeps the grep-friendly single-tag shape).
std::string formatDiagnostic(const ayt::script::logia::LogiaDiagnostic& d)
{
    std::string out;
    out += d.location.file.empty() ? std::string("<input>") : d.location.file;
    out += ':';
    out += std::to_string(d.location.line);
    out += ':';
    out += std::to_string(d.location.column);
    out += ": ";
    out += severityTag(d.severity);
    out += ": ";
    out += d.message;
    out += '\n';
    if (!d.hint.empty()) {
        out += d.location.file.empty() ? std::string("<input>") : d.location.file;
        out += ':';
        out += std::to_string(d.location.line);
        out += ':';
        out += std::to_string(d.location.column);
        out += ": note: ";
        out += d.hint;
        out += '\n';
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    Args args;
    std::string err;
    if (!parseArgs(argc, argv, args, err)) {
        std::fprintf(stderr, "ays-logia: %s\n\n", err.c_str());
        printUsage(stderr);
        return 2;
    }

    ayt::script::logia::CliCompileRequest req;
    req.sourcePath = args.sourcePath;
    req.outputPath = args.outputPath;
    req.ctx.kind = args.hostKind;
    req.ctx.strictInheritance = args.strictInheritance;
    if (args.hostKind == ayt::script::logia::LogiaHostKind::Tool
        || args.hostKind == ayt::script::logia::LogiaHostKind::EventHandler) {
        // Tool / EventHandler never bind a ScriptComponent receiver.
        req.ctx.expectSelf = false;
    }

    ayt::script::logia::CliCompileResult res =
        ayt::script::logia::compileFromCli(req);

    // Always print diagnostics — errors to stderr, warnings also to
    // stderr but they don't gate the exit code.
    for (const auto& d : res.diagnostics) {
        std::fputs(formatDiagnostic(d).c_str(), stderr);
    }

    if (!res.success) {
        std::fprintf(stderr, "ays-logia: compile failed (%zu error%s)\n",
                     res.errors.size(),
                     res.errors.size() == 1 ? "" : "s");
        return 1;
    }

    if (!args.outputPath.empty()) {
        std::ofstream f(args.outputPath, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            std::fprintf(stderr, "ays-logia: could not write '%s'\n",
                         args.outputPath.c_str());
            return 1;
        }
        f.write(res.lua.data(), static_cast<std::streamsize>(res.lua.size()));
    } else {
        std::fwrite(res.lua.data(), 1, res.lua.size(), stdout);
    }
    return 0;
}
