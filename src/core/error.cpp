#include "ddse/core/error.hpp"

namespace ddse::core {

Error Error::with_context(std::string key, std::string value) const {
    Error copy = *this;
    copy.context.insert_or_assign(std::move(key), std::move(value));
    return copy;
}

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidConfiguration: return "InvalidConfiguration";
    case ErrorCode::FileNotFound: return "FileNotFound";
    case ErrorCode::PermissionDenied: return "PermissionDenied";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::DsonMalformed: return "DsonMalformed";
    case ErrorCode::UnsupportedDsonType: return "UnsupportedDsonType";
    case ErrorCode::DatabaseLocked: return "DatabaseLocked";
    case ErrorCode::DatabaseError: return "DatabaseError";
    case ErrorCode::MigrationFailed: return "MigrationFailed";
    case ErrorCode::ContentParseFailed: return "ContentParseFailed";
    case ErrorCode::MappingNotWritable: return "MappingNotWritable";
    case ErrorCode::ConcurrentSaveChanged: return "ConcurrentSaveChanged";
    case ErrorCode::ValidationFailed: return "ValidationFailed";
    case ErrorCode::BackupFailed: return "BackupFailed";
    case ErrorCode::CommitFailed: return "CommitFailed";
    }
    return "UnknownError";
}

} // namespace ddse::core
