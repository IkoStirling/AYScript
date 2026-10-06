#pragma once
#include "AYScript/logia/Logia.h"

namespace ayt::script::logia {
using BuiltinFunctionCatalog = std::unordered_map<std::string, std::pair<std::vector<std::string>, std::string>>;
/// Shared signatures used by validation and authoring tools.
const BuiltinFunctionCatalog& checkedBuiltinFunctions();
/// EventBus positional payload types shared by callback and await validation.
/// Scene payloads are borrowed opaque pointers; only synchronous subscriptions
/// may consume them. Payload-binding awaits support owned scalar aliases.
const std::unordered_map<std::string, std::vector<std::string>>& checkedEventPayloads();
/// Validate and resolve the production language contract, including lexical
/// scopes, expressions, reflected calls, callback signatures and return paths.
std::vector<LogiaDiagnostic> checkLogiaSemantics(
    Program& program, const LogiaHostContext& ctx, const std::string& fileName);
}
