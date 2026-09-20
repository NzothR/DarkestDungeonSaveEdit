#pragma once

#include "ddse/core/error.hpp"

#include <sqlite3.h>

namespace ddse::infrastructure::sqlite::detail {

inline core::Error make_error(sqlite3* db, int result, const char* module, const char* operation) {
    const auto primary = result & 0xff;
    const auto code = (primary == SQLITE_BUSY || primary == SQLITE_LOCKED)
                          ? core::ErrorCode::DatabaseLocked
                          : core::ErrorCode::DatabaseError;
    const char* message = db ? sqlite3_errmsg(db) : sqlite3_errstr(result);
    return {code, message ? message : "SQLite error", module,
            {{"operation", operation}, {"sqlite_code", std::to_string(result)}}};
}

} // namespace ddse::infrastructure::sqlite::detail
