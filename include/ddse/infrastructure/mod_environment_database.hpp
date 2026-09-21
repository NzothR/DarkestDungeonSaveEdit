#pragma once

#include "ddse/application/mod_environment.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstddef>
#include <filesystem>

namespace ddse::infrastructure {

struct ModEnvironmentBuildSummary {
    std::size_t installed_mods{};
    std::size_t enabled_mods{};
    std::size_t workshop_mods{};
    std::size_t local_mods{};
    std::size_t source_files{};
    std::size_t definitions{};
    std::size_t localization_entries{};
    std::size_t assets{};
    std::size_t asset_references{};
    std::size_t relationships{};
    std::size_t effective_paths{};
    std::size_t overridden_paths{};
    std::size_t diagnostics{};
    bool save_order_matches_manager{};
    bool shared_order_matches{};
};

class ModEnvironmentDatabaseBuilder {
public:
    [[nodiscard]] core::Result<ModEnvironmentBuildSummary, core::Error>
    rebuild(const std::filesystem::path& database_path,
            const std::filesystem::path& base_content_database_path,
            const application::ModEnvironmentScanResult& scan) const;
};

} // namespace ddse::infrastructure
