#include "ddse/infrastructure/sqlite/statement.hpp"
#include "sqlite_error.hpp"

#include <sqlite3.h>

#include <utility>

namespace ddse::infrastructure::sqlite {

Statement::~Statement() { if (handle_) sqlite3_finalize(handle_); }
Statement::Statement(Statement&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (handle_) sqlite3_finalize(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

core::Result<void, core::Error> Statement::bind(int index, std::string_view value) {
    const int rc = sqlite3_bind_text(handle_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) return core::Result<void, core::Error>::failure(detail::make_error(sqlite3_db_handle(handle_), rc, "SQLite.Statement", "bind_text"));
    return core::Result<void, core::Error>::success();
}
core::Result<void, core::Error> Statement::bind(int index, std::int64_t value) {
    const int rc = sqlite3_bind_int64(handle_, index, value);
    if (rc != SQLITE_OK) return core::Result<void, core::Error>::failure(detail::make_error(sqlite3_db_handle(handle_), rc, "SQLite.Statement", "bind_int64"));
    return core::Result<void, core::Error>::success();
}
core::Result<void, core::Error> Statement::bind_null(int index) {
    const int rc = sqlite3_bind_null(handle_, index);
    if (rc != SQLITE_OK) return core::Result<void, core::Error>::failure(detail::make_error(sqlite3_db_handle(handle_), rc, "SQLite.Statement", "bind_null"));
    return core::Result<void, core::Error>::success();
}
core::Result<bool, core::Error> Statement::step() {
    const int rc = sqlite3_step(handle_);
    if (rc == SQLITE_ROW) return core::Result<bool, core::Error>::success(true);
    if (rc == SQLITE_DONE) return core::Result<bool, core::Error>::success(false);
    return core::Result<bool, core::Error>::failure(detail::make_error(sqlite3_db_handle(handle_), rc, "SQLite.Statement", "step"));
}
std::int64_t Statement::column_int64(int index) const { return sqlite3_column_int64(handle_, index); }
std::string_view Statement::column_text(int index) const {
    const auto* text = sqlite3_column_text(handle_, index);
    const auto size = sqlite3_column_bytes(handle_, index);
    return text ? std::string_view{reinterpret_cast<const char*>(text), static_cast<std::size_t>(size)} : std::string_view{};
}
bool Statement::column_is_null(int index) const { return sqlite3_column_type(handle_, index) == SQLITE_NULL; }
core::Result<void, core::Error> Statement::reset() {
    const int rc = sqlite3_reset(handle_);
    if (rc != SQLITE_OK) return core::Result<void, core::Error>::failure(detail::make_error(sqlite3_db_handle(handle_), rc, "SQLite.Statement", "reset"));
    sqlite3_clear_bindings(handle_);
    return core::Result<void, core::Error>::success();
}

} // namespace ddse::infrastructure::sqlite
