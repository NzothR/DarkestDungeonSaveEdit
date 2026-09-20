#include "ddse/application/save_profile.hpp"
#include "ddse/infrastructure/native_file_system.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string minimal_dson() {
    std::vector<unsigned char> bytes(102, 0);
    auto put = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<unsigned char>((value >> (i * 8U)) & 0xffU);
    };
    bytes[0] = 1; bytes[1] = 0xb1;
    put(8, 64); put(16, 16); put(20, 1); put(24, 64);
    put(44, 1); put(48, 80); put(56, 10); put(60, 92);
    put(64, UINT32_MAX); put(68, 0); put(72, 0); put(76, 0);
    put(80, ddse::core::dson::string_hash("base_root")); put(84, 0); put(88, (10U << 2U) | 1U);
    const std::string root{"base_root\0", 10};
    std::copy(root.begin(), root.end(), reinterpret_cast<char*>(bytes.data() + 92));
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("ddse-stage3-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write(ddse::infrastructure::NativeFileSystem& fs, const std::filesystem::path& path,
           const std::string& data) {
    const auto result = fs.write_file(path, data);
    ASSERT_TRUE(result) << result.error().message;
}

bool has_domain(const ddse::application::RawSaveProfile& profile, ddse::application::SaveDomain domain) {
    const auto& domains = profile.descriptor.detected_domains;
    return std::find(domains.begin(), domains.end(), domain) != domains.end();
}

} // namespace

TEST(SaveProfileDiscovery, LoadsCampaignProfileAndTracksBaselineFingerprint) {
    ddse::infrastructure::NativeFileSystem fs;
    TempDirectory temp;
    const auto root = temp.path / "profile_0";
    std::filesystem::create_directories(root);
    const auto dson = minimal_dson();
    write(fs, root / "persist.game.json", dson);
    write(fs, root / "persist.roster.json", dson);
    write(fs, root / "mod_extension.bin", "opaque bytes");

    ddse::application::SaveProfileDiscovery discovery{fs};
    auto loaded = discovery.load(root);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded.value().descriptor.id, "profile_0");
    EXPECT_EQ(loaded.value().status, ddse::application::ProfileReadStatus::Complete);
    EXPECT_TRUE(has_domain(loaded.value(), ddse::application::SaveDomain::Campaign));
    ASSERT_EQ(loaded.value().documents.size(), 3U);
    EXPECT_TRUE(loaded.value().documents.at("persist.game.json").decoded.has_value());
    EXPECT_FALSE(loaded.value().documents.at("mod_extension.bin").bytes.empty());
    EXPECT_FALSE(loaded.value().documents.at("mod_extension.bin").decoded.has_value());
    auto unchanged = loaded.value().matches_disk_baseline(fs);
    ASSERT_TRUE(unchanged);
    EXPECT_TRUE(unchanged.value());

    write(fs, root / "mod_extension.bin", "externally changed");
    auto changed = loaded.value().matches_disk_baseline(fs);
    ASSERT_TRUE(changed);
    EXPECT_FALSE(changed.value());
}

TEST(SaveProfileDiscovery, DetectsCircusAndMixedDomains) {
    ddse::infrastructure::NativeFileSystem fs;
    TempDirectory temp;
    const auto dson = minimal_dson();
    const auto circus_root = temp.path / "profile_circus";
    std::filesystem::create_directories(circus_root);
    write(fs, circus_root / "persist.circus.json", dson);
    ddse::application::SaveProfileDiscovery discovery{fs};
    auto circus = discovery.load(circus_root);
    ASSERT_TRUE(circus) << circus.error().message;
    EXPECT_TRUE(has_domain(circus.value(), ddse::application::SaveDomain::Circus));
    EXPECT_FALSE(has_domain(circus.value(), ddse::application::SaveDomain::Campaign));

    const auto mixed_root = temp.path / "profile_mixed";
    std::filesystem::create_directories(mixed_root);
    write(fs, mixed_root / "persist.game.json", dson);
    write(fs, mixed_root / "persist.circus.json", dson);
    write(fs, mixed_root / "persist.shared.json", dson);
    auto mixed = discovery.load(mixed_root);
    ASSERT_TRUE(mixed) << mixed.error().message;
    EXPECT_TRUE(has_domain(mixed.value(), ddse::application::SaveDomain::Campaign));
    EXPECT_TRUE(has_domain(mixed.value(), ddse::application::SaveDomain::Circus));
    EXPECT_TRUE(has_domain(mixed.value(), ddse::application::SaveDomain::Shared));
}

