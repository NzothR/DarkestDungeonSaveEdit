#pragma once

#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"
#include "ddse/infrastructure/sqlite/database.hpp"

namespace ddse::infrastructure::sqlite {

class Transaction {
public:
    [[nodiscard]] static core::Result<Transaction, core::Error> begin(Database& database);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&& other) noexcept;
    [[nodiscard]] core::Result<void, core::Error> commit();
    [[nodiscard]] core::Result<void, core::Error> rollback();

private:
    explicit Transaction(Database& database) : database_(&database), active_(true) {}
    Database* database_{};
    bool active_{};
};

} // namespace ddse::infrastructure::sqlite
