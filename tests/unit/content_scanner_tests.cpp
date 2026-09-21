#include "ddse/application/content_scanner.hpp"
#include "ddse/infrastructure/native_file_system.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

namespace {

struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("ddse-content-scan-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void put(ddse::infrastructure::NativeFileSystem& fs, const std::filesystem::path& root,
         std::string_view relative, const std::string& bytes) {
    const auto path = root / relative;
    std::filesystem::create_directories(path.parent_path());
    auto result = fs.write_file(path, bytes);
    ASSERT_TRUE(result) << result.error().message;
}

const ddse::application::ScannedDefinition* find_definition(
    const ddse::application::BaseContentScanResult& scan, std::string_view kind, std::string_view id) {
    const auto found = std::find_if(scan.definitions.begin(), scan.definitions.end(), [&](const auto& definition) {
        return definition.kind == kind && definition.id == id;
    });
    return found == scan.definitions.end() ? nullptr : &*found;
}

} // namespace

TEST(BaseContentScanner, ParsesDefinitionsLocalizationAssetsAndSeparatesDlcSources) {
    using namespace ddse::application;
    ddse::infrastructure::NativeFileSystem fs;
    TempDirectory temp;
    const auto game = temp.path / "game";
    std::filesystem::create_directories(game);
    auto config = BaseContentScanConfig::defaults(game);
    config.vanilla_directories = {"heroes", "trinkets", "shared", "campaign", "localization", "inventory", "panels", "mods", "modes"};

    put(fs, game, "heroes/crusader/crusader.info.darkest",
        "resistances: .stun 40% .poison 30%\n"
        "combat_skill: .id \"smite\" .level 0 .type \"melee\"\n"
        "combat_skill: .id \"smite\" .level 1 .type \"melee\"\n");
    put(fs, game, "trinkets/base.entries.trinkets.json",
        R"({"entries":[{"id":"test_trinket","price":100,"name":"Test","hero_class_requirements":["crusader"]}]})");
    put(fs, game, "shared/quirk/quirk_library.json",
        R"({"quirks":[{"id":"tough","is_disease":false},{"id":"rabies","is_disease":true}]})");
    put(fs, game, "campaign/town/buildings/abbey/abbey.building.json", R"({"activities":[]})");
    put(fs, game, "localization/heroes.string_table.xml",
        "<?xml version=\"1.0\"?><root><language id=\"english\">"
        "<entry id=\"hero_class_name_crusader\"><![CDATA[Crusader &amp; Co]]></entry>"
        "<entry id=\"combat_skill_name_crusader_smite\">Smite &amp; Strike</entry>"
        "<entry id=\"empty_value\" />"
        "</language><language id=\"schinese\"><entry id=\"hero_class_name_crusader\">十字军</entry>"
        "</language><language id=\"japanese\" /></root>");
    put(fs, game, "inventory/base.currency.inventory.items.darkest",
        "inventory_item: .type \"gold\" .id \"\" .base_stack_limit 1750\n"
        "inventory_item: .type \"heirloom\" .id \"portrait\" .base_stack_limit 3\n");
    put(fs, game, "heroes/crusader/crusader.sprite.png", "png-metadata-only");
    put(fs, game, "heroes/crusader/crusader.camping_skills.json",
        R"({"skills":[{"id":"stand_tall","buffs":[]}]})");
    put(fs, game, "panels/icons_equip/trinket/inv_trinket+test_trinket.png", "trinket-icon");
    put(fs, game, "mods/not_scanned/secret.entries.trinkets.json", R"({"entries":[{"id":"mod_only"}]})");
    put(fs, game, "modes/not_scanned/secret.entries.trinkets.json", R"({"entries":[{"id":"mode_only"}]})");

    put(fs, game, "dlc/580100_crimson_court/heroes/flagellant/flagellant.info.darkest",
        "combat_skill: .id \"exsanguinate\" .level 0\n");
    put(fs, game, "dlc/580100_crimson_court/trinkets/crimson_court.entries.trinkets.json",
        R"({"entries":[{"id":"cc_trinket","price":1}]})");
    put(fs, game, "dlc/580100_crimson_court/features/crimson/inventory/cc.currency.inventory.items.darkest",
        "inventory_item: .type \"blood\" .id \"\" .base_stack_limit 99\n");

    BaseContentScanner scanner{fs};
    auto first = scanner.scan(config);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first.value().diagnostics.empty());
    ASSERT_EQ(first.value().sources.size(), 2U);
    EXPECT_EQ(first.value().sources[0].id, "vanilla");
    EXPECT_EQ(first.value().sources[1].id, "dlc:580100_crimson_court");
    ASSERT_EQ(first.value().assets.size(), 2U);
    EXPECT_TRUE(std::any_of(first.value().assets.begin(), first.value().assets.end(), [](const auto& asset) {
        return asset.virtual_path == "heroes/crusader/crusader.sprite.png";
    }));

    const auto* hero = find_definition(first.value(), "hero_class", "crusader");
    ASSERT_NE(hero, nullptr);
    EXPECT_EQ(hero->source_id, "vanilla");
    EXPECT_EQ(hero->localization_key, "hero_class_name_crusader");
    EXPECT_NE(find_definition(first.value(), "hero_class", "flagellant"), nullptr);
    EXPECT_NE(find_definition(first.value(), "skill", "crusader:smite"), nullptr);
    EXPECT_NE(find_definition(first.value(), "skill", "flagellant:exsanguinate"), nullptr);
    EXPECT_NE(find_definition(first.value(), "skill", "crusader:stand_tall"), nullptr);
    EXPECT_NE(find_definition(first.value(), "trinket", "test_trinket"), nullptr);
    EXPECT_NE(find_definition(first.value(), "trinket", "cc_trinket"), nullptr);
    EXPECT_NE(find_definition(first.value(), "quirk", "tough"), nullptr);
    EXPECT_NE(find_definition(first.value(), "disease", "rabies"), nullptr);
    EXPECT_NE(find_definition(first.value(), "building", "abbey"), nullptr);
    EXPECT_NE(find_definition(first.value(), "resource", "heirloom:portrait"), nullptr);
    EXPECT_NE(find_definition(first.value(), "resource", "blood"), nullptr);
    EXPECT_FALSE(hero->payload_json.empty());
    EXPECT_TRUE(std::any_of(first.value().asset_references.begin(), first.value().asset_references.end(), [](const auto& ref) {
        return ref.definition_type == "hero_class" && ref.content_id == "crusader" &&
               ref.reference_type == "directory" && ref.virtual_path == "heroes/crusader/";
    }));
    EXPECT_TRUE(std::any_of(first.value().asset_references.begin(), first.value().asset_references.end(), [](const auto& ref) {
        return ref.definition_type == "trinket" && ref.content_id == "test_trinket" &&
               ref.virtual_path == "panels/icons_equip/trinket/inv_trinket+test_trinket.png";
    }));
    EXPECT_TRUE(std::any_of(first.value().relationships.begin(), first.value().relationships.end(), [](const auto& relation) {
        return relation.parent_type == "hero_class" && relation.parent_id == "crusader" &&
               relation.relationship_type == "camping_skill" && relation.child_id == "crusader:stand_tall";
    }));
    EXPECT_TRUE(std::any_of(first.value().relationships.begin(), first.value().relationships.end(), [](const auto& relation) {
        return relation.parent_type == "trinket" && relation.parent_id == "test_trinket" &&
               relation.relationship_type == "restricted_to" && relation.child_id == "crusader";
    }));
    EXPECT_EQ(find_definition(first.value(), "trinket", "mod_only"), nullptr);
    EXPECT_EQ(find_definition(first.value(), "trinket", "mode_only"), nullptr);

    const auto localized = std::find_if(first.value().localizations.begin(), first.value().localizations.end(),
        [](const auto& entry) { return entry.key == "hero_class_name_crusader"; });
    ASSERT_NE(localized, first.value().localizations.end());
    EXPECT_EQ(localized->value, "Crusader &amp; Co"); // CDATA content is literal text
    const auto decoded = std::find_if(first.value().localizations.begin(), first.value().localizations.end(),
        [](const auto& entry) { return entry.key == "combat_skill_name_crusader_smite"; });
    ASSERT_NE(decoded, first.value().localizations.end());
    EXPECT_EQ(decoded->value, "Smite & Strike");
    const auto localized_zh = std::find_if(first.value().localizations.begin(), first.value().localizations.end(),
        [](const auto& entry) { return entry.key == "hero_class_name_crusader" && entry.language == "schinese"; });
    ASSERT_NE(localized_zh, first.value().localizations.end());
    EXPECT_EQ(localized_zh->value, "十字军");
    const auto empty_localized = std::find_if(first.value().localizations.begin(), first.value().localizations.end(),
        [](const auto& entry) { return entry.key == "empty_value" && entry.language == "english"; });
    ASSERT_NE(empty_localized, first.value().localizations.end());
    EXPECT_TRUE(empty_localized->value.empty());

    auto second = scanner.scan(config);
    ASSERT_TRUE(second);
    ASSERT_EQ(second.value().source_files.size(), first.value().source_files.size());
    ASSERT_EQ(second.value().definitions.size(), first.value().definitions.size());
    for (std::size_t i = 0; i < first.value().source_files.size(); ++i) {
        EXPECT_EQ(first.value().source_files[i].source_id, second.value().source_files[i].source_id);
        EXPECT_EQ(first.value().source_files[i].virtual_path, second.value().source_files[i].virtual_path);
    }
}

