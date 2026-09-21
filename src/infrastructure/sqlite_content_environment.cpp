#include "ddse/infrastructure/sqlite_content_environment.hpp"

#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/statement.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace ddse::infrastructure {
namespace {

using application::ContentAssetReference;
using application::ContentBundle;
using application::ContentDefinition;
using application::ContentEnvironmentSelection;
using application::ContentProvenance;
using application::ResolvedLocalization;
using application::ResolvedAsset;
using sqlite::Database;
using sqlite::Statement;

struct DatabasePair {
    Database base;
    std::optional<Database> mods;
};

struct DefinitionLayer {
    std::string type;
    std::string id;
    std::string layer;
    std::string source_id;
    std::string path;
    std::string payload;
    std::int64_t rank{};
};

struct FileLayer {
    std::string path;
    std::string layer;
    std::string source_id;
    std::string file_kind;
    std::int64_t rank{};
};

core::Error query_error(const core::Error& error, std::string_view operation) {
    return {error.code, error.message, "SqliteContentEnvironment",
            {{"operation", std::string{operation}}}, std::make_shared<core::Error>(error)};
}

core::Result<DatabasePair, core::Error> open_databases(const SqliteContentEnvironmentConfig& config) {
    if (config.base_content_database.empty())
        return core::Result<DatabasePair, core::Error>::failure(
            {core::ErrorCode::InvalidConfiguration, "Base content database path is required",
             "SqliteContentEnvironment", {}});
    auto base = sqlite::ConnectionFactory{}.open(config.base_content_database);
    if (!base) return core::Result<DatabasePair, core::Error>::failure(query_error(base.error(), "open_base"));
    std::optional<Database> mods;
    if (!config.mod_environment_database.empty()) {
        auto mod_database = sqlite::ConnectionFactory{}.open(config.mod_environment_database);
        if (!mod_database)
            return core::Result<DatabasePair, core::Error>::failure(query_error(mod_database.error(), "open_mod_environment"));
        mods.emplace(std::move(mod_database.value()));
    }
    return core::Result<DatabasePair, core::Error>::success(
        DatabasePair{std::move(base.value()), std::move(mods)});
}

bool source_is_enabled(std::string_view layer, std::string_view source_id,
                       const ContentEnvironmentSelection& selection) {
    if (layer == "mod" || source_id == "vanilla") return true;
    if (source_id.starts_with("dlc:"))
        return std::find(selection.enabled_dlc_sources.begin(), selection.enabled_dlc_sources.end(), source_id) !=
               selection.enabled_dlc_sources.end();
    return false;
}

std::filesystem::path physical_path(std::string_view source_root, std::string_view virtual_path) {
    const auto make_path = [](std::string_view value) {
        std::u8string encoded;
        encoded.reserve(value.size());
        for (const auto character : value) encoded.push_back(static_cast<char8_t>(character));
        return std::filesystem::path{encoded};
    };
    return make_path(source_root) / make_path(virtual_path);
}

core::Result<std::vector<DefinitionLayer>, core::Error>
read_definition_layers(Database& database, std::string_view type,
                       std::optional<std::string_view> id, bool environment_database) {
    const auto sql = environment_database
        ? (id ? "SELECT definition_type,content_id,layer_type,source_id,virtual_path,payload_json,chain_rank "
                "FROM effective_definition_chain WHERE definition_type=? AND content_id=? ORDER BY chain_rank"
              : "SELECT definition_type,content_id,layer_type,source_id,virtual_path,payload_json,chain_rank "
                "FROM effective_definition_chain WHERE definition_type=? ORDER BY content_id,chain_rank")
        : (id ? "SELECT d.definition_type,d.content_id,s.source_type,s.source_id,f.virtual_path,d.payload_json,0 "
                "FROM content_definitions d JOIN source_files f USING(source_file_id) "
                "JOIN content_sources s USING(source_id) WHERE d.definition_type=? AND d.content_id=? "
                "ORDER BY CASE s.source_type WHEN 'Dlc' THEN 0 ELSE 1 END,s.source_id"
              : "SELECT d.definition_type,d.content_id,s.source_type,s.source_id,f.virtual_path,d.payload_json,0 "
                "FROM content_definitions d JOIN source_files f USING(source_file_id) "
                "JOIN content_sources s USING(source_id) WHERE d.definition_type=? ORDER BY d.content_id,s.source_type DESC,s.source_id");
    auto prepared = database.prepare(sql);
    if (!prepared) return core::Result<std::vector<DefinitionLayer>, core::Error>::failure(query_error(prepared.error(), "prepare_definitions"));
    auto statement = std::move(prepared.value());
    auto bound = statement.bind(1, type);
    if (!bound) return core::Result<std::vector<DefinitionLayer>, core::Error>::failure(query_error(bound.error(), "bind_definition_type"));
    if (id) {
        bound = statement.bind(2, *id);
        if (!bound) return core::Result<std::vector<DefinitionLayer>, core::Error>::failure(query_error(bound.error(), "bind_definition_id"));
    }
    std::vector<DefinitionLayer> result;
    while (true) {
        auto row = statement.step();
        if (!row) return core::Result<std::vector<DefinitionLayer>, core::Error>::failure(query_error(row.error(), "read_definitions"));
        if (!row.value()) break;
        result.push_back({std::string{statement.column_text(0)}, std::string{statement.column_text(1)},
                          std::string{statement.column_text(2)}, std::string{statement.column_text(3)},
                          std::string{statement.column_text(4)}, std::string{statement.column_text(5)},
                          statement.column_int64(6)});
    }
    return core::Result<std::vector<DefinitionLayer>, core::Error>::success(std::move(result));
}

core::Result<std::pair<std::string, std::string>, core::Error>
read_definition_labels(Database& database, const DefinitionLayer& layer, bool environment_database) {
    const auto sql = environment_database
        ? "SELECT d.display_name,d.localization_key FROM content_definitions d "
          "JOIN source_files f USING(source_file_id) WHERE d.definition_type=? AND d.content_id=? "
          "AND f.mod_id=? AND f.virtual_path=? ORDER BY d.definition_id LIMIT 1"
        : "SELECT d.display_name,d.localization_key FROM content_definitions d "
          "JOIN source_files f USING(source_file_id) WHERE d.definition_type=? AND d.content_id=? "
          "AND f.source_id=? AND f.virtual_path=? ORDER BY d.definition_id LIMIT 1";
    auto prepared = database.prepare(sql);
    if (!prepared) return core::Result<std::pair<std::string, std::string>, core::Error>::failure(query_error(prepared.error(), "prepare_definition_labels"));
    auto statement = std::move(prepared.value());
    for (const auto& [index, value] : std::array<std::pair<int, std::string_view>, 4>{
             std::pair{1, std::string_view{layer.type}}, {2, layer.id}, {3, layer.source_id}, {4, layer.path}}) {
        auto bound = statement.bind(index, value);
        if (!bound) return core::Result<std::pair<std::string, std::string>, core::Error>::failure(query_error(bound.error(), "bind_definition_labels"));
    }
    auto row = statement.step();
    if (!row) return core::Result<std::pair<std::string, std::string>, core::Error>::failure(query_error(row.error(), "read_definition_labels"));
    if (!row.value()) return core::Result<std::pair<std::string, std::string>, core::Error>::success({{}, {}});
    return core::Result<std::pair<std::string, std::string>, core::Error>::success(
        {std::string{statement.column_text(0)}, std::string{statement.column_text(1)}});
}

core::Result<std::optional<application::ResolvedLocalization>, core::Error>
resolve_localization_in(DatabasePair& databases, const ContentEnvironmentSelection& selection,
                        std::string_view key, std::string_view requested_language,
                        bool environment_database) {
    const std::string language = requested_language.empty() ? selection.language : std::string{requested_language};
    std::vector<std::string> languages{language};
    if (!selection.fallback_language.empty() && selection.fallback_language != language)
        languages.push_back(selection.fallback_language);
    if (std::find(languages.begin(), languages.end(), "english") == languages.end()) languages.emplace_back("english");

    for (const auto& candidate_language : languages) {
        if (environment_database && databases.mods) {
            auto prepared = databases.mods->prepare(
                "SELECT layer_type,source_id,virtual_path,localized_text FROM effective_localization_chain "
                "WHERE language=? AND localization_key=? ORDER BY chain_rank");
            if (!prepared) return core::Result<std::optional<application::ResolvedLocalization>, core::Error>::failure(query_error(prepared.error(), "prepare_localization_chain"));
            auto statement = std::move(prepared.value());
            auto bound = statement.bind(1, candidate_language);
            if (!bound) return core::Result<std::optional<application::ResolvedLocalization>, core::Error>::failure(query_error(bound.error(), "bind_localization_language"));
            bound = statement.bind(2, key);
            if (!bound) return core::Result<std::optional<application::ResolvedLocalization>, core::Error>::failure(query_error(bound.error(), "bind_localization_key"));
            while (true) {
                auto row = statement.step();
                if (!row) return core::Result<std::optional<application::ResolvedLocalization>, core::Error>::failure(query_error(row.error(), "read_localization_chain"));
                if (!row.value()) break;
                const auto layer = std::string{statement.column_text(0)};
                const auto source = std::string{statement.column_text(1)};
                if (!source_is_enabled(layer, source, selection)) continue;
                application::ResolvedLocalization resolved;
                resolved.key = std::string{key};
                resolved.requested_language = language;
                resolved.resolved_language = candidate_language;
                resolved.value = std::string{statement.column_text(3)};
                resolved.provenance = {source, layer, std::string{statement.column_text(2)}, true};
                return core::Result<std::optional<application::ResolvedLocalization>, core::Error>::success(std::move(resolved));
            }
        } else {
            auto prepared = databases.base.prepare(
                "SELECT l.localized_text,s.source_type,s.source_id,f.virtual_path FROM localization_entries l "
                "JOIN source_files f USING(source_file_id) JOIN content_sources s USING(source_id) "
                "WHERE l.language=? AND l.localization_key=? ORDER BY CASE s.source_type WHEN 'Dlc' THEN 0 ELSE 1 END,s.source_id");
            if (!prepared) return core::Result<std::optional<application::ResolvedLocalization>, core::Error>::failure(query_error(prepared.error(), "prepare_base_localization"));
            auto statement = std::move(prepared.value());
            auto bound = statement.bind(1, candidate_language);
            if (!bound) return core::Result<std::optional<application::ResolvedLocalization>, core::Error>::failure(query_error(bound.error(), "bind_base_localization_language"));
            bound = statement.bind(2, key);
            if (!bound) return core::Result<std::optional<application::ResolvedLocalization>, core::Error>::failure(query_error(bound.error(), "bind_base_localization_key"));
            while (true) {
                auto row = statement.step();
                if (!row) return core::Result<std::optional<application::ResolvedLocalization>, core::Error>::failure(query_error(row.error(), "read_base_localization"));
                if (!row.value()) break;
                const auto source = std::string{statement.column_text(2)};
                if (!source_is_enabled(statement.column_text(1), source, selection)) continue;
                application::ResolvedLocalization resolved;
                resolved.key = std::string{key};
                resolved.requested_language = language;
                resolved.resolved_language = candidate_language;
                resolved.value = std::string{statement.column_text(0)};
                resolved.provenance = {source, std::string{statement.column_text(1)},
                                       std::string{statement.column_text(3)}, true};
                return core::Result<std::optional<application::ResolvedLocalization>, core::Error>::success(std::move(resolved));
            }
        }
    }
    return core::Result<std::optional<application::ResolvedLocalization>, core::Error>::success(std::nullopt);
}

core::Result<std::optional<ResolvedAsset>, core::Error>
resolve_asset_in(DatabasePair& databases, const ContentEnvironmentSelection& selection,
                 std::string_view path, bool environment_database) {
    if (environment_database && databases.mods) {
        auto prepared = databases.mods->prepare(
            "SELECT layer_type,source_id,virtual_path,file_kind FROM effective_vfs_chain "
            "WHERE virtual_path=? ORDER BY chain_rank");
        if (!prepared) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(query_error(prepared.error(), "prepare_asset_chain"));
        auto statement = std::move(prepared.value());
        auto bound = statement.bind(1, path);
        if (!bound) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(query_error(bound.error(), "bind_asset_path"));
        while (true) {
            auto row = statement.step();
            if (!row) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(query_error(row.error(), "read_asset_chain"));
            if (!row.value()) break;
            const auto layer = std::string{statement.column_text(0)};
            const auto source = std::string{statement.column_text(1)};
            if (!source_is_enabled(layer, source, selection)) continue;
            std::string extension;
            const auto dot = path.find_last_of('.');
            if (dot != std::string_view::npos) extension = std::string{path.substr(dot)};
            std::uint64_t size = 0;
            if (layer == "mod") {
                auto size_query = databases.mods->prepare(
                    "SELECT f.size_bytes,m.root_path FROM source_files f JOIN mod_sources m USING(mod_id) "
                    "WHERE f.mod_id=? AND f.virtual_path=?");
                if (!size_query) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(
                    query_error(size_query.error(), "prepare_mod_asset_source"));
                auto size_statement = std::move(size_query.value());
                auto source_bound = size_statement.bind(1, source);
                if (!source_bound) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(
                    query_error(source_bound.error(), "bind_mod_asset_source"));
                source_bound = size_statement.bind(2, path);
                if (!source_bound) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(
                    query_error(source_bound.error(), "bind_mod_asset_path"));
                auto size_row = size_statement.step();
                if (!size_row) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(
                    query_error(size_row.error(), "read_mod_asset_source"));
                if (!size_row.value())
                    return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(
                        {core::ErrorCode::DatabaseError, "Effective mod asset has no source file record",
                         "SqliteContentEnvironment", {{"source_id", source}, {"virtual_path", std::string{path}}}});
                size = static_cast<std::uint64_t>(size_statement.column_int64(0));
                const auto root = std::string{size_statement.column_text(1)};
                return core::Result<std::optional<ResolvedAsset>, core::Error>::success(
                    ResolvedAsset{std::string{path}, layer, source, std::move(extension), size,
                                  std::string{statement.column_text(3)}, physical_path(root, path)});
            }
            std::string root;
            if (layer != "mod") {
                auto root_query = databases.base.prepare(
                    "SELECT s.root_path FROM source_files f JOIN content_sources s USING(source_id) "
                    "WHERE s.source_id=? AND f.virtual_path=? LIMIT 1");
                if (!root_query) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(
                    query_error(root_query.error(), "prepare_base_asset_source"));
                auto root_statement = std::move(root_query.value());
                auto source_bound = root_statement.bind(1, source);
                if (!source_bound) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(
                    query_error(source_bound.error(), "bind_base_asset_source"));
                source_bound = root_statement.bind(2, path);
                if (!source_bound) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(
                    query_error(source_bound.error(), "bind_base_asset_path"));
                auto root_row = root_statement.step();
                if (!root_row) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(
                    query_error(root_row.error(), "read_base_asset_source"));
                if (!root_row.value())
                    return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(
                        {core::ErrorCode::DatabaseError, "Effective base asset has no source file record",
                         "SqliteContentEnvironment", {{"source_id", source}, {"virtual_path", std::string{path}}}});
                root = std::string{root_statement.column_text(0)};
            }
            return core::Result<std::optional<ResolvedAsset>, core::Error>::success(
                ResolvedAsset{std::string{path}, layer, source, std::move(extension), size,
                              std::string{statement.column_text(3)}, physical_path(root, path)});
        }
        return core::Result<std::optional<ResolvedAsset>, core::Error>::success(std::nullopt);
    }

    auto prepared = databases.base.prepare(
        "SELECT s.source_type,s.source_id,f.extension,f.size_bytes,f.file_kind,s.root_path FROM source_files f "
        "JOIN content_sources s USING(source_id) WHERE f.virtual_path=? "
        "ORDER BY CASE s.source_type WHEN 'Dlc' THEN 0 ELSE 1 END,s.source_id");
    if (!prepared) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(query_error(prepared.error(), "prepare_base_asset"));
    auto statement = std::move(prepared.value());
    auto bound = statement.bind(1, path);
    if (!bound) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(query_error(bound.error(), "bind_base_asset_path"));
    while (true) {
        auto row = statement.step();
        if (!row) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(query_error(row.error(), "read_base_asset"));
        if (!row.value()) break;
        const auto layer = std::string{statement.column_text(0)};
        const auto source = std::string{statement.column_text(1)};
        if (!source_is_enabled(layer, source, selection)) continue;
        return core::Result<std::optional<ResolvedAsset>, core::Error>::success(
            ResolvedAsset{std::string{path}, layer, source, std::string{statement.column_text(2)},
                          static_cast<std::uint64_t>(statement.column_int64(3)),
                          std::string{statement.column_text(4)}, physical_path(statement.column_text(5), path)});
    }
    return core::Result<std::optional<ResolvedAsset>, core::Error>::success(std::nullopt);
}

std::string escape_like(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        if (c == '%' || c == '_' || c == '\\') result.push_back('\\');
        result.push_back(c);
    }
    return result;
}

