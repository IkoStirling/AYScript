#pragma once
#include "AYScript/logia/Logia.h"
namespace ayt::script::logia
{
/// Shared declarations for the default application host. Metadata grants no
/// runtime access; AYApplication installs the concrete providers into its bridge.
const std::unordered_map<std::string, HostFunctionSignature> &engineLogiaFunctions();
/// Authoring/CLI contract for scripts targeting the default Client/Editor host.
LogiaHostContext engineLogiaHostContext(LogiaHostContext context = {});
} // namespace ayt::script::logia