TEST(BaseContentScanner, ParserErrorsAreIsolatedAndReported) {
    using namespace ddse::application;
    ddse::infrastructure::NativeFileSystem fs;
    TempDirectory temp;
    const auto game = temp.path / "game";
    std::filesystem::create_directories(game);
    auto config = BaseContentScanConfig::defaults(game);
    config.vanilla_directories = {"trinkets", "localization"};
    put(fs, game, "trinkets/broken.entries.trinkets.json", "{ invalid json }");
    put(fs, game, "localization/broken.string_table.xml", "<root><language id=\"english\"><entry id=\"x\">");
    put(fs, game, "trinkets/good.entries.trinkets.json", R"({"entries":[{"id":"still_available"}]})");

    BaseContentScanner scanner{fs};
    auto result = scanner.scan(config);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().diagnostics.size(), 2U);
    EXPECT_NE(find_definition(result.value(), "trinket", "still_available"), nullptr);
}

TEST(BaseContentScanner, DefaultConfigurationNeverSelectsModesOrLocalMods) {
    const auto config = ddse::application::BaseContentScanConfig::defaults("D:/game");
    EXPECT_EQ(std::find(config.vanilla_directories.begin(), config.vanilla_directories.end(), "mods"),
              config.vanilla_directories.end());
    EXPECT_EQ(std::find(config.vanilla_directories.begin(), config.vanilla_directories.end(), "modes"),
              config.vanilla_directories.end());
    EXPECT_NE(std::find(config.vanilla_directories.begin(), config.vanilla_directories.end(), "scripts"),
              config.vanilla_directories.end());
    EXPECT_NE(std::find(config.excluded_directories.begin(), config.excluded_directories.end(), "mods"),
              config.excluded_directories.end());
    EXPECT_NE(std::find(config.excluded_directories.begin(), config.excluded_directories.end(), "modes"),
              config.excluded_directories.end());
}
