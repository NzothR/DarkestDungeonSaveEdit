#include "ddse/application/campaign_mappings.hpp"
#include "ddse/application/campaign_edit_session.hpp"
#include "ddse/application/hero_template.hpp"
#include "ddse/application/save_commit.hpp"
#include "ddse/application/save_profile.hpp"
#include "ddse/core/dson/dson_document_editor.hpp"
#include "ddse/core/dson/dson_reader.hpp"
#include "ddse/core/dson/dson_writer.hpp"
#include "ddse/infrastructure/native_file_system.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ddse;

struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("ddse-stage10-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

class FaultInjectingFileSystem final : public application::IFileSystem {
public:
    bool fail_backup_writes{};
    std::size_t fail_atomic_write_number{};
    bool corrupt_atomic_write{};
    std::size_t atomic_write_count{};

    core::Result<bool, core::Error> exists(const std::filesystem::path& path) const override {
        return native_.exists(path);
    }
    core::Result<std::string, core::Error> read_file(const std::filesystem::path& path) const override {
        return native_.read_file(path);
    }
    core::Result<void, core::Error> write_file(const std::filesystem::path& path,
                                               const std::string& bytes) override {
        if (fail_backup_writes)
            return core::Result<void, core::Error>::failure(
                {core::ErrorCode::IoError, "injected backup write failure", "FaultInjectingFileSystem",
                 {{"path", path.string()}}});
        return native_.write_file(path, bytes);
    }
    core::Result<void, core::Error> write_file_atomic(const std::filesystem::path& path,
                                                      const std::string& bytes) override {
        ++atomic_write_count;
        if (fail_atomic_write_number == atomic_write_count)
            return core::Result<void, core::Error>::failure(
                {core::ErrorCode::IoError, "injected atomic replacement failure", "FaultInjectingFileSystem",
                 {{"path", path.string()}}});
        return native_.write_file_atomic(path, corrupt_atomic_write ? bytes + "corrupt" : bytes);
    }
    core::Result<void, core::Error> create_directories(const std::filesystem::path& path) override {
        return native_.create_directories(path);
    }
    core::Result<std::vector<std::filesystem::path>, core::Error>
    list_files(const std::filesystem::path& path) const override {
        return native_.list_files(path);
    }
    core::Result<std::vector<std::filesystem::path>, core::Error>
    list_directories(const std::filesystem::path& path) const override {
        return native_.list_directories(path);
    }
    core::Result<std::optional<std::filesystem::file_time_type>, core::Error>
    last_modified(const std::filesystem::path& path) const override {
        return native_.last_modified(path);
    }
    core::Result<std::uint64_t, core::Error> file_size(const std::filesystem::path& path) const override {
        return native_.file_size(path);
    }

private:
    infrastructure::NativeFileSystem native_;
};

void copy_profile(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::copy(source, destination, std::filesystem::copy_options::recursive);
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("failed to write Stage 10 test fixture");
}

std::vector<domain::RawLocatorStep> steps_to_field(const core::dson::DsonDocument& document,
                                                    std::size_t field_index) {
    std::vector<std::size_t> indices;
    auto index = field_index;
    while (index != core::dson::DsonField::no_index) {
        if (index >= document.fields.size()) return {};
        indices.push_back(index);
        index = document.fields[index].parent_index;
    }
    std::reverse(indices.begin(), indices.end());
    std::vector<domain::RawLocatorStep> result;
    for (const auto field : indices) {
        const auto& item = document.fields[field];
        result.push_back({field, item.name, item.kind == core::dson::ValueKind::EmbeddedDson});
    }
    return result;
}

application::ChangeSet change_set(std::vector<application::CampaignFieldChange> changes) {
    application::ChangeSet result;
    result.changes = std::move(changes);
    for (const auto& change : result.changes) {
        if (std::find(result.affected_documents.begin(), result.affected_documents.end(),
                      change.raw.document_id) == result.affected_documents.end())
            result.affected_documents.push_back(change.raw.document_id);
    }
    std::sort(result.affected_documents.begin(), result.affected_documents.end());
    return result;
}

std::optional<application::CampaignFieldChange> resource_amount_change(
    const application::RawSaveProfile& profile, std::int32_t increment = 1) {
    const auto estate = profile.documents.find("persist.estate.json");
    if (estate == profile.documents.end() || !estate->second.decoded) return std::nullopt;
    const auto resources = application::read_campaign_resources(*estate->second.decoded);
    if (!resources || resources.value().empty()) return std::nullopt;
    const auto& resource = resources.value().front();
    const auto path = resource.raw_object_path + "/amount";
    const auto& document = *estate->second.decoded;
    const auto field = std::find_if(document.fields.begin(), document.fields.end(), [&](const auto& item) {
        return item.path == path && item.kind == core::dson::ValueKind::Integer;
    });
    if (field == document.fields.end()) return std::nullopt;
    const auto before = std::get_if<std::int32_t>(&field->value);
    if (!before || *before > INT32_MAX - increment) return std::nullopt;
    const auto index = static_cast<std::size_t>(std::distance(document.fields.begin(), field));
    domain::RawLocator raw{estate->second.id, steps_to_field(document, index), field->path};
    return application::CampaignFieldChange{
        {"Estate.Resource.Amount", {}, resource.wallet_index}, std::move(raw), *before, *before + increment};
}

std::optional<application::CampaignFieldChange> hero_name_change(
    const application::RawSaveProfile& profile, std::string suffix = "_stage10") {
    const auto roster = profile.documents.find("persist.roster.json");
    if (roster == profile.documents.end() || !roster->second.decoded) return std::nullopt;
    const auto& outer = *roster->second.decoded;
    for (std::size_t outer_index = 0; outer_index < outer.fields.size(); ++outer_index) {
        const auto& embedded = outer.fields[outer_index];
        if (embedded.name != "raw_data" || embedded.kind != core::dson::ValueKind::EmbeddedDson ||
            !embedded.embedded_document) continue;
        constexpr std::string_view hero_prefix{"base_root/heroes/"};
        if (!embedded.path.starts_with(hero_prefix)) continue;
        const auto id_end = embedded.path.find('/', hero_prefix.size());
        if (id_end == std::string::npos) continue;
        const auto hero_id = embedded.path.substr(hero_prefix.size(), id_end - hero_prefix.size());
        const auto& inner = *embedded.embedded_document;
        const auto name = std::find_if(inner.fields.begin(), inner.fields.end(), [](const auto& field) {
            return field.path == "base_root/actor/name" && field.kind == core::dson::ValueKind::String;
        });
        if (name == inner.fields.end()) continue;
        const auto before = std::get_if<std::string>(&name->value);
        if (!before) continue;

        auto steps = steps_to_field(outer, outer_index);
        const auto inner_index = static_cast<std::size_t>(std::distance(inner.fields.begin(), name));
        auto inner_steps = steps_to_field(inner, inner_index);
        steps.insert(steps.end(), inner_steps.begin(), inner_steps.end());
        domain::RawLocator raw{roster->second.id, std::move(steps), embedded.path + " => " + name->path};
        return application::CampaignFieldChange{
            {"Hero.Name", hero_id, std::nullopt}, std::move(raw), *before, *before + suffix};
    }
    return std::nullopt;
}

std::optional<application::CampaignFieldChange> hero_resolve_xp_change(
    const application::RawSaveProfile& profile, std::int32_t increment = 1) {
    const auto roster = profile.documents.find("persist.roster.json");
    if (roster == profile.documents.end() || !roster->second.decoded) return std::nullopt;
    const auto& outer = *roster->second.decoded;
    for (std::size_t outer_index = 0; outer_index < outer.fields.size(); ++outer_index) {
        const auto& embedded = outer.fields[outer_index];
        if (embedded.name != "raw_data" || embedded.kind != core::dson::ValueKind::EmbeddedDson ||
            !embedded.embedded_document) continue;
        constexpr std::string_view hero_prefix{"base_root/heroes/"};
        if (!embedded.path.starts_with(hero_prefix)) continue;
        const auto id_end = embedded.path.find('/', hero_prefix.size());
        if (id_end == std::string::npos) continue;
        const auto hero_id = embedded.path.substr(hero_prefix.size(), id_end - hero_prefix.size());
        const auto& inner = *embedded.embedded_document;
        const auto xp = std::find_if(inner.fields.begin(), inner.fields.end(), [](const auto& field) {
            return field.path == "base_root/resolveXp" && field.kind == core::dson::ValueKind::Integer;
        });
        if (xp == inner.fields.end()) continue;
        const auto before = std::get_if<std::int32_t>(&xp->value);
        if (!before || *before > INT32_MAX - increment) continue;
        auto steps = steps_to_field(outer, outer_index);
        const auto inner_index = static_cast<std::size_t>(std::distance(inner.fields.begin(), xp));
        auto inner_steps = steps_to_field(inner, inner_index);
        steps.insert(steps.end(), inner_steps.begin(), inner_steps.end());
        domain::RawLocator raw{roster->second.id, std::move(steps), embedded.path + " => " + xp->path};
        return application::CampaignFieldChange{
            {"Hero.ResolveXp", hero_id}, std::move(raw), *before, *before + increment};
    }
    return std::nullopt;
}

std::optional<application::CampaignStructuralChange> first_hero_quirk_erase(
    const application::RawSaveProfile& profile) {
    const auto roster = profile.documents.find("persist.roster.json");
    if (roster == profile.documents.end() || !roster->second.decoded) return std::nullopt;
    const auto& outer = *roster->second.decoded;
    for (std::size_t outer_index = 0; outer_index < outer.fields.size(); ++outer_index) {
        const auto& embedded = outer.fields[outer_index];
        if (embedded.name != "raw_data" || embedded.kind != core::dson::ValueKind::EmbeddedDson ||
            !embedded.embedded_document) continue;
        constexpr std::string_view hero_prefix{"base_root/heroes/"};
        if (!embedded.path.starts_with(hero_prefix)) continue;
        const auto id_end = embedded.path.find('/', hero_prefix.size());
        if (id_end == std::string::npos) continue;
        const auto hero_id = embedded.path.substr(hero_prefix.size(), id_end - hero_prefix.size());
        const auto& inner = *embedded.embedded_document;
        const auto quirk = std::find_if(inner.fields.begin(), inner.fields.end(), [](const auto& field) {
            return field.path.starts_with("base_root/quirks/") &&
                   field.path.find('/', std::string_view{"base_root/quirks/"}.size()) == std::string::npos &&
                   field.kind == core::dson::ValueKind::Object;
        });
        if (quirk == inner.fields.end()) continue;
        auto steps = steps_to_field(outer, outer_index);
        const auto inner_index = static_cast<std::size_t>(std::distance(inner.fields.begin(), quirk));
        auto inner_steps = steps_to_field(inner, inner_index);
        steps.insert(steps.end(), inner_steps.begin(), inner_steps.end());
        domain::RawLocator raw{roster->second.id, std::move(steps), embedded.path + " => " + quirk->path};
        const auto key = quirk->name;
        domain::HeroQuirk removed;
        removed.id = key;
        removed.raw = raw;
        return application::CampaignStructuralChange{
            {"Hero.Quirk.Entry", hero_id, std::nullopt, key}, std::move(raw),
            core::dson::ValueKind::Object, 0U, std::move(removed)};
    }
    return std::nullopt;
}

application::RawSaveProfile load_profile(application::IFileSystem& file_system,
                                         const std::filesystem::path& root) {
    application::SaveProfileDiscovery discovery{file_system};
    auto result = discovery.load(root);
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result.value());
}

} // namespace