core::Result<std::vector<FileLayer>, core::Error>
read_file_layers(Database& database, std::string_view prefix) {
    auto prepared = database.prepare(
        "SELECT virtual_path,layer_type,source_id,file_kind,chain_rank FROM effective_vfs_chain "
        "WHERE virtual_path LIKE ? ESCAPE '\\' ORDER BY virtual_path,chain_rank");
    if (!prepared) return core::Result<std::vector<FileLayer>, core::Error>::failure(query_error(prepared.error(), "prepare_asset_prefix"));
    auto statement = std::move(prepared.value());
    std::string pattern = escape_like(prefix);
    pattern.push_back('%');
    auto bound = statement.bind(1, pattern);
    if (!bound) return core::Result<std::vector<FileLayer>, core::Error>::failure(query_error(bound.error(), "bind_asset_prefix"));
    std::vector<FileLayer> result;
    while (true) {
        auto row = statement.step();
        if (!row) return core::Result<std::vector<FileLayer>, core::Error>::failure(query_error(row.error(), "read_asset_prefix"));
        if (!row.value()) break;
        result.push_back({std::string{statement.column_text(0)}, std::string{statement.column_text(1)},
                          std::string{statement.column_text(2)}, std::string{statement.column_text(3)},
                          statement.column_int64(4)});
    }
    return core::Result<std::vector<FileLayer>, core::Error>::success(std::move(result));
}

