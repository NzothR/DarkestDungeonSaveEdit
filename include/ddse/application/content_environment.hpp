#pragma once

#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ddse::application {

struct ContentEnvironmentSelection {
    std::string language{"english"};
    std::string fallback_language{"english"};
    // Values are Base Content source IDs, for example "dlc:580100_crimson_court".
    std::vector<std::string> enabled_dlc_sources;
};

struct ContentProvenance {
    std::string source_id;
    std::string layer_type;
    std::string virtual_path;
    bool selected{};
};

struct ResolvedAsset {
    std::string virtual_path;
    std::string layer_type;
    std::string source_id;
    std::string extension;
    std::uint64_t size_bytes{};
    std::string file_kind;
    std::filesystem::path physical_path;
};

struct ContentRelationship {
    std::string type;
    std::string id;
    std::string relationship_type;
};

struct ContentAssetReference {
    std::string role;
    std::string reference_type;
    std::string reference_origin;
    std::string virtual_path;
    std::optional<ResolvedAsset> resolved_asset;
};

struct ContentDefinition {
    std::string type;
    std::string id;
    std::string display_name;
    std::string localization_key;
    std::string localized_name;
    std::string payload_json;
    ContentProvenance provenance;
    std::size_t overridden_definition_count{};
    std::vector<ContentRelationship> relationships;
    std::vector<ContentAssetReference> asset_references;
};

struct ContentBundle {
    ContentDefinition definition;
    std::vector<ContentDefinition> related_definitions;
    std::vector<ResolvedAsset> assets;
};

struct ResolvedLocalization {
    std::string key;
    std::string requested_language;
    std::string resolved_language;
    std::string value;
    ContentProvenance provenance;
};

class IContentEnvironment {
public:
    virtual ~IContentEnvironment() = default;

    [[nodiscard]] virtual core::Result<std::optional<ContentDefinition>, core::Error>
    find_content(std::string_view type, std::string_view id) const = 0;

    [[nodiscard]] virtual core::Result<std::vector<ContentDefinition>, core::Error>
    list_content(std::string_view type, std::string_view search_text = {}) const = 0;

    [[nodiscard]] virtual core::Result<std::optional<ContentBundle>, core::Error>
    load_bundle(std::string_view type, std::string_view id) const = 0;

    [[nodiscard]] virtual core::Result<std::optional<ResolvedLocalization>, core::Error>
    resolve_localization(std::string_view key, std::string_view language = {}) const = 0;

    [[nodiscard]] virtual core::Result<std::optional<ResolvedAsset>, core::Error>
    resolve_asset(std::string_view virtual_path) const = 0;

    [[nodiscard]] virtual core::Result<std::vector<ContentProvenance>, core::Error>
    explain_provenance(std::string_view type, std::string_view id) const = 0;
};

} // namespace ddse::application
