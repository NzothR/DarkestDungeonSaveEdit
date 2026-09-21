#pragma once

#include "ddse/application/file_system.hpp"
#include "ddse/core/dson/dson_document.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ddse::application {

enum class SaveDomain { Campaign, Circus, Raid, Shared, Unknown };
enum class ProfileReadStatus { Complete, PartialReadOnly };

struct SaveDiagnostic {
    std::string document_id;
    std::string message;
    bool core_document{};
};

struct SaveProfileDescriptor {
    std::string id;
    std::filesystem::path root_path;
    std::vector<std::string> document_ids;
    std::vector<SaveDomain> detected_domains;
    std::optional<std::filesystem::file_time_type> modified_at;
    std::vector<SaveDiagnostic> diagnostics;
};

struct RawSaveDocument {
    std::string id;
    std::filesystem::path path;
    std::string bytes;
    std::uint64_t baseline_fingerprint{};
    std::vector<SaveDomain> domains;
    bool core_document{};
    std::optional<core::dson::DsonDocument> decoded;
    std::optional<core::Error> decode_error;
};

struct RawSaveProfile {
    SaveProfileDescriptor descriptor;
    ProfileReadStatus status{ProfileReadStatus::Complete};
    std::map<std::string, RawSaveDocument, std::less<>> documents;
    std::uint64_t baseline_fingerprint{};

    [[nodiscard]] core::Result<bool, core::Error> matches_disk_baseline(const IFileSystem& file_system) const;
};

class SaveProfileDiscovery {
public:
    explicit SaveProfileDiscovery(const IFileSystem& file_system) : file_system_(file_system) {}

    [[nodiscard]] core::Result<std::vector<RawSaveProfile>, core::Error>
    discover(const std::filesystem::path& save_root) const;
    [[nodiscard]] core::Result<RawSaveProfile, core::Error> load(const std::filesystem::path& profile_root) const;

    [[nodiscard]] static std::uint64_t fingerprint(std::string_view bytes) noexcept;
    [[nodiscard]] static core::Result<std::uint64_t, core::Error>
    fingerprint_profile(const IFileSystem& file_system, const std::filesystem::path& profile_root);

private:
    const IFileSystem& file_system_;
};

[[nodiscard]] std::string_view to_string(SaveDomain domain) noexcept;
[[nodiscard]] std::string_view to_string(ProfileReadStatus status) noexcept;

} // namespace ddse::application
