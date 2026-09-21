#pragma once

#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ddse::application {

class IFileSystem {
public:
    virtual ~IFileSystem() = default;
    [[nodiscard]] virtual core::Result<bool, core::Error> exists(const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual core::Result<std::string, core::Error> read_file(const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual core::Result<void, core::Error> write_file(const std::filesystem::path& path,
                                                                     const std::string& bytes) = 0;
    [[nodiscard]] virtual core::Result<void, core::Error> write_file_atomic(const std::filesystem::path& path,
                                                                            const std::string& bytes) = 0;
    [[nodiscard]] virtual core::Result<void, core::Error> create_directories(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual core::Result<std::vector<std::filesystem::path>, core::Error>
    list_files(const std::filesystem::path& directory) const = 0;
    [[nodiscard]] virtual core::Result<std::vector<std::filesystem::path>, core::Error>
    list_directories(const std::filesystem::path& directory) const = 0;
    [[nodiscard]] virtual core::Result<std::optional<std::filesystem::file_time_type>, core::Error>
    last_modified(const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual core::Result<std::uint64_t, core::Error>
    file_size(const std::filesystem::path& path) const = 0;
};

} // namespace ddse::application
