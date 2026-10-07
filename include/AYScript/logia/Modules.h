#pragma once
#include "AYScript/logia/Logia.h"
namespace ayt::script::logia {
/// Resolve reusable behaviors with bounded depth/size and root checks.
/// Helpers, assets, state/persist fields and signals form one checked namespace;
/// collisions fail. Imported lifecycle hooks run in depth-first source order,
/// destroy runs in reverse, and each hook retains independent return/local scope.
/// Every node retains its original file location for diagnostics/source maps.
/// Optional resources output collects immutable, portable logical asset paths;
/// runtime compilation permits virtual DB/pak resources, while offline packaging
/// and ProjectBuild validate the referenced physical authoring assets.
std::vector<LogiaDiagnostic> expandLogiaModules(Program& program,
    const LogiaHostContext& context, std::vector<std::string>& dependencies,
    std::vector<std::string>* resources = nullptr);
}
