#include "ddse/application/mod_environment.hpp"
#include "ddse/infrastructure/base_content_database.hpp"
#include "ddse/infrastructure/mod_environment_database.hpp"
#include "ddse/infrastructure/native_file_system.hpp"
#include "ddse/infrastructure/sqlite_content_environment.hpp"
#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/statement.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("ddse-mod-env-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("failed to write test fixture");
}

void write_mod(const std::filesystem::path& root, std::string_view folder,
               std::string_view title, std::string_view published_id,
               std::string_view trinket_payload, std::string_view asset_payload) {
    const auto mod = root / folder;
    write_file(mod / "project.xml", "<Project><Title>" + std::string{title} + "</Title><PublishedFileId>" +
        std::string{published_id} + "</PublishedFileId><VersionMajor>1</VersionMajor><VersionMinor>2</VersionMinor></Project>");
    write_file(mod / "trinkets" / "sample.entries.trinkets.json", "[{\"id\":\"token\",\"payload\":\"" +
        std::string{trinket_payload} + "\"}]");
    write_file(mod / "heroes" / "shared" / "icon.png", asset_payload);
    write_file(mod / "localization" / "values.string_table.xml",
        "<root><language id=\"english\"><entry id=\"mod_test_key\">" +
        std::string{title} + "</entry></language></root>");
}

ddse::application::BaseContentScanResult base_content() {
    using namespace ddse::application;
    BaseContentScanResult scan;
    scan.sources.push_back({"vanilla", "Vanilla", ContentSourceType::Vanilla, "read-only-game-root"});
    scan.source_files.push_back({"vanilla", "trinkets/sample.entries.trinkets.json", ".json", 40, 1, false});
    scan.source_files.push_back({"vanilla", "heroes/shared/icon.png", ".png", 3, 0, true});
    scan.source_files.push_back({"vanilla", "localization/values.string_table.xml", ".xml", 80, 2, false});
    scan.source_files.push_back({"vanilla", "panels/base-only.png", ".png", 7, 0, true});
    scan.source_files.push_back({"vanilla", "heroes/crusader/crusader.info.darkest", ".darkest", 50, 3, false});
    scan.source_files.push_back({"vanilla", "heroes/crusader/base.png", ".png", 9, 0, true});
    scan.source_files.push_back({"vanilla", "panels/icons_equip/trinket/inv_trinket+token.png", ".png", 11, 0, true});
    scan.sources.push_back({"dlc:fixture_dlc", "Fixture DLC", ContentSourceType::Dlc, "read-only-dlc-root"});
    scan.source_files.push_back({"dlc:fixture_dlc", "trinkets/dlc.entries.trinkets.json", ".json", 30, 4, false});
    scan.definitions.push_back({"trinket", "token", "vanilla", "trinkets/sample.entries.trinkets.json",
                                {}, "str_inventory_title_trinkettoken", R"({"id":"token","payload":"base"})"});
    scan.definitions.push_back({"trinket", "base_only", "vanilla", "trinkets/sample.entries.trinkets.json",
                                {}, "str_inventory_title_trinketbase_only", R"({"id":"base_only"})"});
    scan.definitions.push_back({"hero_class", "crusader", "vanilla", "heroes/crusader/crusader.info.darkest",
                                {}, "hero_class_name_crusader", R"({"records":[]})"});
    scan.definitions.push_back({"skill", "crusader:smite", "vanilla", "heroes/crusader/crusader.info.darkest",
                                {}, "combat_skill_name_crusader_smite", R"({"levels":[{"level":["0"]}]})"});
    scan.definitions.push_back({"trinket", "dlc_only", "dlc:fixture_dlc", "trinkets/dlc.entries.trinkets.json",
                                {}, "str_inventory_title_trinketdlc_only", R"({"id":"dlc_only"})"});
    scan.localizations.push_back({"english", "mod_test_key", "Base", "vanilla", "localization/values.string_table.xml"});
    scan.assets.push_back({"vanilla", "heroes/shared/icon.png", ".png", 3});
    scan.assets.push_back({"vanilla", "panels/base-only.png", ".png", 7});
    scan.assets.push_back({"vanilla", "heroes/crusader/base.png", ".png", 9});
    scan.assets.push_back({"vanilla", "panels/icons_equip/trinket/inv_trinket+token.png", ".png", 11});
    scan.asset_references.push_back({"vanilla", "trinket", "token", "trinkets/sample.entries.trinkets.json",
        "inventory_icon", "file", "panels/icons_equip/trinket/inv_trinket+token.png", "convention"});
    scan.asset_references.push_back({"vanilla", "hero_class", "crusader", "heroes/crusader/crusader.info.darkest",
        "class_asset_bundle", "directory", "heroes/crusader/", "convention"});
    scan.relationships.push_back({"vanilla", "hero_class", "crusader", "combat_skill", "skill",
        "crusader:smite", "heroes/crusader/crusader.info.darkest"});
    return scan;
}