TEST(SaveAdapter, BuildsCloneCandidateAndRestrictsSemanticDiffToChangeSet) {
    TempDirectory temp;
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    infrastructure::NativeFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto change = resource_amount_change(profile);
    ASSERT_TRUE(change);
    const auto original_bytes = profile.documents.at("persist.estate.json").bytes;
    const auto original_amount = std::get<std::int32_t>(change->before);

    const auto candidate = application::SaveAdapter{}.build_candidate(profile, change_set({*change}));
    ASSERT_TRUE(candidate) << candidate.error().message;
    ASSERT_EQ(candidate.value().documents.size(), 1U);
    EXPECT_EQ(candidate.value().documents.front().id, "persist.estate.json");
    EXPECT_FALSE(candidate.value().documents.front().binary_diff.binary_identical);
    EXPECT_EQ(candidate.value().documents.front().expected_field_paths,
              (std::vector<std::string>{change->raw.display_path}));
    EXPECT_EQ(profile.documents.at("persist.estate.json").bytes, original_bytes);
    EXPECT_EQ(std::get<std::int32_t>(change->before), original_amount);
}

TEST(SaveAdapter, DeepClonesEmbeddedHeroDocumentBeforePatchingIt) {
    TempDirectory temp;
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    infrastructure::NativeFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto change = hero_name_change(profile);
    ASSERT_TRUE(change);
    const auto& source_roster = *profile.documents.at("persist.roster.json").decoded;
    const auto source_field = std::find_if(source_roster.fields.begin(), source_roster.fields.end(), [&](const auto& item) {
        return item.path == "base_root/heroes/" + change->target.entity_id + "/hero_file_data/raw_data";
    });
    ASSERT_NE(source_field, source_roster.fields.end());
    ASSERT_TRUE(source_field->embedded_document);
    const auto original_name = std::get<std::string>(change->before);

    const auto candidate = application::SaveAdapter{}.build_candidate(profile, change_set({*change}));
    ASSERT_TRUE(candidate) << candidate.error().message;
    EXPECT_EQ(candidate.value().documents.front().id, "persist.roster.json");
    const auto source_name = std::find_if(source_field->embedded_document->fields.begin(),
                                          source_field->embedded_document->fields.end(), [](const auto& item) {
        return item.path == "base_root/actor/name";
    });
    ASSERT_NE(source_name, source_field->embedded_document->fields.end());
    EXPECT_EQ(std::get<std::string>(source_name->value), original_name);
    EXPECT_FALSE(source_field->dirty);
}

