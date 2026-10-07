#pragma once
#include "AYScript/logia/Logia.h"
namespace ayt::script::logia {
/// Resolve helper/asset-constant modules with bounded depth/size and root checks.
/// Every node retains its original file location for diagnostics/source maps.
/// Optional resources output collects immutable, portable logical asset paths;
/// runtime compilation permits virtual DB/pak resources, while offline packaging
/// and ProjectBuild validate the referenced physical authoring assets.
std::vector<LogiaDiagnostic> expandLogiaModules(Program& program,
    const LogiaHostContext& context, std::vector<std::string>& dependencies,
    std::vector<std::string>* resources = nullptr);
}
