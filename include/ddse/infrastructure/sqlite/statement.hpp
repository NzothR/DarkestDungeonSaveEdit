#pragma once

#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstdint>
#include <string_view>

struct sqlite3_stmt;

namespace ddse::infrastructure::sqlite {

class Statement {
public:
    Statement() = default;
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    [[nodiscard]] core::Result<void, core::Error> bind(int index, std::string_view value);
    [[nodiscard]] core::Result<void, core::Error> bind(int index, std::int64_t value);
    [[nodiscard]] core::Result<void, core::Error> bind_null(int index);
    [[nodiscard]] core::Result<bool, core::Error> step(); // true = row, false = done
    [[nodiscard]] std::int64_t column_int64(int index) const;
    [[nodiscard]] std::string_view column_text(int index) const;
    [[nodiscard]] bool column_is_null(int index) const;
    [[nodiscard]] core::Result<void, core::Error> reset();

private:
    friend class Database;
    explicit Statement(sqlite3_stmt* handle) : handle_(handle) {}
    sqlite3_stmt* handle_{};
};

} // namespace ddse::infrastructure::sqlite