std::string column_text(ddse::infrastructure::sqlite::Database& database,
                        std::string_view sql, std::string_view parameter) {
    auto prepared = database.prepare(sql);
    if (!prepared) throw std::runtime_error(prepared.error().message);
    auto statement = std::move(prepared.value());
    auto bound = statement.bind(1, parameter);
    if (!bound) throw std::runtime_error(bound.error().message);
    auto row = statement.step();
    if (!row || !row.value()) throw std::runtime_error("query returned no row");
    return std::string{statement.column_text(0)};
}

std::int64_t column_int(ddse::infrastructure::sqlite::Database& database,
                        std::string_view sql, std::string_view parameter) {
    auto prepared = database.prepare(sql);
    if (!prepared) throw std::runtime_error(prepared.error().message);
    auto statement = std::move(prepared.value());
    auto bound = statement.bind(1, parameter);
    if (!bound) throw std::runtime_error(bound.error().message);
    auto row = statement.step();
    if (!row || !row.value()) throw std::runtime_error("query returned no row");
    return statement.column_int64(0);
}


TEST(ModEnvironment, ScansInstalledDisabledAndEnabledSourcesAndBuildsOverrideViews) {
    namespace fs = std::filesystem;
    TempDirectory temp;
    const auto workshop = temp.path / "workshop";
    const auto local_root = temp.path / "modes";
    fs::create_directories(workshop);
    fs::create_directories(local_root);
    write_mod(workshop, "1001", "Workshop One", "1001", "workshop", "workshop");
    write_mod(workshop, "1002", "Disabled Mod", "1002", "disabled", "disabled");
    write_mod(local_root, "local-class", "Local Class", "", "local", "local");
    // DD's content root is virtualized from the mod root; these similarly named folders are not sources.
    write_file(workshop / "1001" / "modes" / "ignored.json", "[]");
    write_file(workshop / "1001" / "mods" / "ignored.json", "[]");
    write_file(workshop / "1001" / "panels" / "mod-only.png", "new");
    write_file(workshop / "1001" / "trinkets" / "missing.entries.trinkets.json",
        R"([{"id":"missing","rarity":0}])");
    write_file(workshop / "1001" / "heroes" / "crusader" / "crusader.info.darkest",
        "combat_skill: .id \"smite\" .level 0 .type \"melee\"\n");
    write_file(workshop / "1001" / "heroes" / "crusader" / "crusader.ability.one.png", "class-skill-chart");
    const auto manager_json = temp.path / "Default.json";
    write_file(manager_json,
        R"({"version":2,"mods":[)"
        R"({"provider_id":"steam","external_id":"1001","name":"Workshop One","enabled":true},)"
        R"({"provider_id":"steam","external_id":"1002","name":"Disabled Mod","enabled":false},)"
        R"({"provider_id":"local","external_id":"","mod_guid":"local-guid","name":"Local Class","enabled":true}]})");

    ddse::infrastructure::NativeFileSystem file_system;
    ddse::application::ModEnvironmentScanConfig config;
    config.workshop_root = workshop;
    config.local_mod_roots.push_back(local_root);
    config.manager_order_json = manager_json;
    config.base_content_database = temp.path / "base-content.db";
    ddse::application::ModEnvironmentScanner scanner{file_system};
    auto scanned = scanner.scan(config);
    ASSERT_TRUE(scanned) << scanned.error().message;
    ASSERT_EQ(scanned.value().mods.size(), 3U);
    ASSERT_EQ(scanned.value().effective_order.size(), 2U);
    EXPECT_EQ(scanned.value().effective_order_source, "manager_export");
    EXPECT_EQ(scanned.value().effective_order[0], "steam:1001");
    EXPECT_EQ(scanned.value().effective_order[1], "local-path:0:local-class");
    EXPECT_EQ(scanned.value().content.definitions.size(), 6U);
    EXPECT_EQ(scanned.value().content.assets.size(), 5U);
    EXPECT_TRUE(std::any_of(scanned.value().content.asset_references.begin(), scanned.value().content.asset_references.end(), [](const auto& ref) {
        return ref.definition_type == "hero_class" && ref.content_id == "crusader" &&
               ref.source_id == "steam:1001" && ref.reference_type == "directory";
    }));
    EXPECT_EQ(scanned.value().content.localizations.size(), 3U);
    EXPECT_TRUE(std::none_of(scanned.value().content.source_files.begin(), scanned.value().content.source_files.end(),
        [](const auto& file) { return file.virtual_path.starts_with("modes/") || file.virtual_path.starts_with("mods/"); }));

    const auto base_database_path = temp.path / "base-content.db";
    auto base_built = ddse::infrastructure::BaseContentDatabaseBuilder{}.rebuild(base_database_path, base_content());
    ASSERT_TRUE(base_built) << base_built.error().message;
    const auto environment_database_path = temp.path / "environment.db";
    auto environment_built = ddse::infrastructure::ModEnvironmentDatabaseBuilder{}.rebuild(
        environment_database_path, base_database_path, scanned.value());
    ASSERT_TRUE(environment_built) << environment_built.error().message;
    EXPECT_EQ(environment_built.value().installed_mods, 3U);
    EXPECT_EQ(environment_built.value().enabled_mods, 2U);
    EXPECT_EQ(environment_built.value().workshop_mods, 2U);
    EXPECT_EQ(environment_built.value().local_mods, 1U);
    EXPECT_EQ(environment_built.value().overridden_paths, 4U);

    auto database_result = ddse::infrastructure::sqlite::ConnectionFactory{}.open(environment_database_path);
    ASSERT_TRUE(database_result) << database_result.error().message;
    auto& database = database_result.value();
    EXPECT_EQ(column_text(database,
        "SELECT winner_source_id FROM effective_vfs WHERE virtual_path=?", "heroes/shared/icon.png"), "steam:1001");
    EXPECT_EQ(column_int(database,
        "SELECT override_count FROM effective_vfs WHERE virtual_path=?", "heroes/shared/icon.png"), 2);
    EXPECT_EQ(column_text(database,
        "SELECT winner_mod_id FROM effective_definitions WHERE definition_type='trinket' AND content_id=?", "token"), "steam:1001");
    EXPECT_EQ(column_text(database,
        "SELECT winner_source_id FROM effective_assets WHERE virtual_path=?", "heroes/shared/icon.png"), "steam:1001");
    EXPECT_EQ(column_text(database,
        "SELECT winner_source_id FROM effective_localizations WHERE language='english' AND localization_key=?", "mod_test_key"), "steam:1001");
    EXPECT_EQ(column_text(database,
        "SELECT layer_type FROM effective_vfs WHERE virtual_path=?", "panels/base-only.png"), "base");
    EXPECT_EQ(column_text(database,
        "SELECT winner_source_id FROM effective_vfs WHERE virtual_path=?", "panels/mod-only.png"), "steam:1001");
    EXPECT_EQ(column_int(database,
        "SELECT count(*) FROM effective_vfs WHERE virtual_path=?", "trinkets/sample.entries.trinkets.json"), 1);
    EXPECT_EQ(column_int(database,
        "SELECT count(*) FROM mod_sources WHERE enabled=0 AND mod_id=?", "steam:1002"), 1);
    EXPECT_EQ(column_int(database,
        "SELECT count(*) FROM content_asset_references WHERE definition_type='hero_class' AND content_id=?", "crusader"), 2);

    ddse::infrastructure::SqliteContentEnvironment environment({
        base_database_path, environment_database_path,
        ddse::application::ContentEnvironmentSelection{"schinese", "english", {}}});
    auto hero_bundle = environment.load_bundle("hero_class", "crusader");
    ASSERT_TRUE(hero_bundle) << hero_bundle.error().message;
    ASSERT_TRUE(hero_bundle.value());
    EXPECT_EQ(hero_bundle.value()->definition.provenance.source_id, "steam:1001");
    ASSERT_EQ(hero_bundle.value()->related_definitions.size(), 1U);
    EXPECT_EQ(hero_bundle.value()->related_definitions.front().id, "crusader:smite");
    EXPECT_EQ(hero_bundle.value()->related_definitions.front().provenance.source_id, "steam:1001");
    EXPECT_TRUE(std::any_of(hero_bundle.value()->assets.begin(), hero_bundle.value()->assets.end(), [](const auto& asset) {
        return asset.virtual_path == "heroes/crusader/crusader.ability.one.png" && asset.source_id == "steam:1001";
    }));
    EXPECT_TRUE(std::any_of(hero_bundle.value()->assets.begin(), hero_bundle.value()->assets.end(), [](const auto& asset) {
        return asset.virtual_path == "heroes/crusader/base.png" && asset.source_id == "vanilla";
    }));
    auto listed_hero = environment.list_content("hero_class", "crusader");
    ASSERT_TRUE(listed_hero) << listed_hero.error().message;
    ASSERT_EQ(listed_hero.value().size(), 1U);
    EXPECT_EQ(listed_hero.value().front().id, hero_bundle.value()->definition.id);
    EXPECT_EQ(listed_hero.value().front().provenance.source_id,
              hero_bundle.value()->definition.provenance.source_id);

    auto token = environment.find_content("trinket", "token");
    ASSERT_TRUE(token) << token.error().message;
    ASSERT_TRUE(token.value());
    EXPECT_EQ(token.value()->provenance.source_id, "steam:1001");
    ASSERT_EQ(token.value()->asset_references.size(), 1U);
    ASSERT_TRUE(token.value()->asset_references.front().resolved_asset);
    EXPECT_EQ(token.value()->asset_references.front().resolved_asset->source_id, "vanilla");
    auto missing_icon_content = environment.find_content("trinket", "missing");
    ASSERT_TRUE(missing_icon_content) << missing_icon_content.error().message;
    ASSERT_TRUE(missing_icon_content.value());
    ASSERT_EQ(missing_icon_content.value()->asset_references.size(), 1U);
    EXPECT_FALSE(missing_icon_content.value()->asset_references.front().resolved_asset);
    EXPECT_FALSE(environment.find_content("trinket", "dlc_only").value());
    auto with_dlc = ddse::infrastructure::SqliteContentEnvironment({
        base_database_path, environment_database_path,
        ddse::application::ContentEnvironmentSelection{"english", "english", {"dlc:fixture_dlc"}}});
    auto dlc_definition = with_dlc.find_content("trinket", "dlc_only");
    ASSERT_TRUE(dlc_definition) << dlc_definition.error().message;
    ASSERT_TRUE(dlc_definition.value());
    EXPECT_EQ(dlc_definition.value()->provenance.source_id, "dlc:fixture_dlc");
    auto localized = environment.resolve_localization("mod_test_key");
    ASSERT_TRUE(localized) << localized.error().message;
    ASSERT_TRUE(localized.value());
    EXPECT_EQ(localized.value()->value, "Workshop One");
    EXPECT_EQ(localized.value()->resolved_language, "english");
    auto provenance = environment.explain_provenance("trinket", "token");
    ASSERT_TRUE(provenance) << provenance.error().message;
    ASSERT_GE(provenance.value().size(), 2U);
    EXPECT_EQ(provenance.value().front().source_id, "steam:1001");
    EXPECT_TRUE(provenance.value().front().selected);
}

