#pragma once

#include "ddse/application/content_scanner.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstddef>
#include <filesystem>

namespace ddse::infrastructure {

struct BaseContentBuildSummary {
    std::size_t sources{};
    std::size_t source_files{};
    std::size_t hero_classes{};
    std::size_t skills{};
    std::size_t trinkets{};
    std::size_t quirks{};
    std::size_t diseases{};
    std::size_t resources{};
    std::size_t buildings{};
    std::size_t localization_entries{};
    std::size_t assets{};
    std::size_t asset_references{};
    std::size_t relationships{};
    std::size_t diagnostics{};
};

class BaseContentDatabaseBuilder {
public:
    [[nodiscard]] core::Result<BaseContentBuildSummary, core::Error>
    rebuild(const std::filesystem::path& database_path,
            const application::BaseContentScanResult& scan) const;
};

} // namespace ddse::infrastructure