bool is_animation_descriptor(std::string_view path) {
    if (path.size() < 5 || path.substr(path.size() - 5) != ".json") return false;
    return path.find("/anim/") != std::string_view::npos;
}

core::Result<std::vector<ContentAssetReference>, core::Error>
read_asset_references(DatabasePair& databases, const ContentEnvironmentSelection& selection,
                      const DefinitionLayer& layer, bool environment_database) {
    std::vector<ContentAssetReference> result;
    if (environment_database && databases.mods) {
        auto prepared = databases.mods->prepare(
            "SELECT asset_role,reference_type,reference_origin,virtual_path FROM content_asset_references "
            "WHERE definition_type=? AND content_id=? AND definition_source_id=? AND definition_virtual_path=? "
            "ORDER BY asset_role,virtual_path");
        if (!prepared) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(query_error(prepared.error(), "prepare_content_asset_references"));
        auto statement = std::move(prepared.value());
        for (const auto& [index, value] : std::array<std::pair<int, std::string_view>, 4>{
                 std::pair{1, std::string_view{layer.type}}, {2, layer.id}, {3, layer.source_id}, {4, layer.path}}) {
            auto bound = statement.bind(index, value);
            if (!bound) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(query_error(bound.error(), "bind_content_asset_reference"));
        }
        while (true) {
            auto row = statement.step();
            if (!row) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(query_error(row.error(), "read_content_asset_references"));
            if (!row.value()) break;
            ContentAssetReference reference{std::string{statement.column_text(0)},
                std::string{statement.column_text(1)}, std::string{statement.column_text(2)},
                std::string{statement.column_text(3)}, std::nullopt};
            if (reference.reference_type == "directory") {
                auto paths = read_file_layers(*databases.mods, reference.virtual_path);
                if (!paths) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(paths.error());
                std::set<std::string, std::less<>> resolved_paths;
                for (const auto& file : paths.value()) {
                    if (file.file_kind != "asset" && !is_animation_descriptor(file.path)) continue;
                    if (!source_is_enabled(file.layer, file.source_id, selection)) continue;
                    if (!resolved_paths.insert(file.path).second) continue;
                    auto resolved = resolve_asset_in(databases, selection, file.path, true);
                    if (!resolved) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(resolved.error());
                    if (!resolved.value()) continue;
                    auto expanded = reference;
                    expanded.reference_type = "file";
                    expanded.virtual_path = file.path;
                    expanded.resolved_asset = std::move(*resolved.value());
                    result.push_back(std::move(expanded));
                }
            } else {
                auto resolved = resolve_asset_in(databases, selection, reference.virtual_path, true);
                if (!resolved) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(resolved.error());
                reference.resolved_asset = std::move(resolved.value());
                result.push_back(std::move(reference));
            }
        }
        return core::Result<std::vector<ContentAssetReference>, core::Error>::success(std::move(result));
    }

    auto prepared = databases.base.prepare(
        "SELECT r.asset_role,r.reference_type,r.reference_origin,r.virtual_path FROM content_asset_references r "
        "JOIN source_files f USING(source_file_id) WHERE r.definition_type=? AND r.content_id=? "
        "AND f.source_id=? AND f.virtual_path=? ORDER BY r.asset_role,r.virtual_path");
    if (!prepared) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(query_error(prepared.error(), "prepare_base_asset_references"));
    auto statement = std::move(prepared.value());
    for (const auto& [index, value] : std::array<std::pair<int, std::string_view>, 4>{
             std::pair{1, std::string_view{layer.type}}, {2, layer.id}, {3, layer.source_id}, {4, layer.path}}) {
        auto bound = statement.bind(index, value);
        if (!bound) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(query_error(bound.error(), "bind_base_asset_reference"));
    }
    while (true) {
        auto row = statement.step();
        if (!row) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(query_error(row.error(), "read_base_asset_reference"));
        if (!row.value()) break;
        ContentAssetReference reference{std::string{statement.column_text(0)}, std::string{statement.column_text(1)},
            std::string{statement.column_text(2)}, std::string{statement.column_text(3)}, std::nullopt};
        if (reference.reference_type == "directory") {
            // A base-only query can still expand the directory from the Base catalog.
            auto files = databases.base.prepare(
                "SELECT f.virtual_path,s.source_type,s.source_id,f.file_kind,f.extension,f.size_bytes,s.root_path FROM source_files f "
                "JOIN content_sources s USING(source_id) WHERE f.virtual_path LIKE ? ESCAPE '\\' ORDER BY f.virtual_path");
            if (!files) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(query_error(files.error(), "prepare_base_asset_directory"));
            auto file_statement = std::move(files.value());
            auto path_bound = file_statement.bind(1, escape_like(reference.virtual_path) + "%");
            if (!path_bound) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(query_error(path_bound.error(), "bind_base_asset_directory"));
            while (true) {
                auto file_row = file_statement.step();
                if (!file_row) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(query_error(file_row.error(), "read_base_asset_directory"));
                if (!file_row.value()) break;
                const auto source = std::string{file_statement.column_text(2)};
                if (file_statement.column_text(3) != "asset" || !source_is_enabled(file_statement.column_text(1), source, selection)) continue;
                auto expanded = reference;
                expanded.reference_type = "file";
                expanded.virtual_path = std::string{file_statement.column_text(0)};
                expanded.resolved_asset = ResolvedAsset{expanded.virtual_path, std::string{file_statement.column_text(1)},
                    source, std::string{file_statement.column_text(4)},
                    static_cast<std::uint64_t>(file_statement.column_int64(5)),
                    std::string{file_statement.column_text(3)},
                    physical_path(file_statement.column_text(6), expanded.virtual_path)};
                result.push_back(std::move(expanded));
            }
        } else {
            auto resolved = resolve_asset_in(databases, selection, reference.virtual_path, false);
            if (!resolved) return core::Result<std::vector<ContentAssetReference>, core::Error>::failure(resolved.error());
            reference.resolved_asset = std::move(resolved.value());
            result.push_back(std::move(reference));
        }
    }
    return core::Result<std::vector<ContentAssetReference>, core::Error>::success(std::move(result));
}

