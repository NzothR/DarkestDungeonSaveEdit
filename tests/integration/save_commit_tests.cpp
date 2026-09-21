#include "ddse/application/campaign_mappings.hpp"
#include "ddse/application/save_commit.hpp"
#include "ddse/application/save_profile.hpp"
#include "ddse/core/dson/dson_reader.hpp"
#include "ddse/infrastructure/native_file_system.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    const auto hero_name = hero_name_change(profile);
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