TEST(ModEnvironment, ComparesManagerExportWithSaveOrderFromProfile) {
    namespace fs = std::filesystem;
    TempDirectory temp;
    const auto workshop = temp.path / "workshop";
    const auto local_root = temp.path / "modes";
    fs::create_directories(workshop);
    fs::create_directories(local_root);
    ddse::infrastructure::NativeFileSystem file_system;
    ddse::application::ModEnvironmentScanConfig config;
    config.workshop_root = workshop;
    config.local_mod_roots.push_back(local_root);
    config.save_profile_root = DDSE_TEST_SAVE_PROFILE_DIR;

    ddse::application::ModEnvironmentScanner scanner{file_system};
    auto from_save = scanner.scan(config);
    ASSERT_TRUE(from_save) << from_save.error().message;
    ASSERT_EQ(from_save.value().save_order.size(), 122U);
    std::size_t steam_count = 0;
    std::size_t local_count = 0;
    std::string json = R"({"version":2,"mods":[)";
    for (std::size_t i = 0; i < from_save.value().save_order.size(); ++i) {
        const auto& entry = from_save.value().save_order[i];
        if (entry.provider_id == "steam") ++steam_count;
        else ++local_count;
        if (i != 0) json += ',';
        std::ostringstream item;
        item << "{\"provider_id\":" << std::quoted(entry.provider_id)
             << ",\"external_id\":" << std::quoted(entry.external_id)
             << ",\"name\":" << std::quoted(entry.name)
             << ",\"mod_guid\":\"fixture\",\"enabled\":true}";
        json += item.str();
    }
    json += "]}";
    const auto manager_json = temp.path / "Default.json";
    write_file(manager_json, json);
    config.manager_order_json = manager_json;

    auto compared = scanner.scan(config);
    ASSERT_TRUE(compared) << compared.error().message;
    EXPECT_EQ(steam_count, 93U);
    EXPECT_EQ(local_count, 29U);
    EXPECT_TRUE(compared.value().comparison.save_available);
    EXPECT_TRUE(compared.value().comparison.manager_export_available);
    EXPECT_TRUE(compared.value().comparison.exact_enabled_order_match);
    EXPECT_TRUE(compared.value().comparison.shared_enabled_order_match);
    EXPECT_TRUE(compared.value().comparison.only_in_save.empty());
    EXPECT_TRUE(compared.value().comparison.only_in_manager_export.empty());
}