TEST(SaveAdapter, ErasesOnlyTheMappedQuirkSubtreeAndPreservesTheLoadedSource) {
    TempDirectory temp;
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    infrastructure::NativeFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto erase = first_hero_quirk_erase(profile);
    ASSERT_TRUE(erase);
    const auto original_bytes = profile.documents.at("persist.roster.json").bytes;
    application::ChangeSet changes;
    changes.structural_changes.push_back(*erase);
    changes.affected_documents = {"persist.roster.json"};

    const auto candidate = application::SaveAdapter{}.build_candidate(profile, changes);
    ASSERT_TRUE(candidate) << candidate.error().message;
    ASSERT_EQ(candidate.value().documents.size(), 1U);
    EXPECT_EQ(candidate.value().documents.front().expected_field_paths,
              (std::vector<std::string>{erase->raw.display_path}));
    EXPECT_EQ(profile.documents.at("persist.roster.json").bytes, original_bytes);

    core::dson::DsonReader reader;
    const auto& bytes = candidate.value().documents.front().bytes;
    const auto* data = reinterpret_cast<const std::byte*>(bytes.data());
    auto decoded = reader.parse(std::span<const std::byte>{data, bytes.size()}, "persist.roster.json");
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto separator = erase->raw.display_path.find(" => ");
    ASSERT_NE(separator, std::string::npos);
    const auto inner_path = erase->raw.display_path.substr(separator + 4);
    const auto embedded = std::find_if(decoded.value().fields.begin(), decoded.value().fields.end(), [&](const auto& field) {
        return field.kind == core::dson::ValueKind::EmbeddedDson && field.embedded_document &&
               field.path.starts_with("base_root/heroes/" + erase->target.entity_id + "/");
    });
    ASSERT_NE(embedded, decoded.value().fields.end());
    const auto absent = std::find_if(embedded->embedded_document->fields.begin(), embedded->embedded_document->fields.end(),
                                     [&](const auto& field) { return field.path == inner_path; });
    EXPECT_EQ(absent, embedded->embedded_document->fields.end());
}

TEST(SaveAdapter, CreatesAQuirkInAnEmptySlotAndSkipsANetNoOpWrite) {
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    TempDirectory temp;
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    infrastructure::NativeFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto hero_name = hero_name_change(profile);
    ASSERT_TRUE(hero_name);
    const auto hero_id = hero_name->target.entity_id;
    const auto target = "base_root/heroes/" + hero_id +
        "/hero_file_data/raw_data => base_root/quirks/ddse_regression_quirk";
    const auto make_changes = [&](bool erase_after_create) {
        application::ChangeSet changes;
        application::CampaignDocumentMutationBatch batch;
        batch.operation_id = "campaign.hero.add_or_replace_quirk";
        batch.transaction_id = "test-quirk-create";
        batch.mutations.emplace_back(application::CampaignDocumentMutationKind::CreateObject,
            "Hero.Quirks", "persist.roster.json", target, std::string{},
            "ddse_regression_quirk", core::dson::ValueKind::Object);
        if (erase_after_create)
            batch.mutations.emplace_back(application::CampaignDocumentMutationKind::Erase,
                "Hero.Quirks", "persist.roster.json", target, std::string{},
                std::string{}, core::dson::ValueKind::Object);
        changes.document_mutation_batches.push_back(std::move(batch));
        changes.affected_documents = {"persist.roster.json"};
        return changes;
    };
    const auto created = application::SaveAdapter{}.build_candidate(profile, make_changes(false));
    ASSERT_TRUE(created) << created.error().message;
    ASSERT_EQ(created.value().documents.size(), 1U);
    const auto bytes = created.value().documents.front().bytes;
    core::dson::DsonReader reader;
    const auto* data = reinterpret_cast<const std::byte*>(bytes.data());
    const auto decoded = reader.parse(std::span<const std::byte>{data, bytes.size()}, "persist.roster.json");
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto hero_file = std::find_if(decoded.value().fields.begin(), decoded.value().fields.end(),
        [&](const auto& field) {
            return field.path == "base_root/heroes/" + hero_id + "/hero_file_data/raw_data" &&
                field.embedded_document != nullptr;
        });
    ASSERT_NE(hero_file, decoded.value().fields.end());
    const auto& inner = hero_file->embedded_document->fields;
    for (const auto& leaf : {"is_new", "is_locked", "trinketId", "mission_count",
                             "replaces_quirk", "replaces_quirk_viewed", "evolution_duration_remaining"})
        EXPECT_NE(std::find_if(inner.begin(), inner.end(), [&](const auto& field) {
            return field.path == "base_root/quirks/ddse_regression_quirk/" + std::string{leaf};
        }), inner.end());

    const auto unchanged = application::SaveAdapter{}.build_candidate(profile, make_changes(true));
    ASSERT_TRUE(unchanged) << unchanged.error().message;
    EXPECT_TRUE(unchanged.value().documents.empty());
    const auto target_root = temp.path / "output" / "profile_0";
    copy_profile(source_root, target_root);
    const auto backup_root = temp.path / "backups" / "net-no-op";
    const auto committed = application::SafeSaveCommitter{fs}.commit(
        profile, make_changes(true), target_root, backup_root);
    ASSERT_TRUE(committed) << committed.error().message;
    EXPECT_TRUE(committed.value().committed_documents.empty());
    EXPECT_FALSE(std::filesystem::exists(backup_root));
    EXPECT_EQ(read_bytes(target_root / "persist.roster.json"),
              profile.documents.at("persist.roster.json").bytes);
}

TEST(SaveAdapter, RejectsAnUnmappedOrUnverifiedStructuralTarget) {
    application::ChangeSet changes;
    domain::HeroQuirk removed;
    removed.id = "unmapped";
    changes.structural_changes.push_back({{"Hero.PersistentId", "hero-1", std::nullopt, "unmapped"},
        {"persist.roster.json", {{0, "x", false}}, "base_root/heroes/hero-1/unmapped"},
        core::dson::ValueKind::Object, 0U, removed});
    changes.affected_documents = {"persist.roster.json"};
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    TempDirectory temp;
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    infrastructure::NativeFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto candidate = application::SaveAdapter{}.build_candidate(profile, changes);
    ASSERT_FALSE(candidate);
    EXPECT_EQ(candidate.error().code, core::ErrorCode::MappingNotWritable);
}