core::Result<std::vector<application::ContentRelationship>, core::Error>
read_relationships(DatabasePair& databases, const DefinitionLayer& layer, bool environment_database) {
    const auto sql = environment_database
        ? "SELECT relationship_type,child_type,child_id FROM content_relationships "
          "WHERE parent_type=? AND parent_id=? AND definition_source_id=? AND definition_virtual_path=? "
          "ORDER BY relationship_type,child_type,child_id"
        : "SELECT r.relationship_type,r.child_type,r.child_id FROM content_relationships r "
          "JOIN source_files f USING(source_file_id) WHERE r.parent_type=? AND r.parent_id=? "
          "AND f.source_id=? AND f.virtual_path=? ORDER BY r.relationship_type,r.child_type,r.child_id";
    Database& database = environment_database && databases.mods ? *databases.mods : databases.base;
    auto prepared = database.prepare(sql);
    if (!prepared) return core::Result<std::vector<application::ContentRelationship>, core::Error>::failure(
        query_error(prepared.error(), "prepare_content_relationships"));
    auto statement = std::move(prepared.value());
    for (const auto& [index, value] : std::array<std::pair<int, std::string_view>, 4>{
             std::pair{1, std::string_view{layer.type}}, {2, layer.id}, {3, layer.source_id}, {4, layer.path}}) {
        auto bound = statement.bind(index, value);
        if (!bound) return core::Result<std::vector<application::ContentRelationship>, core::Error>::failure(
            query_error(bound.error(), "bind_content_relationship"));
    }
    std::vector<application::ContentRelationship> result;
    while (true) {
        auto row = statement.step();
        if (!row) return core::Result<std::vector<application::ContentRelationship>, core::Error>::failure(
            query_error(row.error(), "read_content_relationships"));
        if (!row.value()) break;
        result.push_back({std::string{statement.column_text(1)}, std::string{statement.column_text(2)},
                          std::string{statement.column_text(0)}});
    }
    return core::Result<std::vector<application::ContentRelationship>, core::Error>::success(std::move(result));
}

