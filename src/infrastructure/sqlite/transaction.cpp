#include "ddse/infrastructure/sqlite/transaction.hpp"

#include <utility>

namespace ddse::infrastructure::sqlite {

core::Result<Transaction, core::Error> Transaction::begin(Database& database) {
    auto result = database.execute("BEGIN IMMEDIATE");
    if (!result) return core::Result<Transaction, core::Error>::failure(result.error());
    return core::Result<Transaction, core::Error>::success(Transaction{database});
}

Transaction::~Transaction() {
    if (active_ && database_) (void)database_->execute("ROLLBACK");
}

Transaction::Transaction(Transaction&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)), active_(std::exchange(other.active_, false)) {}

Transaction& Transaction::operator=(Transaction&& other) noexcept {
    if (this != &other) {
        if (active_ && database_) (void)database_->execute("ROLLBACK");
        database_ = std::exchange(other.database_, nullptr);
        active_ = std::exchange(other.active_, false);
    }
    return *this;
}

core::Result<void, core::Error> Transaction::commit() {
    if (!active_ || !database_)
        return core::Result<void, core::Error>::failure({core::ErrorCode::DatabaseError, "Transaction is not active", "SQLite.Transaction", {}});
    auto result = database_->execute("COMMIT");
    if (result) active_ = false;
    return result;
}

core::Result<void, core::Error> Transaction::rollback() {
    if (!active_ || !database_) return core::Result<void, core::Error>::success();
    auto result = database_->execute("ROLLBACK");
    if (result) active_ = false;
    return result;
}

} // namespace ddse::infrastructure::sqlite
