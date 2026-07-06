// AYCompilerError.cpp

#include "logia/AYCompilerError.h"
#include <sstream>

namespace ayt::script::logia
{

std::string LogiaDiagnostic::toHumanString() const
{
    std::ostringstream oss;
    if (!location.file.empty()) {
        oss << location.file << ':';
    }
    if (location.line > 0) {
        oss << location.line << ':' << location.column << ": ";
    }
    oss << "error[logia/";
    oss << static_cast<int>(errorCode) << "]: " << message;
    if (!hint.empty()) {
        oss << " (" << hint << ')';
    }
    return oss.str();
}

std::string CompilerError::toString() const
{
    std::ostringstream oss;
    oss << "error[logia/" << static_cast<int>(code) << "] at line " << line
        << ", column " << column << ": " << message;
    return oss.str();
}

void CompilerErrorReporter::error(ErrorCode code, const std::string& message, int line, int column)
{
    CompilerError err;
    err.code = code;
    err.message = message;
    err.line = line;
    err.column = column;
    _errors.push_back(std::move(err));
}

} // namespace ayt::script::logia
