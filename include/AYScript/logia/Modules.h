#pragma once
#include "AYScript/logia/Logia.h"
namespace ayt::script::logia {
/// Resolve helper-only modules with bounded depth/size and source-root checks.
/// Every node retains its original file location for diagnostics/source maps.
std::vector<LogiaDiagnostic> expandLogiaModules(Program& program,
    const LogiaHostContext& context, std::vector<std::string>& dependencies);
}