TEST(SaveProfileDiscovery, DiscoversAllProfileDirectoriesInStableOrder) {
    ddse::infrastructure::NativeFileSystem fs;
    TempDirectory temp;
    const auto root = temp.path / "saves";
    std::filesystem::create_directories(root / "profile_2");
    std::filesystem::create_directories(root / "profile_0");
    std::filesystem::create_directories(root / "not-a-profile-file");
    const auto dson = minimal_dson();
    write(fs, root / "profile_2" / "persist.game.json", dson);
    write(fs, root / "profile_0" / "persist.game.json", dson);
    write(fs, root / "not-a-profile-file" / "persist.circus.json", dson);

    ddse::application::SaveProfileDiscovery discovery{fs};
    auto profiles = discovery.discover(root);
    ASSERT_TRUE(profiles) << profiles.error().message;
    ASSERT_EQ(profiles.value().size(), 2U);
    EXPECT_EQ(profiles.value()[0].descriptor.id, "profile_0");
    EXPECT_EQ(profiles.value()[1].descriptor.id, "profile_2");
}

TEST(SaveProfileDiscovery, MalformedNonCoreDocumentIsRetainedAsPartialReadOnly) {
    ddse::infrastructure::NativeFileSystem fs;
    TempDirectory temp;
    const auto root = temp.path / "profile_partial";
    std::filesystem::create_directories(root);
    const auto dson = minimal_dson();
    write(fs, root / "persist.game.json", dson);
    write(fs, root / "persist.tutorial.json", "not a DSON document");

    ddse::application::SaveProfileDiscovery discovery{fs};
    auto loaded = discovery.load(root);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded.value().status, ddse::application::ProfileReadStatus::PartialReadOnly);
    ASSERT_EQ(loaded.value().descriptor.diagnostics.size(), 1U);
    EXPECT_EQ(loaded.value().descriptor.diagnostics.front().document_id, "persist.tutorial.json");
    EXPECT_FALSE(loaded.value().descriptor.diagnostics.front().core_document);
    const auto& damaged = loaded.value().documents.at("persist.tutorial.json");
    EXPECT_TRUE(damaged.decode_error.has_value());
    EXPECT_EQ(damaged.bytes, "not a DSON document");
}

TEST(SaveProfileDiscovery, MalformedCoreDocumentIsReportedAndProfileRemainsDiscoverable) {
    ddse::infrastructure::NativeFileSystem fs;
    TempDirectory temp;
    const auto root = temp.path / "profile_core_broken";
    std::filesystem::create_directories(root);
    write(fs, root / "persist.game.json", "broken");

    ddse::application::SaveProfileDiscovery discovery{fs};
    auto loaded = discovery.load(root);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded.value().status, ddse::application::ProfileReadStatus::PartialReadOnly);
    ASSERT_EQ(loaded.value().descriptor.diagnostics.size(), 1U);
    EXPECT_TRUE(loaded.value().descriptor.diagnostics.front().core_document);
    EXPECT_TRUE(has_domain(loaded.value(), ddse::application::SaveDomain::Campaign));
}

TEST(SaveProfileDiscovery, LoadsTheProvidedProfileWithoutChangingItsFiles) {
    const std::filesystem::path root{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(root)) GTEST_SKIP() << "Optional local save sample is not present";
    ddse::infrastructure::NativeFileSystem fs;
    ddse::application::SaveProfileDiscovery discovery{fs};
    auto loaded = discovery.load(root);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded.value().descriptor.id, "profile_0");
    EXPECT_EQ(loaded.value().status, ddse::application::ProfileReadStatus::Complete);
    EXPECT_TRUE(has_domain(loaded.value(), ddse::application::SaveDomain::Campaign));
    EXPECT_EQ(loaded.value().documents.size(), 16U);
    EXPECT_TRUE(loaded.value().descriptor.modified_at.has_value());
    auto unchanged = loaded.value().matches_disk_baseline(fs);
    ASSERT_TRUE(unchanged);
    EXPECT_TRUE(unchanged.value());
}
