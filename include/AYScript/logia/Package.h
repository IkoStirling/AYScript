#pragma once
#include "AYScript/logia/Logia.h"
namespace ayt::script::logia {
struct LogiaPackageResult {
    bool success = false;
    std::string manifestPath, source, scriptName;
    LogiaHostContext context;
    std::vector<LogiaDiagnostic> diagnostics;
    std::vector<std::string> dependencies;
};
/// Validate a checked entry and copy its complete source closure into a NEW
/// destination directory. Existing output is never overwritten. Manifest v1
/// records schema, host signatures and content fingerprints; no authoring paths
/// are required by readers. This is a source package compiled by the runtime.
LogiaPackageResult buildLogiaPackage(const std::string& sourcePath,
    const std::string& destination, LogiaHostContext context);
/// Read a v1 manifest, enforce path/size/content/dependency closure checks and
/// revalidate source. Fingerprints detect corruption; they are not signatures.
LogiaPackageResult readLogiaPackage(const std::string& manifestPath);
}
