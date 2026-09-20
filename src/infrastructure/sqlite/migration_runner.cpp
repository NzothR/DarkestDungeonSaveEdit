#include "ddse/infrastructure/sqlite/migration_runner.hpp"
#include "ddse/infrastructure/sqlite/statement.hpp"
#include "ddse/infrastructure/sqlite/transaction.hpp"

#include <algorithm>

namespace ddse::infrastructure::sqlite {

core::Result<void, core::Error> MigrationRunner::apply(Database& database,
                                                        const std::vector<Migration>& migrations) const {
    auto table = database.execute("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, name TEXT NOT NULL)");
    if (!table) return core::Result<void, core::Error>::failure(table.error());

    auto ordered = migrations;
    std::sort(ordered.begin(), ordered.end(), [](const Migration& a, const Migration& b) { return a.version < b.version; });
    int previous = 0;
    for (const auto& migration : ordered) {
        if (migration.version <= previous) {
            return core::Result<void, core::Error>::failure({core::ErrorCode::MigrationFailed,
                "Migration versions must be unique positive integers", "MigrationRunner",
                {{"version", std::to_string(migration.version)}}});
        }
        previous = migration.version;

        auto query = database.prepare("SELECT version FROM schema_migrations WHERE version = ?1");
        if (!query) return core::Result<void, core::Error>::failure(query.error());
        auto bound = query.value().bind(1, static_cast<std::int64_t>(migration.version));
        if (!bound) return core::Result<void, core::Error>::failure(bound.error());
        auto row = query.value().step();
        if (!row) return core::Result<void, core::Error>::failure(row.error());
        if (row.value()) continue;

        auto transaction = Transaction::begin(database);
        if (!transaction) return core::Result<void, core::Error>::failure(transaction.error());
        auto applied = database.execute(migration.sql);
        if (!applied) {
            auto error = core::Error{core::ErrorCode::MigrationFailed, "Migration SQL failed", "MigrationRunner",
                                     {{"version", std::to_string(migration.version)}, {"name", migration.name}}};
            error.cause = std::make_shared<core::Error>(applied.error());
            return core::Result<void, core::Error>::failure(std::move(error));
        }
        auto insert = database.prepare("INSERT INTO schema_migrations(version, name) VALUES(?1, ?2)");
        if (!insert) return core::Result<void, core::Error>::failure(insert.error());
        auto bind_version = insert.value().bind(1, static_cast<std::int64_t>(migration.version));
        if (!bind_version) return core::Result<void, core::Error>::failure(bind_version.error());
        auto bind_name = insert.value().bind(2, migration.name);
        if (!bind_name) return core::Result<void, core::Error>::failure(bind_name.error());
        auto inserted = insert.value().step();
        if (!inserted) return core::Result<void, core::Error>::failure(inserted.error());
        auto committed = transaction.value().commit();
        if (!committed) return core::Result<void, core::Error>::failure(committed.error());
    }
    return core::Result<void, core::Error>::success();
}

} // namespace ddse::infrastructure::sqlite
