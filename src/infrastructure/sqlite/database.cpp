#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/statement.hpp"
#include "sqlite_error.hpp"

#include <sqlite3.h>

#include <utility>

namespace ddse::infrastructure::sqlite {

Database::~Database() { if (handle_) sqlite3_close_v2(handle_); }

Database::Database(Database&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        if (handle_) sqlite3_close_v2(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

core::Result<Database, core::Error> Database::open(const std::filesystem::path& path) {
    sqlite3* handle = nullptr;
    const auto native_path = path.u8string();
    const int result = sqlite3_open_v2(reinterpret_cast<const char*>(native_path.c_str()), &handle,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                       nullptr);
    if (result != SQLITE_OK) {
        auto error = detail::make_error(handle, result, "SQLite", "open");
        if (handle) sqlite3_close_v2(handle);
        error.context.insert_or_assign("path", path.string());
        return core::Result<Database, core::Error>::failure(std::move(error));
    }
    sqlite3_busy_timeout(handle, 5000);
    return core::Result<Database, core::Error>::success(Database{handle});
}

core::Result<void, core::Error> Database::execute(std::string_view sql) {
    char* message = nullptr;
    const std::string owned{sql};
    const int result = sqlite3_exec(handle_, owned.c_str(), nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        auto error = detail::make_error(handle_, result, "SQLite", "execute");
        if (message) {
            error.context.insert_or_assign("detail", message);
            sqlite3_free(message);
        }
        return core::Result<void, core::Error>::failure(std::move(error));
    }
    return core::Result<void, core::Error>::success();
}

core::Result<Statement, core::Error> Database::prepare(std::string_view sql) {
    sqlite3_stmt* statement = nullptr;
    const int result = sqlite3_prepare_v2(handle_, sql.data(), static_cast<int>(sql.size()), &statement, nullptr);
    if (result != SQLITE_OK)
        return core::Result<Statement, core::Error>::failure(detail::make_error(handle_, result, "SQLite", "prepare"));
    return core::Result<Statement, core::Error>::success(Statement{statement});
}

} // namespace ddse::infrastructure::sqlite