TEST(ModEnvironment, IsolatesMalformedContentAcrossTwoHundredInstalledMods) {
    namespace fs = std::filesystem;
    TempDirectory temp;
    const auto workshop = temp.path / "workshop";
    fs::create_directories(workshop);
    for (int i = 0; i < 200; ++i) {
        const auto id = std::to_string(1000 + i);
        const auto mod = workshop / id;
        write_file(mod / "project.xml", "<Project><Title>Mod " + id + "</Title><PublishedFileId>" + id +
            "</PublishedFileId></Project>");
        write_file(mod / "shared" / "indexed.json", "{}");
        if (i == 100) write_file(mod / "trinkets" / "broken.entries.trinkets.json", "{not json");
    }
    const auto manager_json = temp.path / "Default.json";
    write_file(manager_json,
        R"({"mods":[{"provider_id":"steam","external_id":"1000","name":"Mod 1000","enabled":true}]})");
    ddse::infrastructure::NativeFileSystem file_system;
    ddse::application::ModEnvironmentScanConfig config;
    config.workshop_root = workshop;
    config.manager_order_json = manager_json;

    auto scanned = ddse::application::ModEnvironmentScanner{file_system}.scan(config);
    ASSERT_TRUE(scanned) << scanned.error().message;
    EXPECT_EQ(scanned.value().mods.size(), 200U);
    ASSERT_EQ(scanned.value().effective_order.size(), 1U);
    EXPECT_EQ(scanned.value().effective_order.front(), "steam:1000");
    EXPECT_TRUE(std::any_of(scanned.value().content.diagnostics.begin(), scanned.value().content.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.source_id == "steam:1100" && diagnostic.virtual_path == "trinkets/broken.entries.trinkets.json";
        }));
    EXPECT_TRUE(std::any_of(scanned.value().content.source_files.begin(), scanned.value().content.source_files.end(),
        [](const auto& file) { return file.source_id == "steam:1199" && file.virtual_path == "shared/indexed.json"; }));
}