core::Result<ContentDefinition, core::Error>
hydrate_definition(DatabasePair& databases, const ContentEnvironmentSelection& selection,
                    const DefinitionLayer& layer, std::size_t override_count,
                    bool environment_database) {
    Database& definition_db = environment_database && layer.layer == "mod" && databases.mods
        ? *databases.mods : databases.base;
    auto labels = read_definition_labels(definition_db, layer, environment_database && layer.layer == "mod");
    if (!labels) return core::Result<ContentDefinition, core::Error>::failure(labels.error());
    ContentDefinition result;
    result.type = layer.type;
    result.id = layer.id;
    result.display_name = labels.value().first;
    result.localization_key = labels.value().second;
    result.payload_json = layer.payload;
    result.provenance = {layer.source_id, layer.layer, layer.path, true};
    result.overridden_definition_count = override_count;
    auto relationships = read_relationships(databases, layer,
        environment_database && databases.mods.has_value());
    if (!relationships) return core::Result<ContentDefinition, core::Error>::failure(relationships.error());
    result.relationships = std::move(relationships.value());
    if (!result.localization_key.empty()) {
        auto localized = resolve_localization_in(databases, selection, result.localization_key,
                                                  selection.language, environment_database);
        if (!localized) return core::Result<ContentDefinition, core::Error>::failure(localized.error());
        if (localized.value()) result.localized_name = localized.value()->value;
    }
    if (result.localized_name.empty())
        result.localized_name = result.display_name.empty() ? result.id : result.display_name;
    auto references = read_asset_references(databases, selection, layer,
        environment_database && databases.mods.has_value());
    if (!references) return core::Result<ContentDefinition, core::Error>::failure(references.error());
    result.asset_references = std::move(references.value());
    return core::Result<ContentDefinition, core::Error>::success(std::move(result));
}

