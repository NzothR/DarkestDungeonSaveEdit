#include "ddse/application/campaign_model_builder.hpp"
#include "ddse/application/campaign_mappings.hpp"
#include "ddse/application/campaign_edit_session.hpp"
#include "ddse/application/save_profile.hpp"
#include "ddse/infrastructure/native_file_system.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace {

class FixtureContentEnvironment final : public ddse::application::IContentEnvironment {
public:
    std::string unresolved_type;
    std::string unresolved_id;

    ddse::core::Result<std::optional<ddse::application::ContentDefinition>, ddse::core::Error>
    find_content(std::string_view type, std::string_view id) const override {
        if (type == unresolved_type && id == unresolved_id)
            return ddse::core::Result<std::optional<ddse::application::ContentDefinition>, ddse::core::Error>::success(std::nullopt);
        if (type == "quirk" && id.starts_with("disease_"))
            return ddse::core::Result<std::optional<ddse::application::ContentDefinition>, ddse::core::Error>::success(std::nullopt);
        if (type == "disease" && !id.starts_with("disease_"))
            return ddse::core::Result<std::optional<ddse::application::ContentDefinition>, ddse::core::Error>::success(std::nullopt);

        ddse::application::ContentDefinition definition;
        definition.type = type;
        definition.id = id;
        definition.localized_name = std::string{type} + " display " + std::string{id};
        definition.payload_json = type == "quirk" ? R"({"is_positive":true})" :
            type == "disease" ? R"({"is_positive":false,"is_disease":true})" : "{}";
        definition.provenance = {"fixture-source", "mod", "fixture/" + std::string{type} + "/" + std::string{id}, true};
        if (type == "hero_class") {
            definition.asset_references.push_back({"class_asset_bundle", "directory", "convention",
                "heroes/" + std::string{id} + "/", ddse::application::ResolvedAsset{
                    "heroes/" + std::string{id} + "/" + std::string{id} + ".sprite.png",
                    "mod", "fixture-source", ".png", 64, "asset", "fixture/hero.sprite.png"}});
        }
        return ddse::core::Result<std::optional<ddse::application::ContentDefinition>, ddse::core::Error>::success(
            std::move(definition));
    }

    ddse::core::Result<std::vector<ddse::application::ContentDefinition>, ddse::core::Error>
    list_content(std::string_view, std::string_view) const override {
        return ddse::core::Result<std::vector<ddse::application::ContentDefinition>, ddse::core::Error>::success({});
    }

    ddse::core::Result<std::optional<ddse::application::ContentBundle>, ddse::core::Error>
    load_bundle(std::string_view type, std::string_view id) const override {
        auto definition = find_content(type, id);
        if (!definition)
            return ddse::core::Result<std::optional<ddse::application::ContentBundle>, ddse::core::Error>::failure(definition.error());
        if (!definition.value())
            return ddse::core::Result<std::optional<ddse::application::ContentBundle>, ddse::core::Error>::success(std::nullopt);
        ddse::application::ContentBundle bundle;
        bundle.definition = std::move(*definition.value());
        return ddse::core::Result<std::optional<ddse::application::ContentBundle>, ddse::core::Error>::success(
            std::move(bundle));
    }

    ddse::core::Result<std::optional<ddse::application::ResolvedLocalization>, ddse::core::Error>
    resolve_localization(std::string_view, std::string_view) const override {
        return ddse::core::Result<std::optional<ddse::application::ResolvedLocalization>, ddse::core::Error>::success(std::nullopt);
    }

    ddse::core::Result<std::optional<ddse::application::ResolvedAsset>, ddse::core::Error>
    resolve_asset(std::string_view) const override {
        return ddse::core::Result<std::optional<ddse::application::ResolvedAsset>, ddse::core::Error>::success(std::nullopt);
    }

    ddse::core::Result<std::vector<ddse::application::ContentProvenance>, ddse::core::Error>
    explain_provenance(std::string_view, std::string_view) const override {
        return ddse::core::Result<std::vector<ddse::application::ContentProvenance>, ddse::core::Error>::success({});
    }
};

ddse::application::RawSaveProfile load_profile() {
    ddse::infrastructure::NativeFileSystem file_system;
    ddse::application::SaveProfileDiscovery discovery{file_system};
    auto profile = discovery.load(DDSE_TEST_SAVE_PROFILE_DIR);
    if (!profile) throw std::runtime_error(profile.error().message);
    return std::move(profile.value());
}