TEST(SafeSaveCommitter, BacksUpCompleteProfileAndWritesOnlyTheExplicitCopy) {
    TempDirectory temp;
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    write_bytes(source_root / "extensions" / "opaque.bin", "preserve nested data");
    std::filesystem::create_directories(source_root / "empty-extension-directory");
    const auto target_root = temp.path / "output" / "profile_0";
    copy_profile(source_root, target_root);

    infrastructure::NativeFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto change = resource_amount_change(profile);
    ASSERT_TRUE(change);
    const auto original_estate = profile.documents.at("persist.estate.json").bytes;
    const auto original_roster = profile.documents.at("persist.roster.json").bytes;
    const auto backup_path = temp.path / "backups" / "commit-1";

    const auto result = application::SafeSaveCommitter{fs}.commit(
        profile, change_set({*change}), target_root, backup_path);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().committed_documents, (std::vector<std::string>{"persist.estate.json"}));
    EXPECT_EQ(result.value().backup_directory, backup_path);
    EXPECT_EQ(read_bytes(source_root / "persist.estate.json"), original_estate);
    EXPECT_NE(read_bytes(target_root / "persist.estate.json"), original_estate);
    EXPECT_EQ(read_bytes(target_root / "persist.roster.json"), original_roster);
    EXPECT_EQ(read_bytes(backup_path / "persist.estate.json"), original_estate);
    EXPECT_EQ(read_bytes(backup_path / "persist.roster.json"), original_roster);
    EXPECT_EQ(read_bytes(backup_path / "extensions" / "opaque.bin"), "preserve nested data");
    EXPECT_TRUE(std::filesystem::is_directory(backup_path / "empty-extension-directory"));

    const auto backup_fingerprint = application::SaveProfileDiscovery::fingerprint_profile(fs, backup_path);
    ASSERT_TRUE(backup_fingerprint) << backup_fingerprint.error().message;
    EXPECT_EQ(backup_fingerprint.value(), profile.baseline_fingerprint);

    core::dson::DsonReader reader;
    const auto saved_bytes = read_bytes(target_root / "persist.estate.json");
    const auto* data = reinterpret_cast<const std::byte*>(saved_bytes.data());
    const auto decoded = reader.parse(std::span<const std::byte>{data, saved_bytes.size()}, "persist.estate.json");
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto found = std::find_if(decoded.value().fields.begin(), decoded.value().fields.end(), [&](const auto& field) {
        return field.path == change->raw.display_path;
    });
    ASSERT_NE(found, decoded.value().fields.end());
    EXPECT_EQ(std::get<std::int32_t>(found->value), std::get<std::int32_t>(change->after));
}

TEST(SaveAdapter, BuildsANameChangeCandidateWithoutChangingOtherEmbeddedHeroFields) {
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    infrastructure::NativeFileSystem fs;
    const auto profile = load_profile(fs, fixture);
    const auto name = hero_name_change(profile, "英雄名称测试");
    ASSERT_TRUE(name);

    const auto candidate = application::SaveAdapter{}.build_candidate(profile, change_set({*name}));
    ASSERT_TRUE(candidate) << candidate.error().message;
    ASSERT_EQ(candidate.value().documents.size(), 1U);
    EXPECT_EQ(candidate.value().documents.front().id, "persist.roster.json");
}

TEST(SafeSaveCommitter, CommitsMappedStructuralEraseToTheCopyAndKeepsBackupAndSource) {
    TempDirectory temp;
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    const auto target_root = temp.path / "output" / "profile_0";
    copy_profile(source_root, target_root);
    const auto backup_path = temp.path / "backups" / "quirk-erase";
    infrastructure::NativeFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto erase = first_hero_quirk_erase(profile);
    ASSERT_TRUE(erase);
    const auto original_source = profile.documents.at("persist.roster.json").bytes;
    application::ChangeSet changes;
    changes.structural_changes.push_back(*erase);
    changes.affected_documents = {"persist.roster.json"};

    const auto result = application::SafeSaveCommitter{fs}.commit(profile, changes, target_root, backup_path);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(read_bytes(source_root / "persist.roster.json"), original_source);
    EXPECT_EQ(read_bytes(backup_path / "persist.roster.json"), original_source);
    EXPECT_NE(read_bytes(target_root / "persist.roster.json"), original_source);
    EXPECT_EQ(result.value().committed_documents, (std::vector<std::string>{"persist.roster.json"}));
}

TEST(SafeSaveCommitter, CommitsVerifiedHeroRenameWithBackup) {
    TempDirectory temp;
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    const auto target_root = temp.path / "output" / "profile_0";
    copy_profile(source_root, target_root);
    FaultInjectingFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto name = hero_name_change(profile);
    ASSERT_TRUE(name);
    const auto backup_path = temp.path / "backups" / "hero-rename";

    const auto result = application::SafeSaveCommitter{fs}.commit(profile, change_set({*name}), target_root, backup_path);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(fs.atomic_write_count, 1U);
    EXPECT_TRUE(std::filesystem::exists(backup_path / "persist.roster.json"));
    EXPECT_NE(read_bytes(target_root / "persist.roster.json"), profile.documents.at("persist.roster.json").bytes);
}