core::Result<std::optional<ContentDefinition>, core::Error>
find_content_in(DatabasePair& databases, const ContentEnvironmentSelection& selection,
                std::string_view type, std::string_view id, bool environment_database) {
    Database& definition_db = environment_database && databases.mods ? *databases.mods : databases.base;
    auto layers = read_definition_layers(definition_db, type, id, environment_database && databases.mods.has_value());
    if (!layers) return core::Result<std::optional<ContentDefinition>, core::Error>::failure(layers.error());
    std::vector<DefinitionLayer> visible;
    for (const auto& layer : layers.value())
        if (source_is_enabled(layer.layer, layer.source_id, selection)) visible.push_back(layer);
    if (visible.empty() && environment_database && databases.mods) {
        auto base_layers = read_definition_layers(databases.base, type, id, false);
        if (!base_layers) return core::Result<std::optional<ContentDefinition>, core::Error>::failure(base_layers.error());
        for (const auto& layer : base_layers.value())
            if (source_is_enabled(layer.layer, layer.source_id, selection)) visible.push_back(layer);
    }
    if (visible.empty()) return core::Result<std::optional<ContentDefinition>, core::Error>::success(std::nullopt);
    auto definition = hydrate_definition(databases, selection, visible.front(), visible.size() - 1,
                                         environment_database && databases.mods.has_value());
    if (!definition) return core::Result<std::optional<ContentDefinition>, core::Error>::failure(definition.error());
    return core::Result<std::optional<ContentDefinition>, core::Error>::success(std::move(definition.value()));
}

} // namespace