std::size_t saved_hero_count(const ddse::application::RawSaveProfile& profile) {
    const auto& roster = profile.documents.at("persist.roster.json");
    if (!roster.decoded) return 0;
    const auto heroes = std::find_if(roster.decoded->fields.begin(), roster.decoded->fields.end(), [](const auto& field) {
        return field.path == "base_root/heroes";
    });
    if (heroes == roster.decoded->fields.end()) return 0;
    return static_cast<std::size_t>(std::count_if(heroes->children.begin(), heroes->children.end(), [&](const auto index) {
        return index < roster.decoded->fields.size() && roster.decoded->fields[index].kind == ddse::core::dson::ValueKind::Object;
    }));
}

std::optional<std::pair<std::string, std::string>> first_hero_class(
    const ddse::application::RawSaveProfile& profile) {
    const auto& roster = profile.documents.at("persist.roster.json");
    if (!roster.decoded) return std::nullopt;
    for (const auto& field : roster.decoded->fields) {
        if (field.name != "raw_data" || field.kind != ddse::core::dson::ValueKind::EmbeddedDson ||
            !field.embedded_document) continue;
        const auto class_field = std::find_if(field.embedded_document->fields.begin(), field.embedded_document->fields.end(),
            [](const auto& item) { return item.path == "base_root/heroClass"; });
        if (class_field == field.embedded_document->fields.end()) continue;
        const auto* class_id = std::get_if<std::string>(&class_field->value);
        if (!class_id) continue;
        const auto slash = field.path.find("/hero_file_data/raw_data");
        if (slash == std::string::npos) continue;
        const auto separator = field.path.find_last_of('/', slash - 1);
        if (separator == std::string::npos) continue;
        return std::pair{field.path.substr(separator + 1, slash - separator - 1), *class_id};
    }
    return std::nullopt;
}

void break_first_hero_body(ddse::application::RawSaveProfile& profile) {
    auto& document = *profile.documents.at("persist.roster.json").decoded;
    const auto raw_data = std::find_if(document.fields.begin(), document.fields.end(), [](const auto& field) {
        return field.path.find("/hero_file_data/raw_data") != std::string::npos &&
               field.kind == ddse::core::dson::ValueKind::EmbeddedDson;
    });
    ASSERT_NE(raw_data, document.fields.end());
    raw_data->embedded_document.reset();
}

bool locator_points_to_field(const ddse::application::RawSaveProfile& profile,
                             const ddse::domain::RawLocator& locator,
                             std::string_view expected_field_name) {
    const auto found = profile.documents.find(locator.document_id);
    if (found == profile.documents.end() || !found->second.decoded) return false;
    const ddse::core::dson::DsonDocument* document = &*found->second.decoded;
    const ddse::core::dson::DsonField* field = nullptr;
    for (const auto& step : locator.steps) {
        if (step.field_index >= document->fields.size()) return false;
        field = &document->fields[step.field_index];
        if (field->name != step.field_name) return false;
        if (step.enters_embedded_document) {
            if (!field->embedded_document) return false;
            document = field->embedded_document.get();
        }
    }
    return field && field->name == expected_field_name;
}

} // namespace

