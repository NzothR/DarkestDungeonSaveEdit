#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/migration_runner.hpp"
#include "ddse/infrastructure/sqlite/statement.hpp"
#include "ddse/infrastructure/sqlite/transaction.hpp"

#include <gtest/gtest.h>

TEST(SQLite, TransactionDestructorRollsBackUncommittedChanges) {
    auto opened = ddse::infrastructure::sqlite::Database::open(":memory:");
    ASSERT_TRUE(opened);
    auto& db = opened.value();
    ASSERT_TRUE(db.execute("CREATE TABLE sample (value INTEGER NOT NULL)"));
    {
        auto transaction = ddse::infrastructure::sqlite::Transaction::begin(db);
        ASSERT_TRUE(transaction);
        ASSERT_TRUE(db.execute("INSERT INTO sample(value) VALUES(7)"));
    }
    auto query = db.prepare("SELECT COUNT(*) FROM sample");
    ASSERT_TRUE(query);
    auto row = query.value().step();
    ASSERT_TRUE(row);
    ASSERT_TRUE(row.value());
    EXPECT_EQ(query.value().column_int64(0), 0);
}

TEST(SQLite, MigrationRunnerAppliesEachVersionOnce) {
    auto opened = ddse::infrastructure::sqlite::Database::open(":memory:");
    ASSERT_TRUE(opened);
    const std::vector<ddse::infrastructure::sqlite::Migration> migrations{
        {1, "sample", "CREATE TABLE sample (id INTEGER PRIMARY KEY)"}};
    ddse::infrastructure::sqlite::MigrationRunner runner;
    ASSERT_TRUE(runner.apply(opened.value(), migrations));
    ASSERT_TRUE(runner.apply(opened.value(), migrations));
    auto query = opened.value().prepare("SELECT COUNT(*) FROM schema_migrations");
    ASSERT_TRUE(query);
    auto row = query.value().step();
    ASSERT_TRUE(row);
    ASSERT_TRUE(row.value());
    EXPECT_EQ(query.value().column_int64(0), 1);
}