core::Result<std::optional<ContentDefinition>, core::Error>
SqliteContentEnvironment::find_content(std::string_view type, std::string_view id) const {
    auto opened = open_databases(config_);
    if (!opened) return core::Result<std::optional<ContentDefinition>, core::Error>::failure(opened.error());
    auto databases = std::move(opened.value());
    return find_content_in(databases, config_.selection, type, id, databases.mods.has_value());
}

core::Result<std::vector<ContentDefinition>, core::Error>
SqliteContentEnvironment::list_content(std::string_view type, std::string_view search_text) const {
    auto opened = open_databases(config_);
    if (!opened) return core::Result<std::vector<ContentDefinition>, core::Error>::failure(opened.error());
    auto databases = std::move(opened.value());
    const bool environment_database = databases.mods.has_value();
    Database& source = environment_database ? *databases.mods : databases.base;
    auto layers = read_definition_layers(source, type, std::nullopt, environment_database);
    if (!layers) return core::Result<std::vector<ContentDefinition>, core::Error>::failure(layers.error());
    std::vector<ContentDefinition> result;
    for (std::size_t i = 0; i < layers.value().size();) {
        std::size_t end = i + 1;
        while (end < layers.value().size() && layers.value()[end].id == layers.value()[i].id) ++end;
        std::vector<DefinitionLayer> visible;
        for (std::size_t j = i; j < end; ++j)
            if (source_is_enabled(layers.value()[j].layer, layers.value()[j].source_id, config_.selection))
                visible.push_back(layers.value()[j]);
        if (!visible.empty()) {
            auto definition = hydrate_definition(databases, config_.selection, visible.front(), visible.size() - 1,
                environment_database && databases.mods.has_value());
            if (!definition) return core::Result<std::vector<ContentDefinition>, core::Error>::failure(definition.error());
            auto matches = [&](std::string_view value) {
                if (search_text.empty()) return true;
                auto lower_value = std::string{value};
                auto lower_query = std::string{search_text};
                std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return lower_value.find(lower_query) != std::string::npos;
            };
            if (matches(definition.value().id) || matches(definition.value().localized_name) ||
                matches(definition.value().display_name)) result.push_back(std::move(definition.value()));
        }
        i = end;
    }
    return core::Result<std::vector<ContentDefinition>, core::Error>::success(std::move(result));
}