TEST(SaveAdapter, AddsAndRenamesTemplateHeroInAnEmptyRoster) {
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    TempDirectory temp;
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    infrastructure::NativeFileSystem fs;
    auto profile = load_profile(fs, source_root);
    const auto template_result = application::build_blank_level_zero_hero_template(
        "crusader", {"smite", "stunning_blow"}, {}, 33.0F);
    ASSERT_TRUE(template_result) << template_result.error().message;
    auto empty_roster = *profile.documents.at("persist.roster.json").decoded;
    constexpr std::string_view hero_prefix{"base_root/heroes/"};
    while (true) {
        const auto hero = std::find_if(empty_roster.fields.begin(), empty_roster.fields.end(), [hero_prefix](const auto& field) {
            return field.kind == core::dson::ValueKind::Object && field.path.starts_with(hero_prefix) &&
                field.path.find('/', hero_prefix.size()) == std::string::npos;
        });
        if (hero == empty_roster.fields.end()) break;
        const auto erased = core::dson::DsonDocumentEditor::erase(empty_roster, hero->path);
        ASSERT_TRUE(erased) << erased.error().message;
    }
    const auto encoded = core::dson::DsonWriter{}.encode(empty_roster);
    ASSERT_TRUE(encoded) << encoded.error().message;
    const std::string bytes{reinterpret_cast<const char*>(encoded.value().data()), encoded.value().size()};
    write_bytes(source_root / "persist.roster.json", bytes);
    profile = load_profile(fs, source_root);
    const auto parsed = core::dson::DsonReader{}.parse(
        std::span<const std::byte>{encoded.value().data(), encoded.value().size()}, "persist.roster.json");
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto next_guid_field = std::find_if(parsed.value().fields.begin(), parsed.value().fields.end(),
        [](const auto& field) { return field.path == "base_root/nextGuid"; });
    ASSERT_NE(next_guid_field, parsed.value().fields.end());
    const auto starting_guid = std::get<std::int32_t>(next_guid_field->value);
    const auto hero_id = std::to_string(std::max(starting_guid, 2));

    application::CampaignDocumentMutation append{
        application::CampaignDocumentMutationKind::AppendTemplate, "Hero.PersistentId",
        "persist.roster.json", "base_root/heroes/" + hero_id, "base_root/heroes/1", hero_id,
        core::dson::ValueKind::Object};
    append.template_document = template_result.value();
    append.template_hero_class = "crusader";
    append.template_class_name = "Crusader";
    append.template_source_id = "vanilla";
    append.template_base_hit_points = 33.0F;
    append.template_combat_skills = {"smite", "stunning_blow"};
    application::CampaignEditSession session{domain::CampaignModel{}};
    const auto added = session.apply(application::CampaignOperation{
        application::ApplyCampaignDocumentMutationsOperation{"campaign.hero.add", {append}}}, 0);
    ASSERT_TRUE(added) << added.error().message;
    ASSERT_EQ(session.model().heroes.size(), 1U);
    const auto renamed = session.apply(application::CampaignOperation{
        application::SetCampaignValueOperation{{"Hero.Name", hero_id}, std::string{"New Crusader"}}},
        session.revision());
    ASSERT_TRUE(renamed) << renamed.error().message;
    const auto leveled = session.apply(application::CampaignOperation{
        application::SetCampaignValueOperation{{"Hero.ResolveXp", hero_id}, std::int32_t{2}}},
        session.revision());
    ASSERT_TRUE(leveled) << leveled.error().message;

    const auto candidate = application::SaveAdapter{}.build_candidate(profile, session.pending_changes());
    ASSERT_TRUE(candidate) << candidate.error().message;
    ASSERT_EQ(candidate.value().documents.size(), 1U);
    const auto& candidate_bytes = candidate.value().documents.front().bytes;
    const auto decoded = core::dson::DsonReader{}.parse(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(candidate_bytes.data()), candidate_bytes.size()},
        "persist.roster.json");
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto advanced = std::find_if(decoded.value().fields.begin(), decoded.value().fields.end(),
        [](const auto& field) { return field.path == "base_root/nextGuid"; });
    ASSERT_NE(advanced, decoded.value().fields.end());
    EXPECT_EQ(std::get<std::int32_t>(advanced->value), std::max(starting_guid, std::stoi(hero_id) + 1));
    const auto embedded = std::find_if(decoded.value().fields.begin(), decoded.value().fields.end(), [&](const auto& field) {
        return field.path == "base_root/heroes/" + hero_id + "/hero_file_data/raw_data";
    });
    ASSERT_NE(embedded, decoded.value().fields.end());
    ASSERT_TRUE(embedded->embedded_document);
    const auto embedded_value = [&](std::string_view path) -> const core::dson::DsonField* {
        const auto found = std::find_if(embedded->embedded_document->fields.begin(),
            embedded->embedded_document->fields.end(), [path](const auto& field) { return field.path == path; });
        return found == embedded->embedded_document->fields.end() ? nullptr : &*found;
    };
    const auto* roster_status = embedded_value("base_root/roster.status");
    const auto* prior_status = embedded_value("base_root/roster.before_on_start_town_visit_status");
    const auto* current_hp = embedded_value("base_root/actor/current_hp");
    const auto* resolve_xp = embedded_value("base_root/resolveXp");
    const auto* weapon_rank = embedded_value("base_root/weapon_rank");
    const auto* armour_rank = embedded_value("base_root/armour_rank");
    ASSERT_NE(roster_status, nullptr);
    ASSERT_NE(prior_status, nullptr);
    ASSERT_NE(current_hp, nullptr);
    ASSERT_NE(resolve_xp, nullptr);
    ASSERT_NE(weapon_rank, nullptr);
    ASSERT_NE(armour_rank, nullptr);
    EXPECT_EQ(std::get<std::int32_t>(roster_status->value), 0);
    EXPECT_EQ(std::get<std::int32_t>(prior_status->value), 0);
    EXPECT_FLOAT_EQ(std::get<float>(current_hp->value), 33.0F);
    EXPECT_EQ(std::get<std::int32_t>(resolve_xp->value), 2);
    EXPECT_EQ(std::get<std::int32_t>(weapon_rank->value), 0);
    EXPECT_EQ(std::get<std::int32_t>(armour_rank->value), 0);
    ASSERT_NE(embedded_value("base_root/skills/selected_combat_skills/smite"), nullptr);
    ASSERT_NE(embedded_value("base_root/skills/selected_combat_skills/stunning_blow"), nullptr);
    const auto name = std::find_if(embedded->embedded_document->fields.begin(), embedded->embedded_document->fields.end(),
        [](const auto& field) { return field.path == "base_root/actor/name"; });
    ASSERT_NE(name, embedded->embedded_document->fields.end());
    EXPECT_EQ(std::get<std::string>(name->value), "New Crusader");

    // New hero scalar edits must also survive a commit that touches the separate
    // purchase-history document in the same ChangeSet.
    const auto& upgrades = *profile.documents.at("persist.upgrades.json").decoded;
    const auto purchased = std::find_if(upgrades.fields.begin(), upgrades.fields.end(), [](const auto& field) {
        return field.path == "base_root/purchases/0/is_purchased" &&
            field.kind == core::dson::ValueKind::Boolean;
    });
    ASSERT_NE(purchased, upgrades.fields.end());
    const auto prior_purchase = std::get<bool>(purchased->value);
    auto multi_document_changes = session.pending_changes();
    multi_document_changes.document_mutation_batches.push_back({
        "campaign.hero.maximize_progression", "test-purchase-history",
        {{application::CampaignDocumentMutationKind::SetValue, "Upgrade.PurchaseNode.Entry",
          "persist.upgrades.json", purchased->path, {}, {}, core::dson::ValueKind::Boolean,
          application::CampaignValue{prior_purchase}, application::CampaignValue{!prior_purchase}}}});
    multi_document_changes.affected_documents.push_back("persist.upgrades.json");
    const auto multi_document_candidate = application::SaveAdapter{}.build_candidate(profile, multi_document_changes);
    ASSERT_TRUE(multi_document_candidate) << multi_document_candidate.error().message;
    EXPECT_EQ(multi_document_candidate.value().documents.size(), 2U);

    const auto target_root = temp.path / "output" / "profile_0";
    copy_profile(source_root, target_root);
    const auto backup_root = temp.path / "backups" / "hero-add-rename";
    const auto committed = application::SafeSaveCommitter{fs}.commit(
        profile, session.pending_changes(), target_root, backup_root);
    ASSERT_TRUE(committed) << committed.error().message;
    EXPECT_EQ(read_bytes(target_root / "persist.roster.json"), candidate_bytes);
    EXPECT_EQ(read_bytes(backup_root / "persist.roster.json"), bytes);
    EXPECT_EQ(read_bytes(source_root / "persist.roster.json"), bytes);
}