TEST(ModEnvironment, RejectsOutputDatabaseInsideConfiguredReadOnlyRoots) {
    namespace fs = std::filesystem;
    TempDirectory temp;
    const auto protected_root = temp.path / "workshop";
    fs::create_directories(protected_root);
    ddse::application::ModEnvironmentScanResult scan;
    scan.protected_roots.push_back(protected_root);
    const auto base_database_path = temp.path / "base-content.db";
    auto base_built = ddse::infrastructure::BaseContentDatabaseBuilder{}.rebuild(base_database_path, base_content());
    ASSERT_TRUE(base_built) << base_built.error().message;

    const auto output = protected_root / "environment.db";
    auto built = ddse::infrastructure::ModEnvironmentDatabaseBuilder{}.rebuild(output, base_database_path, scan);
    ASSERT_FALSE(built);
    EXPECT_EQ(built.error().code, ddse::core::ErrorCode::ValidationFailed);
    EXPECT_FALSE(fs::exists(output));
    EXPECT_FALSE(fs::exists(output.string() + ".new"));
}

TEST(ModEnvironment, RejectsTemporaryReplacementThatWouldOverwriteConfiguredInputFile) {
    namespace fs = std::filesystem;
    TempDirectory temp;
    const auto protected_file = temp.path / "manager-export.json.new";
    write_file(protected_file, "preserve this input");
    ddse::application::ModEnvironmentScanResult scan;
    scan.protected_roots.push_back(protected_file);
    const auto base_database_path = temp.path / "base-content.db";
    auto base_built = ddse::infrastructure::BaseContentDatabaseBuilder{}.rebuild(base_database_path, base_content());
    ASSERT_TRUE(base_built) << base_built.error().message;

    const auto output = temp.path / "manager-export.json";
    auto built = ddse::infrastructure::ModEnvironmentDatabaseBuilder{}.rebuild(output, base_database_path, scan);
    ASSERT_FALSE(built);
    EXPECT_EQ(built.error().code, ddse::core::ErrorCode::ValidationFailed);
    EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(std::filesystem::file_size(protected_file), 19U);
    std::ifstream input(protected_file, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    EXPECT_EQ(bytes, "preserve this input");
}

} // namespace
