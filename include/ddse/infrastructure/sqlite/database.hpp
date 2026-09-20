#pragma once

#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <filesystem>
#include <string_view>

struct sqlite3;

namespace ddse::infrastructure::sqlite {

class Statement;

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    [[nodiscard]] static core::Result<Database, core::Error> open(const std::filesystem::path& path);
    [[nodiscard]] core::Result<void, core::Error> execute(std::string_view sql);
    [[nodiscard]] core::Result<Statement, core::Error> prepare(std::string_view sql);
    [[nodiscard]] sqlite3* native_handle() const noexcept { return handle_; }

private:
    explicit Database(sqlite3* handle) : handle_(handle) {}
    sqlite3* handle_{};
};

class ConnectionFactory {
public:
    [[nodiscard]] core::Result<Database, core::Error> open(const std::filesystem::path& path) const {
        return Database::open(path);
    }
};

} // namespace ddse::infrastructure::sqlite