TEST(SaveAdapter, SavesCampingSkillReplacementInEquipmentOrder) {
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    infrastructure::NativeFileSystem fs;
    const auto profile = load_profile(fs, fixture);
    const auto& roster = *profile.documents.at("persist.roster.json").decoded;
    std::map<std::string, std::vector<const core::dson::DsonField*>, std::less<>> selected;
    constexpr std::string_view hero_prefix{"base_root/heroes/"};
    constexpr std::string_view camping_path{"base_root/skills/selected_camping_skills/"};
    for (const auto& field : roster.fields) {
        if (!field.embedded_document || !field.path.starts_with(hero_prefix) ||
            !field.path.ends_with("/hero_file_data/raw_data")) continue;
        const auto hero_id = field.path.substr(hero_prefix.size(),
            field.path.size() - hero_prefix.size() - std::string_view{"/hero_file_data/raw_data"}.size());
        for (const auto& skill : field.embedded_document->fields)
            if (skill.kind == core::dson::ValueKind::Integer && skill.path.starts_with(camping_path) &&
                skill.path.find('/', camping_path.size()) == std::string::npos)
                selected[hero_id].push_back(&skill);
    }
    const auto found = std::find_if(selected.begin(), selected.end(),
        [](const auto& entry) { return !entry.second.empty(); });
    if (found == selected.end()) GTEST_SKIP() << "No hero has an equipped camping skill";
    domain::CampaignModel model;
    domain::Hero hero;
    hero.persistent_id = found->first;
    hero.state = domain::EntityState::Resolved;
    hero.raw = {"persist.roster.json", {}, std::string{hero_prefix} + found->first};
    for (const auto* field : found->second) {
        domain::HeroSkillSelection skill;
        skill.id = field->name;
        skill.camping = true;
        skill.raw = {"persist.roster.json", {}, std::string{hero_prefix} + found->first +
            "/hero_file_data/raw_data => " + field->path};
        skill.raw_value.value = std::get<std::int32_t>(field->value);
        skill.raw_value.raw = skill.raw;
        hero.camping_skills.push_back(std::move(skill));
    }
    model.heroes.push_back(std::move(hero));
    application::CampaignEditSession session{std::move(model)};
    for (int index = static_cast<int>(found->second.size()); index < 4; ++index) {
        const auto id = "ddse_camp_test_" + std::to_string(index);
        auto planned = application::make_set_hero_camping_skill_equipped_operation(
            session.model(), found->first, id, true);
        ASSERT_TRUE(planned) << planned.error().message;
        auto applied = session.apply(planned.value(), session.revision());
        ASSERT_TRUE(applied) << applied.error().message;
    }
    std::vector<std::string> initial_skills;
    for (const auto& skill : session.model().heroes.front().camping_skills)
        initial_skills.push_back(skill.id);
    ASSERT_EQ(initial_skills.size(), 4U);
    for (const auto& id : {std::string{"ddse_camp_test_e"}, std::string{"ddse_camp_test_f"},
                           initial_skills[0], initial_skills[1], initial_skills[2]}) {
        auto planned = application::make_set_hero_camping_skill_equipped_operation(
            session.model(), found->first, id, true);
        ASSERT_TRUE(planned) << planned.error().message;
        auto applied = session.apply(planned.value(), session.revision());
        ASSERT_TRUE(applied) << applied.error().message;
    }
    const auto candidate = application::SaveAdapter{}.build_candidate(profile, session.pending_changes());
    ASSERT_TRUE(candidate) << candidate.error().message;
    const auto& bytes = candidate.value().documents.front().bytes;
    const auto decoded = core::dson::DsonReader{}.parse(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()},
        "persist.roster.json");
    ASSERT_TRUE(decoded) << decoded.error().message;
    std::vector<std::string> final_skills;
    const auto embedded = std::find_if(decoded.value().fields.begin(), decoded.value().fields.end(),
        [&](const auto& field) { return field.path == std::string{hero_prefix} + found->first +
            "/hero_file_data/raw_data"; });
    ASSERT_NE(embedded, decoded.value().fields.end());
    ASSERT_TRUE(embedded->embedded_document);
    for (const auto& field : embedded->embedded_document->fields)
        if (field.kind == core::dson::ValueKind::Integer && field.path.starts_with(camping_path) &&
            field.path.find('/', camping_path.size()) == std::string::npos)
            final_skills.push_back(field.name);
    EXPECT_EQ(final_skills, (std::vector<std::string>{
        "ddse_camp_test_f", initial_skills[0], initial_skills[1], initial_skills[2]}));
}

