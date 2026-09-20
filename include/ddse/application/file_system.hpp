#pragma once

#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <filesystem>
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
    [[nodiscard]] virtual core::Result<void, core::Error> create_directories(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual core::Result<std::vector<std::filesystem::path>, core::Error>
    list_files(const std::filesystem::path& directory) const = 0;
};

} // namespace ddse::application
