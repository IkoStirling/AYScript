#pragma once
// AYCompilerError.h - Compiler error definitions for Logia

#include <string>
#include <vector>

namespace ayt::script::logia
{

enum class ErrorCode : uint8_t {
    UnexpectedToken,
    InvalidExpression,
    InvalidStatement,
    UnterminatedString,
    InvalidNumber,
    UnexpectedEndOfFile,
    TypeMismatch,
    UnknownIdentifier,
    InvalidOperation
};

enum class DiagnosticSeverity : uint8_t {
    Error,
    Warning,
    Info,
};

struct SourceLocation {
    std::string file;
    int line = 0;
    int column = 0;
};

// Forward declaration so LogiaDiagnostic can name CompilerError as a
// return type. The full definition follows.
struct CompilerError;

struct LogiaDiagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    ErrorCode errorCode = ErrorCode::UnexpectedToken;
    std::string message;
    SourceLocation location;
    std::string hint;

    std::string toHumanString() const;

    // Project to legacy CompilerError (drops file + hint, keeps code /
    // message / line / column). Defined out-of-line below so the
    // `CompilerError` type is fully visible.
    CompilerError toCompilerError() const;
};

struct CompilerError {
    ErrorCode code = ErrorCode::UnexpectedToken;
    std::string message;
    int line = 0;
    int column = 0;

    CompilerError() = default;
    CompilerError(ErrorCode c, std::string m, int l, int col)
        : code(c), message(std::move(m)), line(l), column(col) {}

    std::string toString() const;
};

// Out-of-line definition after both types are complete.
inline CompilerError LogiaDiagnostic::toCompilerError() const
{
    return CompilerError(errorCode, message, location.line, location.column);
}

class CompilerErrorReporter {
public:
    void error(ErrorCode code, const std::string& message, int line, int column);
    bool hasErrors() const { return !_errors.empty(); }
    const std::vector<CompilerError>& errors() const { return _errors; }
    void clear() { _errors.clear(); }

private:
    std::vector<CompilerError> _errors;
};

} // namespace ayt::script::logia