TEST(SaveAdapter, ReordersAndDeletesHeroesWithUndoAndCandidateReadback) {
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    infrastructure::NativeFileSystem fs;
    const auto profile = load_profile(fs, fixture);
    const auto& roster = *profile.documents.at("persist.roster.json").decoded;
    domain::CampaignModel model;
    std::vector<std::string> original_ids;
    constexpr std::string_view prefix{"base_root/heroes/"};
    for (const auto& field : roster.fields) {
        if (field.kind != core::dson::ValueKind::Object || !field.path.starts_with(prefix) ||
            field.path.find('/', prefix.size()) != std::string::npos) continue;
        const auto id = field.path.substr(prefix.size());
        domain::Hero hero;
        hero.persistent_id = id;
        hero.roster_position = model.heroes.size();
        hero.state = domain::EntityState::Resolved;
        hero.read_only = false;
        hero.raw = {"persist.roster.json", {}, field.path};
        model.heroes.push_back(std::move(hero));
        original_ids.push_back(id);
    }
    ASSERT_GE(original_ids.size(), 2U);

    using Kind = application::CampaignDocumentMutationKind;
    const auto path = [](std::string_view id) { return "base_root/heroes/" + std::string{id}; };
    auto reordered_ids = original_ids;
    std::swap(reordered_ids[0], reordered_ids[1]);
    std::vector<application::CampaignDocumentMutation> reorder;
    for (std::size_t index = 0; index < reordered_ids.size(); ++index) {
        const auto temporary = "ddse_test_order_" + std::to_string(index);
        reorder.emplace_back(Kind::AppendClone, "Hero.PersistentId", "persist.roster.json",
            path(temporary), path(reordered_ids[index]), temporary, core::dson::ValueKind::Object);
    }
    for (const auto& id : original_ids)
        reorder.emplace_back(Kind::Erase, "Hero.PersistentId", "persist.roster.json",
            path(id), "", "", core::dson::ValueKind::Object);
    for (std::size_t index = 0; index < reordered_ids.size(); ++index)
        reorder.emplace_back(Kind::Rename, "Hero.PersistentId", "persist.roster.json",
            path("ddse_test_order_" + std::to_string(index)), "", reordered_ids[index], core::dson::ValueKind::Object);

    application::CampaignEditSession session{model};
    const auto applied = session.apply(application::CampaignOperation{
        application::ApplyCampaignDocumentMutationsOperation{"campaign.hero.reorder", reorder}}, 0);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(session.model().heroes.front().persistent_id, reordered_ids.front());
    ASSERT_TRUE(session.undo(session.revision()));
    EXPECT_EQ(session.model().heroes.front().persistent_id, original_ids.front());
    ASSERT_TRUE(session.redo(session.revision()));
    const auto candidate = application::SaveAdapter{}.build_candidate(profile, session.pending_changes());
    ASSERT_TRUE(candidate) << candidate.error().message;
    const auto& bytes = candidate.value().documents.front().bytes;
    const auto decoded = core::dson::DsonReader{}.parse(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()}, "persist.roster.json");
    ASSERT_TRUE(decoded) << decoded.error().message;
    std::vector<std::string> actual_ids;
    for (const auto& field : decoded.value().fields)
        if (field.kind == core::dson::ValueKind::Object && field.path.starts_with(prefix) &&
            field.path.find('/', prefix.size()) == std::string::npos)
            actual_ids.push_back(field.path.substr(prefix.size()));
    EXPECT_EQ(actual_ids, reordered_ids);

    application::CampaignEditSession deletion{model};
    const auto deleted_id = original_ids.back();
    std::vector<application::CampaignDocumentMutation> erase;
    erase.emplace_back(Kind::Erase, "Hero.PersistentId", "persist.roster.json",
        path(deleted_id), "", "", core::dson::ValueKind::Object);
    const auto deleted = deletion.apply(application::CampaignOperation{
        application::ApplyCampaignDocumentMutationsOperation{"campaign.hero.delete", erase}}, 0);
    ASSERT_TRUE(deleted) << deleted.error().message;
    ASSERT_TRUE(deletion.undo(deletion.revision()));
    ASSERT_TRUE(deletion.redo(deletion.revision()));
    const auto deletion_candidate = application::SaveAdapter{}.build_candidate(profile, deletion.pending_changes());
    ASSERT_TRUE(deletion_candidate) << deletion_candidate.error().message;
    const auto& deleted_bytes = deletion_candidate.value().documents.front().bytes;
    const auto deleted_roster = core::dson::DsonReader{}.parse(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(deleted_bytes.data()), deleted_bytes.size()},
        "persist.roster.json");
    ASSERT_TRUE(deleted_roster) << deleted_roster.error().message;
    EXPECT_TRUE(std::none_of(deleted_roster.value().fields.begin(), deleted_roster.value().fields.end(),
        [&](const auto& field) { return field.path == path(deleted_id); }));
    EXPECT_EQ(profile.documents.at("persist.roster.json").bytes, read_bytes(fixture / "persist.roster.json"));

    const auto rename = hero_name_change(profile, "Delete After Rename");
    ASSERT_TRUE(rename);
    auto named_model = model;
    auto named_hero = std::find_if(named_model.heroes.begin(), named_model.heroes.end(), [&](const auto& hero) {
        return hero.persistent_id == rename->target.entity_id;
    });
    ASSERT_NE(named_hero, named_model.heroes.end());
    named_hero->name.value = std::get<std::string>(rename->before);
    named_hero->name.raw = rename->raw;
    application::CampaignEditSession renamed_then_reordered{named_model};
    const auto rename_before_reorder = renamed_then_reordered.apply(application::CampaignOperation{
        application::SetCampaignValueOperation{{"Hero.Name", named_hero->persistent_id},
            std::string{"Rename Before Reorder"}}}, 0);
    ASSERT_TRUE(rename_before_reorder) << rename_before_reorder.error().message;
    const auto reordered_after_rename = renamed_then_reordered.apply(application::CampaignOperation{
        application::ApplyCampaignDocumentMutationsOperation{"campaign.hero.reorder", reorder}},
        renamed_then_reordered.revision());
    ASSERT_TRUE(reordered_after_rename) << reordered_after_rename.error().message;
    const auto renamed_reordered_candidate = application::SaveAdapter{}.build_candidate(
        profile, renamed_then_reordered.pending_changes());
    ASSERT_TRUE(renamed_reordered_candidate) << renamed_reordered_candidate.error().message;
    application::CampaignEditSession renamed_then_deleted{named_model};
    const auto renamed = renamed_then_deleted.apply(application::CampaignOperation{
        application::SetCampaignValueOperation{{"Hero.Name", named_hero->persistent_id},
            std::string{"Delete After Rename"}}}, 0);
    ASSERT_TRUE(renamed) << renamed.error().message;
    std::vector<application::CampaignDocumentMutation> erase_named;
    erase_named.emplace_back(Kind::Erase, "Hero.PersistentId", "persist.roster.json",
        path(named_hero->persistent_id), "", "", core::dson::ValueKind::Object);
    const auto removed_named = renamed_then_deleted.apply(application::CampaignOperation{
        application::ApplyCampaignDocumentMutationsOperation{"campaign.hero.delete", erase_named}},
        renamed_then_deleted.revision());
    ASSERT_TRUE(removed_named) << removed_named.error().message;
    const auto renamed_deleted_candidate = application::SaveAdapter{}.build_candidate(
        profile, renamed_then_deleted.pending_changes());
    ASSERT_TRUE(renamed_deleted_candidate) << renamed_deleted_candidate.error().message;
    TempDirectory temp;
    const auto target_root = temp.path / "output" / "profile_0";
    copy_profile(fixture, target_root);
    const auto committed = application::SafeSaveCommitter{fs}.commit(profile,
        renamed_then_deleted.pending_changes(), target_root, temp.path / "backup");
    ASSERT_TRUE(committed) << committed.error().message;
    EXPECT_EQ(read_bytes(target_root / "persist.roster.json"),
        renamed_deleted_candidate.value().documents.front().bytes);
}

