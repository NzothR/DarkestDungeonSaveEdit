#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ddse::core {

enum class ErrorCode {
    InvalidConfiguration,
    FileNotFound,
    PermissionDenied,
    IoError,
    DsonMalformed,
    UnsupportedDsonType,
    DatabaseLocked,
    DatabaseError,
    MigrationFailed,
    ContentParseFailed,
    MappingNotWritable,
    ConcurrentSaveChanged,
    ValidationFailed,
    BackupFailed,
    CommitFailed,
};

struct Error {
    ErrorCode code;
    std::string message;
    std::string module;
    std::map<std::string, std::string> context;
    std::shared_ptr<Error> cause;

    Error(ErrorCode error_code, std::string error_message, std::string error_module,
          std::map<std::string, std::string> error_context = {},
          std::shared_ptr<Error> error_cause = {})
        : code(error_code), message(std::move(error_message)), module(std::move(error_module)),
          context(std::move(error_context)), cause(std::move(error_cause)) {}

    [[nodiscard]] Error with_context(std::string key, std::string value) const;
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

} // namespace ddse::core
