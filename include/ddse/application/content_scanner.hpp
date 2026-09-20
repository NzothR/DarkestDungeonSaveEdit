#pragma once

#include "ddse/application/file_system.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ddse::application {

enum class ContentSourceType { Vanilla, Dlc };

struct ContentSourceRoot {
    std::string id;
    std::string name;
    ContentSourceType type{ContentSourceType::Vanilla};
    std::filesystem::path path;
};

struct BaseContentScanConfig {
    std::filesystem::path game_root;
    std::vector<std::string> vanilla_directories;
    std::string dlc_directory{"dlc"};
    std::vector<std::string> excluded_directories{"mods", "modes"};
    std::vector<std::string> content_extensions{".json", ".darkest", ".xml"};
    std::vector<std::string> asset_extensions{
        ".png", ".tga", ".atlas", ".skel", ".fnt", ".bank", ".wav", ".ogg", ".mp3"};
    std::string preferred_language{"english"};

    [[nodiscard]] static BaseContentScanConfig defaults(std::filesystem::path game_root);
};

struct ScannedContentSource {
    std::string id;
    std::string name;
    ContentSourceType type{ContentSourceType::Vanilla};
    std::string root_path;
};

struct ScannedSourceFile {
    std::string source_id;
    std::string virtual_path;
    std::string extension;
    std::uint64_t size_bytes{};
    std::uint64_t content_fingerprint{};
    bool is_asset{};
};

struct ScannedDefinition {
    std::string kind;
    std::string id;
    std::string source_id;
    std::string virtual_path;
    std::string display_name;
    std::string localization_key;
    std::string payload_json;
};

struct ScannedLocalization {
    std::string language;
    std::string key;
    std::string value;
    std::string source_id;
    std::string virtual_path;
};

struct ScannedAsset {
    std::string source_id;
    std::string virtual_path;
    std::string extension;
    std::uint64_t size_bytes{};
};

struct ContentScanDiagnostic {
    std::string source_id;
    std::string virtual_path;
    std::string message;
};

struct BaseContentScanResult {
    std::vector<ScannedContentSource> sources;
    std::vector<ScannedSourceFile> source_files;
    std::vector<ScannedDefinition> definitions;
    std::vector<ScannedLocalization> localizations;
    std::vector<ScannedAsset> assets;
    std::vector<ContentScanDiagnostic> diagnostics;
};

class BaseContentScanner {
public:
    explicit BaseContentScanner(const IFileSystem& file_system) : file_system_(file_system) {}

    [[nodiscard]] core::Result<BaseContentScanResult, core::Error>
    scan(const BaseContentScanConfig& config) const;

private:
    const IFileSystem& file_system_;
};

[[nodiscard]] std::string_view to_string(ContentSourceType type) noexcept;

} // namespace ddse::application