TEST(SaveAdapter, AddsTrinketFromBuiltInSchemaToEmptyInventory) {
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    infrastructure::NativeFileSystem fs;
    auto profile = load_profile(fs, fixture);
    auto& estate = profile.documents.at("persist.estate.json");
    auto empty_estate = *estate.decoded;
    constexpr std::string_view item_prefix{"base_root/trinkets/items/"};
    while (true) {
        const auto item = std::find_if(empty_estate.fields.begin(), empty_estate.fields.end(), [item_prefix](const auto& field) {
            return field.kind == core::dson::ValueKind::Object && field.path.starts_with(item_prefix) &&
                field.path.find('/', item_prefix.size()) == std::string::npos;
        });
        if (item == empty_estate.fields.end()) break;
        const auto erased = core::dson::DsonDocumentEditor::erase(empty_estate, item->path);
        ASSERT_TRUE(erased) << erased.error().message;
    }
    const auto encoded = core::dson::DsonWriter{}.encode(empty_estate);
    ASSERT_TRUE(encoded) << encoded.error().message;
    estate.bytes.assign(reinterpret_cast<const char*>(encoded.value().data()), encoded.value().size());
    const auto parsed = core::dson::DsonReader{}.parse(
        std::span<const std::byte>{encoded.value().data(), encoded.value().size()}, "persist.estate.json");
    ASSERT_TRUE(parsed) << parsed.error().message;
    estate.decoded = parsed.value();

    using Kind = application::CampaignDocumentMutationKind;
    std::vector<application::CampaignDocumentMutation> mutations;
    mutations.emplace_back(Kind::CreateObject, "TrinketInventory.Items", "persist.estate.json",
        "base_root/trinkets/items/0", "", "0", core::dson::ValueKind::Object);
    mutations.emplace_back(Kind::SetValue, "TrinketInventory.Items", "persist.estate.json",
        "base_root/trinkets/items/0/id", "", "", core::dson::ValueKind::String,
        application::CampaignValue{std::string{}}, application::CampaignValue{std::string{"test_trinket"}});
    application::CampaignEditSession session{domain::CampaignModel{}};
    const auto added = session.apply(application::CampaignOperation{
        application::ApplyCampaignDocumentMutationsOperation{"campaign.trinket.add_inventory", mutations}}, 0);
    ASSERT_TRUE(added) << added.error().message;
    const auto candidate = application::SaveAdapter{}.build_candidate(profile, session.pending_changes());
    ASSERT_TRUE(candidate) << candidate.error().message;
    ASSERT_EQ(candidate.value().documents.size(), 1U);
    const auto& output = candidate.value().documents.front().bytes;
    const auto decoded = core::dson::DsonReader{}.parse(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(output.data()), output.size()},
        "persist.estate.json");
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto trinket = std::find_if(decoded.value().fields.begin(), decoded.value().fields.end(), [](const auto& field) {
        return field.path == "base_root/trinkets/items/0/id";
    });
    ASSERT_NE(trinket, decoded.value().fields.end());
    EXPECT_EQ(std::get<std::string>(trinket->value), "test_trinket");
}

TEST(SafeSaveCommitter, AcceptanceTestModeWritesCandidateMappingOnlyToVerifiedCopy) {
    TempDirectory temp;
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    const auto target_root = temp.path / "output" / "profile_0";
    copy_profile(source_root, target_root);
    const auto backup_path = temp.path / "backups" / "candidate-acceptance";
    infrastructure::NativeFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto name = hero_name_change(profile);
    ASSERT_TRUE(name);
    const auto original_source = profile.documents.at("persist.roster.json").bytes;

    const auto result = application::SafeSaveCommitter{fs}.commit(
        profile, change_set({*name}), target_root, backup_path,
        application::SaveCommitMode::AcceptanceTestCandidate);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(read_bytes(source_root / "persist.roster.json"), original_source);
    EXPECT_EQ(read_bytes(backup_path / "persist.roster.json"), original_source);
    EXPECT_NE(read_bytes(target_root / "persist.roster.json"), original_source);
    EXPECT_EQ(result.value().committed_documents, (std::vector<std::string>{"persist.roster.json"}));
}

TEST(SafeSaveCommitter, BackupFailurePerformsZeroTargetWrites) {
    TempDirectory temp;
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    const auto target_root = temp.path / "output" / "profile_0";
    copy_profile(source_root, target_root);

    FaultInjectingFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto change = resource_amount_change(profile);
    ASSERT_TRUE(change);
    const auto before = read_bytes(target_root / "persist.estate.json");
    fs.fail_backup_writes = true;
    const auto result = application::SafeSaveCommitter{fs}.commit(
        profile, change_set({*change}), target_root, temp.path / "backups" / "failed-backup");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::BackupFailed);
    EXPECT_EQ(fs.atomic_write_count, 0U);
    EXPECT_EQ(read_bytes(target_root / "persist.estate.json"), before);
}

TEST(SafeSaveCommitter, SecondDocumentFailureIsReportedAsPartialCommitWithRecoveryBackup) {
    TempDirectory temp;
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    const auto target_root = temp.path / "output" / "profile_0";
    copy_profile(source_root, target_root);

    FaultInjectingFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto resource = resource_amount_change(profile);
    const auto hero_name = hero_resolve_xp_change(profile);
    ASSERT_TRUE(resource);
    ASSERT_TRUE(hero_name);
    const auto changes = change_set({*resource, *hero_name});
    ASSERT_EQ(changes.affected_documents,
              (std::vector<std::string>{"persist.estate.json", "persist.roster.json"}));
    const auto original_roster = read_bytes(target_root / "persist.roster.json");
    const auto backup_path = temp.path / "backups" / "partial-commit";
    fs.fail_atomic_write_number = 2;

    const auto result = application::SafeSaveCommitter{fs}.commit(profile, changes, target_root, backup_path);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::PartialCommit);
    EXPECT_EQ(result.error().context.at("partial_commit"), "true");
    EXPECT_EQ(result.error().context.at("committed_documents"), "persist.estate.json");
    EXPECT_EQ(result.error().context.at("failed_document"), "persist.roster.json");
    EXPECT_TRUE(std::filesystem::exists(backup_path / "persist.estate.json"));
    EXPECT_EQ(read_bytes(backup_path / "persist.roster.json"), original_roster);
    EXPECT_EQ(read_bytes(target_root / "persist.roster.json"), original_roster);
    EXPECT_NE(read_bytes(target_root / "persist.estate.json"), read_bytes(backup_path / "persist.estate.json"));
}

TEST(SafeSaveCommitter, ExternalTreeChangeRejectsCommitBeforeBackupOrWrite) {
    TempDirectory temp;
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    const auto target_root = temp.path / "output" / "profile_0";
    copy_profile(source_root, target_root);

    FaultInjectingFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto change = resource_amount_change(profile);
    ASSERT_TRUE(change);
    write_bytes(target_root / "extra" / "external-change.bin", "changed after open");
    const auto backup_path = temp.path / "backups" / "stale-target";

    const auto result = application::SafeSaveCommitter{fs}.commit(
        profile, change_set({*change}), target_root, backup_path);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::ConcurrentSaveChanged);
    EXPECT_EQ(fs.atomic_write_count, 0U);
    EXPECT_FALSE(std::filesystem::exists(backup_path));
}

TEST(SafeSaveCommitter, ReadBackFailureNeverReportsSuccessAndReturnsBackupLocation) {
    TempDirectory temp;
    const auto fixture = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "Optional local save sample is not present";
    const auto source_root = temp.path / "source" / "profile_0";
    copy_profile(fixture, source_root);
    const auto target_root = temp.path / "output" / "profile_0";
    copy_profile(source_root, target_root);

    FaultInjectingFileSystem fs;
    const auto profile = load_profile(fs, source_root);
    const auto change = resource_amount_change(profile);
    ASSERT_TRUE(change);
    const auto backup_path = temp.path / "backups" / "readback-failure";
    fs.corrupt_atomic_write = true;

    const auto result = application::SafeSaveCommitter{fs}.commit(
        profile, change_set({*change}), target_root, backup_path);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::PartialCommit);
    EXPECT_EQ(result.error().context.at("backup_directory"), backup_path.string());
    EXPECT_EQ(result.error().context.at("committed_documents"), "persist.estate.json");
    EXPECT_TRUE(std::filesystem::exists(backup_path / "persist.estate.json"));
}
