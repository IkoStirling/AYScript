#pragma once
#include "AYScript/logia/Logia.h"

namespace ayt::script::logia {
using BuiltinFunctionCatalog = std::unordered_map<std::string, std::pair<std::vector<std::string>, std::string>>;
/// Shared signatures used by validation and authoring tools.
const BuiltinFunctionCatalog& checkedBuiltinFunctions();
/// Validate and resolve the production language contract, including lexical
/// scopes, expressions, reflected calls, callback signatures and return paths.
std::vector<LogiaDiagnostic> checkLogiaSemantics(
    Program& program, const LogiaHostContext& ctx, const std::string& fileName);
}