TEST(Stage8CampaignModel, ProjectsProfileSummaryAndKeepsRawLocatorsAndContentProvenance) {
    const auto profile = load_profile();
    FixtureContentEnvironment content;
    const auto model = ddse::application::CampaignModelBuilder{}.build(profile, content);
    const auto expected_hero_count = saved_hero_count(profile);

    EXPECT_EQ(model.summary.profile_id, "profile_0");
    EXPECT_EQ(model.summary.document_count, profile.documents.size());
    EXPECT_GT(expected_hero_count, 0U);
    EXPECT_EQ(model.heroes.size(), expected_hero_count);
    EXPECT_EQ(model.resources.size(), 8U);
    EXPECT_GT(model.trinket_inventory.size(), 0U);
    EXPECT_GT(model.town_buildings.size(), 0U);
    EXPECT_EQ(model.summary.district_count, model.districts.size());
    EXPECT_EQ(model.summary.hero_count, model.heroes.size());
    EXPECT_EQ(model.summary.resource_count, model.resources.size());
    EXPECT_EQ(model.summary.trinket_inventory_count, model.trinket_inventory.size());
    EXPECT_TRUE(model.read_only);

    const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [](const auto& item) {
        return item.class_id.value && item.name.value && item.definition.state == ddse::domain::EntityState::Resolved;
    });
    ASSERT_NE(hero, model.heroes.end());
    EXPECT_FALSE(hero->persistent_id.empty());
    EXPECT_FALSE(hero->resolve_xp.value == std::nullopt);
    EXPECT_FALSE(hero->level.value);  // No verified effective XP threshold model exists yet.
    EXPECT_FALSE(hero->definition.display_name.empty());
    EXPECT_EQ(hero->definition.source_id, "fixture-source");
    ASSERT_FALSE(hero->definition.assets.empty());
    EXPECT_TRUE(hero->definition.assets.front().resolved);
    ASSERT_TRUE(hero->name.raw);
    EXPECT_NE(hero->name.raw->display_path.find(" => base_root/actor/name"), std::string::npos);
    EXPECT_TRUE(locator_points_to_field(profile, *hero->name.raw, "name"));

    EXPECT_FALSE(model.resources.empty());
    EXPECT_TRUE(model.resources.front().id.raw.has_value());
    EXPECT_TRUE(model.resources.front().amount.raw.has_value());
    EXPECT_TRUE(locator_points_to_field(profile, *model.resources.front().amount.raw, "amount"));
    EXPECT_EQ(model.summary.diagnostic_count, model.diagnostics.size());
}

TEST(Stage8CampaignModel, MissingModHeroDefinitionPreservesIdAndMarksOnlyThatHeroPartial) {
    const auto profile = load_profile();
    const auto first = first_hero_class(profile);
    ASSERT_TRUE(first);
    FixtureContentEnvironment content;
    content.unresolved_type = "hero_class";
    content.unresolved_id = first->second;

    const auto model = ddse::application::CampaignModelBuilder{}.build(profile, content);
    const auto expected_hero_count = saved_hero_count(profile);
    ASSERT_GT(expected_hero_count, 0U);
    ASSERT_EQ(model.heroes.size(), expected_hero_count);
    const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
        return item.persistent_id == first->first;
    });
    ASSERT_NE(hero, model.heroes.end());
    EXPECT_EQ(hero->class_id.value, first->second);
    EXPECT_EQ(hero->definition.raw_id, first->second);
    EXPECT_EQ(hero->definition.state, ddse::domain::EntityState::Unresolved);
    EXPECT_EQ(hero->state, ddse::domain::EntityState::Partial);
    EXPECT_EQ(model.state, ddse::domain::ModelState::Partial);
    EXPECT_EQ(model.summary.unresolved_hero_count, 1U);
    EXPECT_EQ(model.summary.hero_count, expected_hero_count);
}

TEST(Stage8CampaignModel, OneMalformedHeroDoesNotBlockTheRemainingRoster) {
    auto profile = load_profile();
    break_first_hero_body(profile);
    FixtureContentEnvironment content;

    const auto model = ddse::application::CampaignModelBuilder{}.build(profile, content);
    const auto expected_hero_count = saved_hero_count(profile);
    ASSERT_GT(expected_hero_count, 0U);
    ASSERT_EQ(model.heroes.size(), expected_hero_count);
    EXPECT_EQ(model.state, ddse::domain::ModelState::Partial);
    EXPECT_EQ(model.summary.unresolved_hero_count, 1U);
    EXPECT_EQ(std::count_if(model.heroes.begin(), model.heroes.end(), [](const auto& hero) {
        return hero.state == ddse::domain::EntityState::Resolved;
    }), static_cast<std::ptrdiff_t>(expected_hero_count - 1));
    EXPECT_TRUE(std::any_of(model.diagnostics.begin(), model.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "hero.embedded_save_missing";
    }));
}