core::Result<std::optional<ContentBundle>, core::Error>
SqliteContentEnvironment::load_bundle(std::string_view type, std::string_view id) const {
    auto found = find_content(type, id);
    if (!found) return core::Result<std::optional<ContentBundle>, core::Error>::failure(found.error());
    if (!found.value()) return core::Result<std::optional<ContentBundle>, core::Error>::success(std::nullopt);
    ContentBundle bundle;
    bundle.definition = std::move(*found.value());
    for (const auto& reference : bundle.definition.asset_references)
        if (reference.resolved_asset) bundle.assets.push_back(*reference.resolved_asset);

    if (type == "hero_class") {
        std::set<std::string, std::less<>> related_ids;
        for (const auto& relationship : bundle.definition.relationships)
            if (relationship.type == "skill" &&
                (relationship.relationship_type == "combat_skill" || relationship.relationship_type == "camping_skill"))
                related_ids.insert(relationship.id);
        const std::string prefix = std::string{id} + ":";
        if (related_ids.empty()) {
            auto skills = list_content("skill", prefix);
            if (!skills) return core::Result<std::optional<ContentBundle>, core::Error>::failure(skills.error());
            for (const auto& skill : skills.value())
                if (skill.id.starts_with(prefix)) related_ids.insert(skill.id);
        }
        for (const auto& skill_id : related_ids) {
            auto skill = find_content("skill", skill_id);
            if (!skill) return core::Result<std::optional<ContentBundle>, core::Error>::failure(skill.error());
            if (skill.value()) bundle.related_definitions.push_back(std::move(*skill.value()));
        }
        std::sort(bundle.assets.begin(), bundle.assets.end(), [](const auto& a, const auto& b) {
            return a.virtual_path < b.virtual_path;
        });
        bundle.assets.erase(std::unique(bundle.assets.begin(), bundle.assets.end(), [](const auto& a, const auto& b) {
            return a.virtual_path == b.virtual_path;
        }), bundle.assets.end());
    }
    return core::Result<std::optional<ContentBundle>, core::Error>::success(std::move(bundle));
}

core::Result<std::optional<ResolvedLocalization>, core::Error>
SqliteContentEnvironment::resolve_localization(std::string_view key, std::string_view language) const {
    auto opened = open_databases(config_);
    if (!opened) return core::Result<std::optional<ResolvedLocalization>, core::Error>::failure(opened.error());
    auto databases = std::move(opened.value());
    return resolve_localization_in(databases, config_.selection, key, language, databases.mods.has_value());
}

core::Result<std::optional<ResolvedAsset>, core::Error>
SqliteContentEnvironment::resolve_asset(std::string_view virtual_path) const {
    auto opened = open_databases(config_);
    if (!opened) return core::Result<std::optional<ResolvedAsset>, core::Error>::failure(opened.error());
    auto databases = std::move(opened.value());
    return resolve_asset_in(databases, config_.selection, virtual_path, databases.mods.has_value());
}

core::Result<std::vector<ContentProvenance>, core::Error>
SqliteContentEnvironment::explain_provenance(std::string_view type, std::string_view id) const {
    auto opened = open_databases(config_);
    if (!opened) return core::Result<std::vector<ContentProvenance>, core::Error>::failure(opened.error());
    auto databases = std::move(opened.value());
    const bool environment_database = databases.mods.has_value();
    Database& source = environment_database ? *databases.mods : databases.base;
    auto layers = read_definition_layers(source, type, id, environment_database);
    if (!layers) return core::Result<std::vector<ContentProvenance>, core::Error>::failure(layers.error());
    std::vector<ContentProvenance> result;
    bool selected = false;
    for (const auto& layer : layers.value()) {
        const bool allowed = source_is_enabled(layer.layer, layer.source_id, config_.selection);
        result.push_back({layer.source_id, layer.layer, layer.path, allowed && !selected});
        if (allowed) selected = true;
    }
    return core::Result<std::vector<ContentProvenance>, core::Error>::success(std::move(result));
}

} // namespace ddse::infrastructure
