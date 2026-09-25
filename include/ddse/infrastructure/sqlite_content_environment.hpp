#pragma once

#include "ddse/application/content_environment.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>

namespace ddse::infrastructure {

struct SqliteContentEnvironmentConfig {
    std::filesystem::path base_content_database;
    std::filesystem::path mod_environment_database;
    application::ContentEnvironmentSelection selection;
};

struct SqliteContentDatabasePair;

class SqliteContentEnvironment final : public application::IContentEnvironment {
public:
    explicit SqliteContentEnvironment(SqliteContentEnvironmentConfig config);
    ~SqliteContentEnvironment() override;

    [[nodiscard]] core::Result<std::optional<application::ContentDefinition>, core::Error>
    find_content(std::string_view type, std::string_view id) const override;

    [[nodiscard]] core::Result<std::vector<application::ContentDefinition>, core::Error>
    list_content(std::string_view type, std::string_view search_text = {}) const override;

    [[nodiscard]] core::Result<std::optional<application::ContentBundle>, core::Error>
    load_bundle(std::string_view type, std::string_view id) const override;

    [[nodiscard]] core::Result<std::optional<application::ResolvedLocalization>, core::Error>
    resolve_localization(std::string_view key, std::string_view language = {}) const override;

    [[nodiscard]] core::Result<std::optional<application::ResolvedAsset>, core::Error>
    resolve_asset(std::string_view virtual_path) const override;

    [[nodiscard]] core::Result<std::vector<application::ContentProvenance>, core::Error>
    explain_provenance(std::string_view type, std::string_view id) const override;

private:
    [[nodiscard]] core::Result<SqliteContentDatabasePair*, core::Error> databases() const;
    SqliteContentEnvironmentConfig config_;
    mutable std::mutex database_mutex_;
    mutable std::unique_ptr<SqliteContentDatabasePair> databases_;
};

} // namespace ddse::infrastructure