TEST(Stage8CampaignModel, AMalformedWalletEntryDoesNotHideOtherResources) {
    auto profile = load_profile();
    auto& estate = *profile.documents.at("persist.estate.json").decoded;
    const auto amount = std::find_if(estate.fields.begin(), estate.fields.end(), [](const auto& field) {
        return field.path == "base_root/wallet/0/amount";
    });
    ASSERT_NE(amount, estate.fields.end());
    amount->value = true;  // Preserve the wrong payload shape to exercise tolerant mapping diagnostics.
    FixtureContentEnvironment content;

    const auto model = ddse::application::CampaignModelBuilder{}.build(profile, content);
    ASSERT_EQ(model.resources.size(), 8U);
    const auto malformed = std::find_if(model.resources.begin(), model.resources.end(), [](const auto& item) {
        return item.index == 0;
    });
    ASSERT_NE(malformed, model.resources.end());
    EXPECT_EQ(malformed->state, ddse::domain::EntityState::Invalid);
    EXPECT_FALSE(malformed->amount.value);
    EXPECT_TRUE(malformed->amount.raw);
    EXPECT_EQ(std::count_if(model.resources.begin(), model.resources.end(), [](const auto& item) {
        return item.state == ddse::domain::EntityState::Resolved;
    }), 7);
    EXPECT_EQ(model.state, ddse::domain::ModelState::Partial);
}

TEST(Stage8CampaignModel, MappingRegistryMarksOnlyGameVerifiedOperationsForCommit) {
    const auto& mappings = ddse::application::stage8_campaign_mappings();
    ASSERT_GE(mappings.size(), 10U);
    EXPECT_TRUE(std::any_of(mappings.begin(), mappings.end(), [](const auto& mapping) {
        return mapping.semantic_property == "Hero.Quirks" && mapping.raw_path_template.find("{quirkId}") != std::string::npos;
    }));
    EXPECT_TRUE(std::any_of(mappings.begin(), mappings.end(), [](const auto& mapping) {
        return mapping.semantic_property == "TrinketInventory.Items";
    }));
    const auto quirk_lock = ddse::application::find_campaign_mapping("Hero.Quirk.Locked");
    const auto quirk_remove = ddse::application::find_campaign_mapping("Hero.Quirk.Entry");
    const auto trinket_destroy = ddse::application::find_campaign_mapping("TrinketInventory.Entry");
    const auto district_built = ddse::application::find_campaign_mapping("Town.District.Built");
    ASSERT_NE(quirk_lock, nullptr);
    ASSERT_NE(quirk_remove, nullptr);
    ASSERT_NE(trinket_destroy, nullptr);
    ASSERT_NE(district_built, nullptr);
    for (const auto* mapping : {quirk_lock, quirk_remove, trinket_destroy, district_built}) {
        EXPECT_TRUE(mapping->semantically_writable);
        EXPECT_TRUE(mapping->game_mutation_verified);
        EXPECT_EQ(mapping->evidence_level, "VERIFIED_GAME");
    }
    const auto weapon_rank = ddse::application::find_campaign_mapping("Hero.WeaponRank");
    ASSERT_NE(weapon_rank, nullptr);
    EXPECT_TRUE(weapon_rank->game_mutation_verified);
    EXPECT_FALSE(weapon_rank->semantically_writable);
    EXPECT_EQ(weapon_rank->capability(), ddse::application::CampaignMappingCapability::SessionOnly);
}

TEST(Stage9CampaignEditSession, StagesARealProfileHeroEditWithoutChangingRawSaveBytes) {
    auto profile = load_profile();
    FixtureContentEnvironment content;
    auto model = ddse::application::CampaignModelBuilder{}.build(profile, content);
    const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [](const auto& item) {
        return item.name.value && item.name.raw && item.state != ddse::domain::EntityState::Invalid;
    });
    ASSERT_NE(hero, model.heroes.end());
    const auto hero_id = hero->persistent_id;
    const auto original_name = *hero->name.value;
    const auto locator = *hero->name.raw;
    const auto original_document_bytes = profile.documents.at(locator.document_id).bytes;

    ddse::application::CampaignEditSession session{std::move(model)};
    const ddse::application::CampaignOperation operation = ddse::application::SetCampaignValueOperation{
        {"Hero.Name", hero_id, std::nullopt}, original_name + "_stage9"};
    const auto applied = session.apply(operation, 0);

    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied.value().changes.changes.size(), 1U);
    EXPECT_EQ(applied.value().changes.changes.front().raw.display_path, locator.display_path);
    EXPECT_EQ(applied.value().changes.affected_documents,
              (std::vector<std::string>{"persist.roster.json"}));
    const auto changed = std::find_if(session.model().heroes.begin(), session.model().heroes.end(), [&](const auto& item) {
        return item.persistent_id == hero_id;
    });
    ASSERT_NE(changed, session.model().heroes.end());
    EXPECT_EQ(changed->name.value, original_name + "_stage9");
    EXPECT_EQ(profile.documents.at(locator.document_id).bytes, original_document_bytes);
}
