#pragma once

#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ddse::infrastructure {

struct DatabaseModRecord {
    std::size_t order{};
    std::string key;
    std::string provider_id;
    std::string external_id;
    std::string display_name;
    std::string fallback_name;
    std::string matched_mod_id;
    std::filesystem::path root_path;
    bool enabled{};
};

[[nodiscard]] core::Result<std::vector<DatabaseModRecord>, core::Error>
read_enabled_mods(const std::filesystem::path& database_path);

[[nodiscard]] core::Result<std::filesystem::path, core::Error>
find_mod_cover(const std::filesystem::path& database_path, std::string_view mod_id);

} // namespace ddse::infrastructure
