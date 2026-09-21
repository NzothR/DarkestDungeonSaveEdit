#include "ddse/application/campaign_mappings.hpp"
#include "ddse/core/dson/dson_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

ddse::core::Result<ddse::core::dson::DsonDocument, ddse::core::Error>
parse_estate(const std::filesystem::path& path, const std::string& bytes) {
    ddse::core::dson::DsonReader reader;
    const auto* begin = reinterpret_cast<const std::byte*>(bytes.data());
    return reader.parse(std::span<const std::byte>{begin, bytes.size()}, path.string());
}
} // namespace

TEST(Stage7ResourceMapping, ReadsAllWalletEntriesFromRealProfileAndMatchesBackup) {
    const auto current_path = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR} / "persist.estate.json";
    const auto backup_path = std::filesystem::path{DDSE_TEST_SAVE_BACKUP_DIR} / "persist.estate.json";
    const auto current_bytes = read_bytes(current_path);
    const auto backup_bytes = read_bytes(backup_path);
    ASSERT_FALSE(current_bytes.empty());
    ASSERT_EQ(current_bytes, backup_bytes);

    auto parsed = parse_estate(current_path, current_bytes);
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto resources = ddse::application::read_campaign_resources(parsed.value());
    ASSERT_TRUE(resources) << resources.error().message;
    ASSERT_EQ(resources.value().size(), 8U);

    std::set<std::string> ids;
    for (const auto& resource : resources.value()) {
        EXPECT_GE(resource.amount, 0);
        EXPECT_EQ(resource.raw_object_path,
                  "base_root/wallet/" + std::to_string(resource.wallet_index));
        EXPECT_TRUE(ids.insert(resource.id).second);
    }
    EXPECT_EQ(ids, (std::set<std::string>{"gold", "bust", "portrait", "deed", "crest", "shard", "memory", "blueprint"}));

    auto backup = parse_estate(backup_path, backup_bytes);
    ASSERT_TRUE(backup) << backup.error().message;
    auto backup_resources = ddse::application::read_campaign_resources(backup.value());
    ASSERT_TRUE(backup_resources) << backup_resources.error().message;
    EXPECT_EQ(backup_resources.value().size(), resources.value().size());
    for (std::size_t i = 0; i < resources.value().size(); ++i) {
        EXPECT_EQ(backup_resources.value()[i].id, resources.value()[i].id);
        EXPECT_EQ(backup_resources.value()[i].amount, resources.value()[i].amount);
    }
}

TEST(Stage7ResourceMapping, PreservesUnknownResourceIdsInsteadOfUsingAFixedEnum) {
    const auto path = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR} / "persist.estate.json";
    auto parsed = parse_estate(path, read_bytes(path));
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto changed_copy = parsed.value();
    const auto field = std::find_if(changed_copy.fields.begin(), changed_copy.fields.end(), [](const auto& item) {
        return item.path == "base_root/wallet/0/type";
    });
    ASSERT_NE(field, changed_copy.fields.end());
    field->replace_value(std::string{"modded_currency_id"});

    auto resources = ddse::application::read_campaign_resources(changed_copy);
    ASSERT_TRUE(resources) << resources.error().message;
    EXPECT_EQ(resources.value().front().id, "modded_currency_id");
}

TEST(Stage7ResourceMapping, RegistryRecordsGameVerifiedResourceMutation) {
    const auto& mappings = ddse::application::stage7_resource_mappings();
    ASSERT_EQ(mappings.size(), 2U);
    EXPECT_EQ(mappings[0].semantic_property, "Estate.Resource.Amount");
    EXPECT_EQ(mappings[0].expected_type, ddse::core::dson::ValueKind::Integer);
    EXPECT_EQ(mappings[0].evidence_level, "VERIFIED_GAME");
    EXPECT_TRUE(mappings[0].semantically_writable);
    EXPECT_TRUE(mappings[0].game_mutation_verified);
    EXPECT_FALSE(mappings[1].semantically_writable);
}

TEST(Stage7ResourceMapping, MappingSeparatesCandidateSupportFromGameEvidence) {
    const auto resource = std::find_if(ddse::application::stage7_resource_mappings().begin(),
        ddse::application::stage7_resource_mappings().end(), [](const auto& item) {
            return item.semantic_property == "Estate.Resource.Amount";
        });
    ASSERT_NE(resource, ddse::application::stage7_resource_mappings().end());
    EXPECT_TRUE(resource->editable_in_session);
    EXPECT_TRUE(resource->semantically_writable);
    EXPECT_TRUE(resource->game_mutation_verified);

    const auto hero = std::find_if(ddse::application::stage8_campaign_mappings().begin(),
        ddse::application::stage8_campaign_mappings().end(), [](const auto& item) {
            return item.semantic_property == "Hero.Name";
        });
    ASSERT_NE(hero, ddse::application::stage8_campaign_mappings().end());
    EXPECT_TRUE(hero->editable_in_session);
    EXPECT_TRUE(hero->semantically_writable);
    EXPECT_FALSE(hero->game_mutation_verified);

    const auto* stress = ddse::application::find_campaign_mapping("Hero.Stress");
    ASSERT_NE(stress, nullptr);
    EXPECT_TRUE(stress->semantically_writable);
    EXPECT_TRUE(stress->editable_in_session);
    EXPECT_EQ(stress->capability(), ddse::application::CampaignMappingCapability::CandidateWritable);

    for (const auto property : {"Hero.AfflictionId", "Hero.AfflictionSeverity", "Hero.VirtueId"}) {
        const auto* mapping = ddse::application::find_campaign_mapping(property);
        ASSERT_NE(mapping, nullptr) << property;
        EXPECT_TRUE(mapping->semantically_writable) << property;
        EXPECT_FALSE(mapping->game_mutation_verified) << property;
        EXPECT_FALSE(mapping->editable_in_session) << property;
        EXPECT_EQ(mapping->capability(), ddse::application::CampaignMappingCapability::CandidateWritable) << property;
    }
    const auto* current_hp = ddse::application::find_campaign_mapping("Hero.CurrentHp");
    ASSERT_NE(current_hp, nullptr);
    EXPECT_FALSE(current_hp->semantically_writable);
    EXPECT_EQ(current_hp->capability(), ddse::application::CampaignMappingCapability::ReadOnly);
}
