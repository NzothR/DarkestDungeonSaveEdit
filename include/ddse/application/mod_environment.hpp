#pragma once

#include "ddse/application/content_scanner.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ddse::application {

enum class ModOrderSource { SaveProfile, ManagerExport };

struct ModOrderEntry {
    ModOrderSource source{ModOrderSource::ManagerExport};
    std::size_t position{};
    std::optional<std::size_t> active_order;
    bool enabled{};
    std::string identity;
    std::string provider_id;
    std::string external_id;
    std::string mod_guid;
    std::string name;
    std::string version;
    std::string matched_mod_id;
};

struct ModSourceRecord {
    std::string id;
    std::string provider_id;
    std::string external_id;
    std::string mod_guid;
    std::string display_name;
    std::string version;
    std::filesystem::path root_path;
    bool enabled{};
    std::optional<std::size_t> active_order;
    std::string order_source;
};

struct ModListComparison {
    bool save_available{};
    bool manager_export_available{};
    bool exact_enabled_order_match{};
    bool shared_enabled_order_match{};
    std::vector<std::string> only_in_save;
    std::vector<std::string> only_in_manager_export;
};

struct ModScanDiagnostic {
    std::string mod_id;
    std::string virtual_path;
    std::string message;
};

struct ModEnvironmentScanConfig {
    std::filesystem::path workshop_root;
    std::vector<std::filesystem::path> local_mod_roots;
    std::filesystem::path save_profile_root;
    std::filesystem::path manager_order_json;
    std::filesystem::path base_content_database;
    bool prefer_manager_export{true};
};

struct ModEnvironmentScanResult {
    std::vector<ModSourceRecord> mods;
    std::vector<ModOrderEntry> save_order;
    std::vector<ModOrderEntry> manager_order;
    std::vector<std::string> effective_order;
    std::string effective_order_source;
    ModListComparison comparison;
    BaseContentScanResult content;
    std::vector<ModScanDiagnostic> diagnostics;
    std::vector<std::filesystem::path> protected_roots;
};

class ModEnvironmentScanner {
public:
    explicit ModEnvironmentScanner(const IFileSystem& file_system) : file_system_(file_system) {}

    [[nodiscard]] core::Result<ModEnvironmentScanResult, core::Error>
    scan(const ModEnvironmentScanConfig& config) const;

private:
    const IFileSystem& file_system_;
};

[[nodiscard]] std::string_view to_string(ModOrderSource source) noexcept;

} // namespace ddse::application
