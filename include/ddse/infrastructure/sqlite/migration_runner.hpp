#pragma once

#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"
#include "ddse/infrastructure/sqlite/database.hpp"

#include <string>
#include <vector>

namespace ddse::infrastructure::sqlite {

struct Migration {
    int version;
    std::string name;
    std::string sql;
};

class MigrationRunner {
public:
    [[nodiscard]] core::Result<void, core::Error> apply(Database& database,
                                                         const std::vector<Migration>& migrations) const;
};

} // namespace ddse::infrastructure::sqlite
