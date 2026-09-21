#pragma once

#include "ddse/application/file_system.hpp"

namespace ddse::infrastructure {

class NativeFileSystem final : public application::IFileSystem {
public:
    [[nodiscard]] core::Result<bool, core::Error> exists(const std::filesystem::path& path) const override;
    [[nodiscard]] core::Result<std::string, core::Error> read_file(const std::filesystem::path& path) const override;
    [[nodiscard]] core::Result<void, core::Error> write_file(const std::filesystem::path& path,
                                                              const std::string& bytes) override;
    [[nodiscard]] core::Result<void, core::Error> write_file_atomic(const std::filesystem::path& path,
                                                                    const std::string& bytes) override;
    [[nodiscard]] core::Result<void, core::Error> create_directories(const std::filesystem::path& path) override;
    [[nodiscard]] core::Result<std::vector<std::filesystem::path>, core::Error>
    list_files(const std::filesystem::path& directory) const override;
    [[nodiscard]] core::Result<std::vector<std::filesystem::path>, core::Error>
    list_directories(const std::filesystem::path& directory) const override;
    [[nodiscard]] core::Result<std::optional<std::filesystem::file_time_type>, core::Error>
    last_modified(const std::filesystem::path& path) const override;
    [[nodiscard]] core::Result<std::uint64_t, core::Error> file_size(const std::filesystem::path& path) const override;
};

} // namespace ddse::infrastructure
