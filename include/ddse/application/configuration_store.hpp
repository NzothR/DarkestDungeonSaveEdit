#pragma once

#include "ddse/application/app_configuration.hpp"
#include "ddse/application/file_system.hpp"
#include "ddse/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace ddse::application {

struct RecoveryStatus {
    bool available{};
    std::string profile_id;
    std::filesystem::path source_profile;
    std::uint64_t source_fingerprint{};
    std::uint64_t revision{};
    std::string saved_at;
    std::string path;
};

// Owns the persisted application settings and the small recovery marker used
// by the F1 initialization flow.  Session document serialization is supplied
// by the editor session in later slices; this class deliberately only handles
// configuration and recovery metadata.
class AppConfigurationStore {
public:
    AppConfigurationStore(IFileSystem& file_system, std::filesystem::path configuration_file,
                          std::filesystem::path default_data_root);

    [[nodiscard]] core::Result<AppConfiguration, core::Error> initialize();
    [[nodiscard]] core::Result<AppConfiguration, core::Error> load() const;
    [[nodiscard]] core::Result<void, core::Error> save(const AppConfiguration& configuration);
    [[nodiscard]] const AppConfiguration& current() const noexcept { return configuration_; }
    [[nodiscard]] IFileSystem& file_system() const noexcept { return file_system_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] const std::filesystem::path& configuration_file() const noexcept { return configuration_file_; }
    [[nodiscard]] std::filesystem::path auto_edit_save_directory() const;
    [[nodiscard]] core::Result<RecoveryStatus, core::Error> recovery_status() const;
    [[nodiscard]] core::Result<std::string, core::Error> recovery_snapshot() const;
    [[nodiscard]] core::Result<void, core::Error> write_recovery_marker(
        std::string profile_id, const std::filesystem::path& source_profile,
        std::uint64_t source_fingerprint, std::uint64_t revision, std::string saved_at,
        std::string session_snapshot);
    [[nodiscard]] core::Result<void, core::Error> discard_recovery();

private:
    [[nodiscard]] static AppConfiguration defaults(const std::filesystem::path& data_root);
    [[nodiscard]] core::Result<void, core::Error> ensure_storage() const;

    IFileSystem& file_system_;
    std::filesystem::path configuration_file_;
    std::filesystem::path default_data_root_;
    AppConfiguration configuration_;
    bool initialized_{};
};

} // namespace ddse::application
