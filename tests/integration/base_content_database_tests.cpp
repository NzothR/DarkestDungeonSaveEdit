#include "ddse/core/dson/dson_document.hpp"
#include "ddse/infrastructure/base_content_database.hpp"
#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/statement.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("ddse-content-db-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

ddse::application::BaseContentScanResult sample_scan() {
    using namespace ddse::application;
    BaseContentScanResult scan;
    scan.sources.push_back({"vanilla", "Vanilla", ContentSourceType::Vanilla, "D:/readonly/game"});
    scan.source_files.push_back({"vanilla", "trinkets/base.entries.trinkets.json", ".json", 80, 0x1234, false});
    scan.source_files.push_back({"vanilla", "localization/trinkets.string_table.xml", ".xml", 120, 0x2345, false});
    scan.source_files.push_back({"vanilla", "heroes/crusader/trinket.png", ".png", 1024, 0, true});
    scan.definitions.push_back({"trinket", "sample_token", "vanilla", "trinkets/base.entries.trinkets.json",
                                "", "trinket_name_sample_token", R"({"id":"sample_token"})"});
    scan.localizations.push_back({"english", "trinket_name_sample_token", "Duplicate Token", "vanilla",
                                  "localization/trinkets.string_table.xml"});
    scan.localizations.push_back({"english", "trinket_name_sample_token", "Sample Token", "vanilla",
                                  "localization/trinkets.string_table.xml"});
    scan.assets.push_back({"vanilla", "heroes/crusader/trinket.png", ".png", 1024});
    scan.asset_references.push_back({"vanilla", "trinket", "sample_token",
        "trinkets/base.entries.trinkets.json", "inventory_icon", "file",
        "panels/icons_equip/trinket/inv_trinket+sample_token.png", "convention"});
    scan.relationships.push_back({"vanilla", "trinket", "sample_token", "restricted_to",
        "hero_class", "crusader", "trinkets/base.entries.trinkets.json"});
    return scan;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST(BaseContentDatabase, BuildsQueryableSourceLocalizationAssetAndHashIndexes) {
    TempDirectory temp;
    const auto path = temp.path / "base_content.db";
    ddse::infrastructure::BaseContentDatabaseBuilder builder;
    auto built = builder.rebuild(path, sample_scan());
    ASSERT_TRUE(built) << built.error().message;
    EXPECT_EQ(built.value().sources, 1U);
    EXPECT_EQ(built.value().source_files, 3U);
    EXPECT_EQ(built.value().trinkets, 1U);
    EXPECT_EQ(built.value().localization_entries, 1U);
    EXPECT_EQ(built.value().assets, 1U);
    EXPECT_EQ(built.value().asset_references, 1U);
    EXPECT_EQ(built.value().relationships, 1U);
    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".new"));

    auto database = ddse::infrastructure::sqlite::ConnectionFactory{}.open(path);
    ASSERT_TRUE(database) << database.error().message;
    auto statement_result = database.value().prepare(
        "SELECT d.content_id,s.display_name,f.virtual_path,l.localized_text,a.virtual_path,h.hash_value,"
        "(SELECT r.virtual_path FROM content_asset_references r WHERE r.content_id=d.content_id LIMIT 1),"
        "(SELECT r.child_id FROM content_relationships r WHERE r.parent_id=d.content_id LIMIT 1) "
        "FROM content_definitions d JOIN source_files f USING(source_file_id) "
        "JOIN content_sources s USING(source_id) "
        "JOIN localization_entries l ON l.localization_key=d.localization_key "
        "JOIN assets a ON a.virtual_path='heroes/crusader/trinket.png' "
        "JOIN content_hash_index h ON h.content_id=d.content_id WHERE d.content_id=?");
    ASSERT_TRUE(statement_result) << statement_result.error().message;
    auto statement = std::move(statement_result.value());
    ASSERT_TRUE(statement.bind(1, "sample_token"));
    auto row = statement.step();
    ASSERT_TRUE(row) << row.error().message;
    ASSERT_TRUE(row.value());
    EXPECT_EQ(statement.column_text(0), "sample_token");
    EXPECT_EQ(statement.column_text(1), "Vanilla");
    EXPECT_EQ(statement.column_text(2), "trinkets/base.entries.trinkets.json");
    EXPECT_EQ(statement.column_text(3), "Sample Token");
    EXPECT_EQ(statement.column_text(4), "heroes/crusader/trinket.png");
    EXPECT_EQ(statement.column_int64(5), ddse::core::dson::string_hash("sample_token"));
    EXPECT_EQ(statement.column_text(6), "panels/icons_equip/trinket/inv_trinket+sample_token.png");
    EXPECT_EQ(statement.column_text(7), "crusader");
}

TEST(BaseContentDatabase, FailedRebuildLeavesExistingDatabaseByteIdentical) {
    TempDirectory temp;
    const auto path = temp.path / "base_content.db";
    ddse::infrastructure::BaseContentDatabaseBuilder builder;
    auto initial = builder.rebuild(path, sample_scan());
    ASSERT_TRUE(initial) << initial.error().message;
    const auto before = read_file(path);

    auto invalid = sample_scan();
    invalid.definitions.front().virtual_path = "missing/definition.json";
    auto failed = builder.rebuild(path, invalid);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ddse::core::ErrorCode::ValidationFailed);
    EXPECT_EQ(read_file(path), before);
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".new"));
}

TEST(BaseContentDatabase, RejectsOutputInsideScannedGameRootBeforeCreatingFiles) {
    TempDirectory temp;
    const auto game_root = temp.path / "game";
    std::filesystem::create_directories(game_root);
    const auto path = game_root / "base_content.db";
    auto scan = sample_scan();
    scan.sources.front().root_path = game_root.string();

    ddse::infrastructure::BaseContentDatabaseBuilder builder;
    auto built = builder.rebuild(path, scan);
    ASSERT_FALSE(built);
    EXPECT_EQ(built.error().code, ddse::core::ErrorCode::ValidationFailed);
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".new"));
}
