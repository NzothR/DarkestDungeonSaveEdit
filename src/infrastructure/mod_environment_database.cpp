#include "ddse/infrastructure/mod_environment_database.hpp"

#include "ddse/core/dson/dson_document.hpp"
#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/statement.hpp"
#include "ddse/infrastructure/sqlite/transaction.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#endif

namespace ddse::infrastructure {
namespace {

using sqlite::Statement;

std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

struct BaseFile {
    std::string source_id;
    std::string source_type;
    std::string virtual_path;
    std::string extension;
    std::string file_kind;
    std::uint64_t size_bytes{};
    std::int64_t source_file_id{};
};

struct BaseDefinition {
    std::string kind;
    std::string id;
    std::string source_id;
    std::string source_type;
    std::string virtual_path;
    std::string payload_json;
};

struct BaseLocalization {
    std::string language;
    std::string key;
    std::string value;
    std::string source_id;
    std::string source_type;
    std::string virtual_path;
};

struct BaseAssetReference {
    std::string definition_type;
    std::string content_id;
    std::string source_id;
    std::string definition_virtual_path;
    std::string asset_role;
    std::string reference_type;
    std::string virtual_path;
    std::string reference_origin;
};

struct BaseRelationship {
    std::string parent_type;
    std::string parent_id;
    std::string relationship_type;
    std::string child_type;
    std::string child_id;
    std::string source_id;
    std::string virtual_path;
};

struct LayerCandidate {
    std::string virtual_path;
    std::string layer;
    std::string source_id;
    std::string mod_id;
    std::string file_kind;
    std::int64_t source_file_id{};
    std::int64_t base_source_file_id{};
    std::int64_t priority{};
};

struct DefinitionCandidate {
    std::string kind;
    std::string id;
    std::string layer;
    std::string source_id;
    std::string mod_id;
    std::string virtual_path;
    std::string payload_json;
    std::int64_t definition_id{};
    std::int64_t source_file_id{};
    std::int64_t priority{};
};

struct LocalizationCandidate {
    std::string language;
    std::string key;
    std::string value;
    std::string layer;
    std::string source_id;
    std::string mod_id;
    std::string virtual_path;
    std::int64_t source_file_id{};
    std::int64_t priority{};
};

std::string file_key(std::string_view mod_id, std::string_view path) {
    std::string key{mod_id};
    key.push_back('\n');
    key.append(path);
    return key;
}

std::string hex_u64(std::uint64_t value) {
    char buffer[17]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool is_within_path(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    auto candidate_it = candidate.begin();
    auto root_it = root.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end()) return false;
        auto candidate_component = candidate_it->generic_string();
        auto root_component = root_it->generic_string();
#ifdef _WIN32
        candidate_component = lower_ascii(std::move(candidate_component));
        root_component = lower_ascii(std::move(root_component));
#endif
        if (candidate_component != root_component) return false;
    }
    return true;
}

core::Result<void, core::Error> replace_atomically(const std::filesystem::path& replacement,
                                                    const std::filesystem::path& target) {
#ifdef _WIN32
    const BOOL ok = MoveFileExW(replacement.c_str(), target.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) return core::Result<void, core::Error>::failure(
        {core::ErrorCode::IoError, "Atomic mod environment database replacement failed", "ModEnvironmentDatabase",
         {{"target", path_utf8(target)}, {"replacement", path_utf8(replacement)},
          {"os_error", std::to_string(GetLastError())}}});
#else
    if (std::rename(replacement.c_str(), target.c_str()) != 0)
        return core::Result<void, core::Error>::failure(
            {core::ErrorCode::IoError, "Atomic mod environment database replacement failed", "ModEnvironmentDatabase",
             {{"target", path_utf8(target)}, {"replacement", path_utf8(replacement)}, {"os_error", std::to_string(errno)}}});
#endif
    return core::Result<void, core::Error>::success();
}

core::Result<void, core::Error> bind_text(Statement& statement, int index, std::string_view value) {
    return statement.bind(index, value);
}

std::int64_t layer_priority(std::string_view source_type) {
    return source_type == "Dlc" ? 200 : 100;
}

core::Result<std::vector<BaseFile>, core::Error>
read_base_files(const std::filesystem::path& path) {
    auto opened = sqlite::ConnectionFactory{}.open(path);
    if (!opened) return core::Result<std::vector<BaseFile>, core::Error>::failure(opened.error());
    auto database = std::move(opened.value());
    auto prepared = database.prepare(
        "SELECT f.source_id,s.source_type,f.virtual_path,f.extension,f.file_kind,f.size_bytes,f.source_file_id "
        "FROM source_files f JOIN content_sources s USING(source_id) ORDER BY f.virtual_path,s.source_type,s.source_id");
    if (!prepared) return core::Result<std::vector<BaseFile>, core::Error>::failure(prepared.error());
    auto statement = std::move(prepared.value());
    std::vector<BaseFile> rows;
    while (true) {
        auto row = statement.step();
        if (!row) return core::Result<std::vector<BaseFile>, core::Error>::failure(row.error());
        if (!row.value()) break;
        rows.push_back({std::string{statement.column_text(0)}, std::string{statement.column_text(1)},
                        std::string{statement.column_text(2)}, std::string{statement.column_text(3)},
                        std::string{statement.column_text(4)}, static_cast<std::uint64_t>(statement.column_int64(5)),
                        statement.column_int64(6)});
    }
    return core::Result<std::vector<BaseFile>, core::Error>::success(std::move(rows));
}

core::Result<std::vector<BaseDefinition>, core::Error>
read_base_definitions(const std::filesystem::path& path) {
    auto opened = sqlite::ConnectionFactory{}.open(path);
    if (!opened) return core::Result<std::vector<BaseDefinition>, core::Error>::failure(opened.error());
    auto database = std::move(opened.value());
    auto prepared = database.prepare(
        "SELECT d.definition_type,d.content_id,f.source_id,s.source_type,f.virtual_path,d.payload_json "
        "FROM content_definitions d JOIN source_files f USING(source_file_id) "
        "JOIN content_sources s USING(source_id) ORDER BY d.definition_type,d.content_id,s.source_type,s.source_id,f.virtual_path");
    if (!prepared) return core::Result<std::vector<BaseDefinition>, core::Error>::failure(prepared.error());
    auto statement = std::move(prepared.value());
    std::vector<BaseDefinition> rows;
    while (true) {
        auto row = statement.step();
        if (!row) return core::Result<std::vector<BaseDefinition>, core::Error>::failure(row.error());
        if (!row.value()) break;
        rows.push_back({std::string{statement.column_text(0)}, std::string{statement.column_text(1)},
                        std::string{statement.column_text(2)}, std::string{statement.column_text(3)},
                        std::string{statement.column_text(4)}, std::string{statement.column_text(5)}});
    }
    return core::Result<std::vector<BaseDefinition>, core::Error>::success(std::move(rows));
}

core::Result<std::vector<BaseLocalization>, core::Error>
read_base_localizations(const std::filesystem::path& path) {
    auto opened = sqlite::ConnectionFactory{}.open(path);
    if (!opened) return core::Result<std::vector<BaseLocalization>, core::Error>::failure(opened.error());
    auto database = std::move(opened.value());
    auto prepared = database.prepare(
        "SELECT l.language,l.localization_key,l.localized_text,f.source_id,s.source_type,f.virtual_path "
        "FROM localization_entries l JOIN source_files f USING(source_file_id) "
        "JOIN content_sources s USING(source_id) ORDER BY l.language,l.localization_key,s.source_type,s.source_id,f.virtual_path");
    if (!prepared) return core::Result<std::vector<BaseLocalization>, core::Error>::failure(prepared.error());
    auto statement = std::move(prepared.value());
    std::vector<BaseLocalization> rows;
    while (true) {
        auto row = statement.step();
        if (!row) return core::Result<std::vector<BaseLocalization>, core::Error>::failure(row.error());
        if (!row.value()) break;
        rows.push_back({std::string{statement.column_text(0)}, std::string{statement.column_text(1)},
                        std::string{statement.column_text(2)}, std::string{statement.column_text(3)},
                        std::string{statement.column_text(4)}, std::string{statement.column_text(5)}});
    }
    return core::Result<std::vector<BaseLocalization>, core::Error>::success(std::move(rows));
}

core::Result<std::vector<BaseAssetReference>, core::Error>
read_base_asset_references(const std::filesystem::path& path) {
    auto opened = sqlite::ConnectionFactory{}.open(path);
    if (!opened) return core::Result<std::vector<BaseAssetReference>, core::Error>::failure(opened.error());
    auto database = std::move(opened.value());
    auto prepared = database.prepare(
        "SELECT r.definition_type,r.content_id,f.source_id,f.virtual_path,r.asset_role,"
        "r.reference_type,r.virtual_path,r.reference_origin "
        "FROM content_asset_references r JOIN source_files f USING(source_file_id) "
        "ORDER BY r.definition_type,r.content_id,f.source_id,f.virtual_path,r.asset_role,r.virtual_path");
    if (!prepared) return core::Result<std::vector<BaseAssetReference>, core::Error>::failure(prepared.error());
    auto statement = std::move(prepared.value());
    std::vector<BaseAssetReference> rows;
    while (true) {
        auto row = statement.step();
        if (!row) return core::Result<std::vector<BaseAssetReference>, core::Error>::failure(row.error());
        if (!row.value()) break;
        rows.push_back({std::string{statement.column_text(0)}, std::string{statement.column_text(1)},
                        std::string{statement.column_text(2)}, std::string{statement.column_text(3)},
                        std::string{statement.column_text(4)}, std::string{statement.column_text(5)},
                        std::string{statement.column_text(6)}, std::string{statement.column_text(7)}});
    }
    return core::Result<std::vector<BaseAssetReference>, core::Error>::success(std::move(rows));
}

core::Result<std::vector<BaseRelationship>, core::Error>
read_base_relationships(const std::filesystem::path& path) {
    auto opened = sqlite::ConnectionFactory{}.open(path);
    if (!opened) return core::Result<std::vector<BaseRelationship>, core::Error>::failure(opened.error());
    auto database = std::move(opened.value());
    auto prepared = database.prepare(
        "SELECT r.parent_type,r.parent_id,r.relationship_type,r.child_type,r.child_id,f.source_id,f.virtual_path "
        "FROM content_relationships r JOIN source_files f USING(source_file_id) "
        "ORDER BY r.parent_type,r.parent_id,r.relationship_type,r.child_type,r.child_id,f.source_id");
    if (!prepared) return core::Result<std::vector<BaseRelationship>, core::Error>::failure(prepared.error());
    auto statement = std::move(prepared.value());
    std::vector<BaseRelationship> rows;
    while (true) {
        auto row = statement.step();
        if (!row) return core::Result<std::vector<BaseRelationship>, core::Error>::failure(row.error());
        if (!row.value()) break;
        rows.push_back({std::string{statement.column_text(0)}, std::string{statement.column_text(1)},
                        std::string{statement.column_text(2)}, std::string{statement.column_text(3)},
                        std::string{statement.column_text(4)}, std::string{statement.column_text(5)},
                        std::string{statement.column_text(6)}});
    }
    return core::Result<std::vector<BaseRelationship>, core::Error>::success(std::move(rows));
}

} // namespace

core::Result<ModEnvironmentBuildSummary, core::Error>
ModEnvironmentDatabaseBuilder::rebuild(const std::filesystem::path& database_path,
                                       const std::filesystem::path& base_content_database_path,
                                       const application::ModEnvironmentScanResult& scan) const {
    std::error_code ec;
    const auto output_absolute_input = std::filesystem::absolute(database_path, ec);
    if (ec) return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(
        {core::ErrorCode::IoError, ec.message(), "ModEnvironmentDatabase", {{"path", path_utf8(database_path)}}});
    const auto output_absolute = std::filesystem::weakly_canonical(output_absolute_input, ec);
    if (ec) return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(
        {core::ErrorCode::IoError, ec.message(), "ModEnvironmentDatabase", {{"path", path_utf8(database_path)}}});
    auto replacement_absolute_input = output_absolute_input;
    replacement_absolute_input += ".new";
    const auto replacement_absolute = std::filesystem::weakly_canonical(replacement_absolute_input, ec);
    if (ec) return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(
        {core::ErrorCode::IoError, ec.message(), "ModEnvironmentDatabase", {{"path", path_utf8(replacement_absolute_input)}}});
    for (const auto& root : scan.protected_roots) {
        if (root.empty()) continue;
        const auto protected_absolute = std::filesystem::weakly_canonical(root, ec);
        if (ec) return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(
            {core::ErrorCode::IoError, ec.message(), "ModEnvironmentDatabase", {{"path", path_utf8(root)}}});
        if (is_within_path(output_absolute, protected_absolute) ||
            is_within_path(replacement_absolute, protected_absolute))
            return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(
                {core::ErrorCode::ValidationFailed,
                 "Environment database output and temporary replacement must be outside all mod, save, and source paths",
                 "ModEnvironmentDatabase", {{"database_path", path_utf8(output_absolute)},
                                             {"replacement_path", path_utf8(replacement_absolute)},
                                             {"protected_root", path_utf8(protected_absolute)}}});
    }

    auto base_files_result = read_base_files(base_content_database_path);
    if (!base_files_result) return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(base_files_result.error());
    auto base_definitions_result = read_base_definitions(base_content_database_path);
    if (!base_definitions_result) return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(base_definitions_result.error());
    auto base_localizations_result = read_base_localizations(base_content_database_path);
    if (!base_localizations_result) return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(base_localizations_result.error());
    auto base_asset_references_result = read_base_asset_references(base_content_database_path);
    if (!base_asset_references_result) return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(base_asset_references_result.error());
    auto base_relationships_result = read_base_relationships(base_content_database_path);
    if (!base_relationships_result) return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(base_relationships_result.error());
    const auto& base_files = base_files_result.value();
    const auto& base_definitions = base_definitions_result.value();
    const auto& base_localizations = base_localizations_result.value();
    const auto& base_asset_references = base_asset_references_result.value();
    const auto& base_relationships = base_relationships_result.value();

    const auto parent = database_path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(
        {core::ErrorCode::IoError, ec.message(), "ModEnvironmentDatabase", {{"path", path_utf8(parent)}}});
    auto replacement = database_path;
    replacement += ".new";
    std::filesystem::remove(replacement, ec);
    if (ec) return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(
        {core::ErrorCode::IoError, ec.message(), "ModEnvironmentDatabase", {{"path", path_utf8(replacement)}}});

    ModEnvironmentBuildSummary summary;
    summary.installed_mods = scan.mods.size();
    summary.enabled_mods = scan.effective_order.size();
    summary.source_files = scan.content.source_files.size();
    summary.definitions = scan.content.definitions.size();
    summary.assets = scan.content.assets.size();
    summary.asset_references = base_asset_references.size() + scan.content.asset_references.size();
    summary.relationships = base_relationships.size() + scan.content.relationships.size();
    summary.save_order_matches_manager = scan.comparison.exact_enabled_order_match;
    summary.shared_order_matches = scan.comparison.shared_enabled_order_match;
    for (const auto& mod : scan.mods) {
        if (mod.provider_id == "steam") ++summary.workshop_mods;
        else ++summary.local_mods;
    }
    std::map<std::string, const application::ModSourceRecord*, std::less<>> mods_by_id;
    for (const auto& mod : scan.mods) mods_by_id.emplace(mod.id, &mod);

    std::vector<application::ModScanDiagnostic> all_diagnostics = scan.diagnostics;
    for (const auto& diagnostic : scan.content.diagnostics)
        all_diagnostics.push_back({diagnostic.source_id, diagnostic.virtual_path, diagnostic.message});
    summary.diagnostics = all_diagnostics.size();

    auto built = [&]() -> core::Result<void, core::Error> {
        auto opened = sqlite::ConnectionFactory{}.open(replacement);
        if (!opened) return core::Result<void, core::Error>::failure(opened.error());
        auto database = std::move(opened.value());
        for (const auto pragma : {"PRAGMA foreign_keys=ON", "PRAGMA journal_mode=DELETE", "PRAGMA synchronous=FULL"}) {
            auto applied = database.execute(pragma);
            if (!applied) return core::Result<void, core::Error>::failure(applied.error());
        }
        auto transaction_result = sqlite::Transaction::begin(database);
        if (!transaction_result) return core::Result<void, core::Error>::failure(transaction_result.error());
        auto transaction = std::move(transaction_result.value());
        constexpr std::string_view schema = R"SQL(
CREATE TABLE environment_info (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE mod_sources (
 mod_id TEXT PRIMARY KEY, provider_id TEXT NOT NULL, external_id TEXT NOT NULL, mod_guid TEXT NOT NULL,
 display_name TEXT NOT NULL, version TEXT NOT NULL, root_path TEXT NOT NULL, enabled INTEGER NOT NULL,
 active_order INTEGER, order_source TEXT NOT NULL
);
CREATE TABLE mod_order_entries (
 order_source TEXT NOT NULL, position INTEGER NOT NULL, active_order INTEGER, identity TEXT NOT NULL,
 provider_id TEXT NOT NULL, external_id TEXT NOT NULL, mod_guid TEXT NOT NULL, display_name TEXT NOT NULL,
 version TEXT NOT NULL, enabled INTEGER NOT NULL, matched_mod_id TEXT,
 PRIMARY KEY(order_source,position)
);
CREATE TABLE source_files (
 source_file_id INTEGER PRIMARY KEY, mod_id TEXT NOT NULL REFERENCES mod_sources(mod_id),
 virtual_path TEXT NOT NULL, extension TEXT NOT NULL, size_bytes INTEGER NOT NULL,
 content_fingerprint TEXT NOT NULL, file_kind TEXT NOT NULL,
 UNIQUE(mod_id,virtual_path)
);
CREATE TABLE content_definitions (
 definition_id INTEGER PRIMARY KEY, definition_type TEXT NOT NULL, content_id TEXT NOT NULL,
 source_file_id INTEGER NOT NULL REFERENCES source_files(source_file_id),
 display_name TEXT NOT NULL, localization_key TEXT NOT NULL, payload_json TEXT NOT NULL
);
CREATE TABLE localization_entries (
 language TEXT NOT NULL, localization_key TEXT NOT NULL, localized_text TEXT NOT NULL,
 source_file_id INTEGER NOT NULL REFERENCES source_files(source_file_id),
 PRIMARY KEY(language,localization_key,source_file_id)
);
CREATE TABLE assets (
 asset_id INTEGER PRIMARY KEY, source_file_id INTEGER NOT NULL UNIQUE REFERENCES source_files(source_file_id),
 virtual_path TEXT NOT NULL, extension TEXT NOT NULL, size_bytes INTEGER NOT NULL
);
CREATE TABLE content_asset_references (
 asset_reference_id INTEGER PRIMARY KEY, definition_type TEXT NOT NULL, content_id TEXT NOT NULL,
 definition_source_id TEXT NOT NULL, definition_virtual_path TEXT NOT NULL, asset_role TEXT NOT NULL,
 reference_type TEXT NOT NULL, virtual_path TEXT NOT NULL, reference_origin TEXT NOT NULL,
 UNIQUE(definition_type,content_id,definition_source_id,definition_virtual_path,asset_role,reference_type,virtual_path)
);
CREATE TABLE content_relationships (
 relationship_id INTEGER PRIMARY KEY, parent_type TEXT NOT NULL, parent_id TEXT NOT NULL,
 relationship_type TEXT NOT NULL, child_type TEXT NOT NULL, child_id TEXT NOT NULL,
 definition_source_id TEXT NOT NULL, definition_virtual_path TEXT NOT NULL,
 UNIQUE(parent_type,parent_id,relationship_type,child_type,child_id,definition_source_id,definition_virtual_path)
);
CREATE TABLE mod_content_hash_index (
 hash_value INTEGER NOT NULL, definition_type TEXT NOT NULL, content_id TEXT NOT NULL,
 mod_id TEXT NOT NULL REFERENCES mod_sources(mod_id), source_file_id INTEGER NOT NULL REFERENCES source_files(source_file_id),
 PRIMARY KEY(hash_value,definition_type,content_id,mod_id,source_file_id)
);
CREATE TABLE mod_diagnostics (
 diagnostic_id INTEGER PRIMARY KEY, mod_id TEXT NOT NULL, virtual_path TEXT NOT NULL, message TEXT NOT NULL
);
CREATE TABLE mod_order_comparison (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE effective_vfs_chain (
 virtual_path TEXT NOT NULL, chain_rank INTEGER NOT NULL, layer_type TEXT NOT NULL,
 source_id TEXT NOT NULL, mod_id TEXT, source_file_id INTEGER, base_source_file_id INTEGER,
 file_kind TEXT NOT NULL, priority INTEGER NOT NULL,
 PRIMARY KEY(virtual_path,chain_rank)
);
CREATE TABLE effective_vfs (
 virtual_path TEXT PRIMARY KEY, layer_type TEXT NOT NULL, winner_source_id TEXT NOT NULL,
 winner_mod_id TEXT, source_file_id INTEGER, base_source_file_id INTEGER,
 file_kind TEXT NOT NULL, override_count INTEGER NOT NULL
);
CREATE TABLE effective_definition_chain (
 definition_type TEXT NOT NULL, content_id TEXT NOT NULL, chain_rank INTEGER NOT NULL,
 layer_type TEXT NOT NULL, source_id TEXT NOT NULL, mod_id TEXT, virtual_path TEXT NOT NULL,
 definition_id INTEGER, payload_json TEXT NOT NULL, priority INTEGER NOT NULL,
 PRIMARY KEY(definition_type,content_id,chain_rank)
);
CREATE TABLE effective_definitions (
 definition_type TEXT NOT NULL, content_id TEXT NOT NULL, layer_type TEXT NOT NULL,
 winner_source_id TEXT NOT NULL, winner_mod_id TEXT, virtual_path TEXT NOT NULL,
 definition_id INTEGER, payload_json TEXT NOT NULL, override_count INTEGER NOT NULL,
 PRIMARY KEY(definition_type,content_id)
);
CREATE TABLE effective_localization_chain (
 language TEXT NOT NULL, localization_key TEXT NOT NULL, chain_rank INTEGER NOT NULL,
 layer_type TEXT NOT NULL, source_id TEXT NOT NULL, mod_id TEXT, virtual_path TEXT NOT NULL,
 source_file_id INTEGER, localized_text TEXT NOT NULL, priority INTEGER NOT NULL,
 PRIMARY KEY(language,localization_key,chain_rank)
);
CREATE TABLE effective_localizations (
 language TEXT NOT NULL, localization_key TEXT NOT NULL, layer_type TEXT NOT NULL,
 winner_source_id TEXT NOT NULL, winner_mod_id TEXT, virtual_path TEXT NOT NULL,
 source_file_id INTEGER, localized_text TEXT NOT NULL, override_count INTEGER NOT NULL,
 PRIMARY KEY(language,localization_key)
);
CREATE TABLE effective_assets (
 virtual_path TEXT PRIMARY KEY, layer_type TEXT NOT NULL, winner_source_id TEXT NOT NULL,
 winner_mod_id TEXT, source_file_id INTEGER, base_source_file_id INTEGER,
 extension TEXT NOT NULL, size_bytes INTEGER NOT NULL
);
CREATE INDEX idx_mod_order_enabled ON mod_sources(enabled,active_order);
CREATE INDEX idx_mod_files_path ON source_files(virtual_path);
CREATE INDEX idx_mod_definitions_id ON content_definitions(definition_type,content_id);
CREATE INDEX idx_mod_localization_key ON localization_entries(language,localization_key);
CREATE INDEX idx_mod_hash_value ON mod_content_hash_index(hash_value);
CREATE INDEX idx_mod_asset_references_entity ON content_asset_references(definition_type,content_id,definition_source_id);
CREATE INDEX idx_mod_relationships_parent ON content_relationships(parent_type,parent_id,relationship_type);
CREATE INDEX idx_effective_vfs_winner ON effective_vfs(winner_mod_id,virtual_path);
)SQL";
        auto created = database.execute(schema);
        if (!created) return core::Result<void, core::Error>::failure(created.error());

        auto insert_rows = [&](std::string_view sql, std::size_t count, auto&& bind_row) -> core::Result<void, core::Error> {
            auto prepared = database.prepare(sql);
            if (!prepared) return core::Result<void, core::Error>::failure(prepared.error());
            auto statement = std::move(prepared.value());
            for (std::size_t i = 0; i < count; ++i) {
                auto bound = bind_row(statement, i);
                if (!bound) return bound;
                auto step = statement.step();
                if (!step) return core::Result<void, core::Error>::failure(step.error());
                if (step.value()) return core::Result<void, core::Error>::failure(
                    {core::ErrorCode::DatabaseError, "Insert unexpectedly returned a row", "ModEnvironmentDatabase", {}});
                auto reset = statement.reset();
                if (!reset) return reset;
            }
            return core::Result<void, core::Error>::success();
        };

        auto inserted = insert_rows("INSERT INTO environment_info(key,value) VALUES(?,?)", 5,
            [&](Statement& s, std::size_t i) {
                static const std::vector<std::pair<std::string_view, std::string>> values{
                    {"schema_version", "2"}, {"scanner_version", "stage6"},
                    {"effective_order_source", scan.effective_order_source},
                    {"installed_mods", std::to_string(summary.installed_mods)},
                    {"enabled_mods", std::to_string(summary.enabled_mods)}};
                auto r = bind_text(s, 1, values[i].first); if (!r) return r;
                return bind_text(s, 2, values[i].second);
            });
        if (!inserted) return inserted;

        inserted = insert_rows("INSERT INTO mod_sources(mod_id,provider_id,external_id,mod_guid,display_name,version,root_path,enabled,active_order,order_source) VALUES(?,?,?,?,?,?,?,?,?,?)",
            scan.mods.size(), [&](Statement& s, std::size_t i) {
                const auto& row = scan.mods[i];
                auto r = bind_text(s, 1, row.id); if (!r) return r;
                r = bind_text(s, 2, row.provider_id); if (!r) return r;
                r = bind_text(s, 3, row.external_id); if (!r) return r;
                r = bind_text(s, 4, row.mod_guid); if (!r) return r;
                r = bind_text(s, 5, row.display_name); if (!r) return r;
                r = bind_text(s, 6, row.version); if (!r) return r;
                r = bind_text(s, 7, path_utf8(row.root_path)); if (!r) return r;
                r = s.bind(8, row.enabled ? 1 : 0); if (!r) return r;
                if (row.active_order) r = s.bind(9, static_cast<std::int64_t>(*row.active_order));
                else r = s.bind_null(9);
                if (!r) return r;
                return bind_text(s, 10, row.order_source);
            });
        if (!inserted) return inserted;

        std::vector<application::ModOrderEntry> order_rows = scan.save_order;
        order_rows.insert(order_rows.end(), scan.manager_order.begin(), scan.manager_order.end());
        inserted = insert_rows("INSERT INTO mod_order_entries(order_source,position,active_order,identity,provider_id,external_id,mod_guid,display_name,version,enabled,matched_mod_id) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            order_rows.size(), [&](Statement& s, std::size_t i) {
                const auto& row = order_rows[i];
                auto r = bind_text(s, 1, application::to_string(row.source)); if (!r) return r;
                r = s.bind(2, static_cast<std::int64_t>(row.position)); if (!r) return r;
                if (row.active_order) r = s.bind(3, static_cast<std::int64_t>(*row.active_order));
                else r = s.bind_null(3);
                if (!r) return r;
                r = bind_text(s, 4, row.identity); if (!r) return r;
                r = bind_text(s, 5, row.provider_id); if (!r) return r;
                r = bind_text(s, 6, row.external_id); if (!r) return r;
                r = bind_text(s, 7, row.mod_guid); if (!r) return r;
                r = bind_text(s, 8, row.name); if (!r) return r;
                r = bind_text(s, 9, row.version); if (!r) return r;
                r = s.bind(10, row.enabled ? 1 : 0); if (!r) return r;
                if (row.matched_mod_id.empty()) return s.bind_null(11);
                return bind_text(s, 11, row.matched_mod_id);
            });
        if (!inserted) return inserted;

        std::map<std::string, std::int64_t, std::less<>> source_file_ids;
        std::int64_t next_file_id = 1;
        inserted = insert_rows("INSERT INTO source_files(source_file_id,mod_id,virtual_path,extension,size_bytes,content_fingerprint,file_kind) VALUES(?,?,?,?,?,?,?)",
            scan.content.source_files.size(), [&](Statement& s, std::size_t i) {
                const auto& row = scan.content.source_files[i];
                const auto id = next_file_id++;
                source_file_ids.emplace(file_key(row.source_id, row.virtual_path), id);
                auto r = s.bind(1, id); if (!r) return r;
                r = bind_text(s, 2, row.source_id); if (!r) return r;
                r = bind_text(s, 3, row.virtual_path); if (!r) return r;
                r = bind_text(s, 4, row.extension); if (!r) return r;
                r = s.bind(5, static_cast<std::int64_t>(row.size_bytes)); if (!r) return r;
                r = bind_text(s, 6, hex_u64(row.content_fingerprint)); if (!r) return r;
                return bind_text(s, 7, row.is_asset ? "asset" : "content");
            });
        if (!inserted) return inserted;

        std::vector<std::int64_t> definition_ids(scan.content.definitions.size());
        std::int64_t next_definition_id = 1;
        inserted = insert_rows("INSERT INTO content_definitions(definition_id,definition_type,content_id,source_file_id,display_name,localization_key,payload_json) VALUES(?,?,?,?,?,?,?)",
            scan.content.definitions.size(), [&](Statement& s, std::size_t i) {
                const auto& row = scan.content.definitions[i];
                const auto file = source_file_ids.find(file_key(row.source_id, row.virtual_path));
                if (file == source_file_ids.end()) return core::Result<void, core::Error>::failure(
                    {core::ErrorCode::ValidationFailed, "Definition references an unregistered mod source file",
                     "ModEnvironmentDatabase", {{"mod", row.source_id}, {"path", row.virtual_path}}});
                const auto id = next_definition_id++;
                definition_ids[i] = id;
                auto r = s.bind(1, id); if (!r) return r;
                r = bind_text(s, 2, row.kind); if (!r) return r;
                r = bind_text(s, 3, row.id); if (!r) return r;
                r = s.bind(4, file->second); if (!r) return r;
                r = bind_text(s, 5, row.display_name); if (!r) return r;
                r = bind_text(s, 6, row.localization_key); if (!r) return r;
                return bind_text(s, 7, row.payload_json);
            });
        if (!inserted) return inserted;

        inserted = insert_rows("INSERT OR IGNORE INTO mod_content_hash_index(hash_value,definition_type,content_id,mod_id,source_file_id) VALUES(?,?,?,?,?)",
            scan.content.definitions.size(), [&](Statement& s, std::size_t i) {
                const auto& row = scan.content.definitions[i];
                const auto file = source_file_ids.find(file_key(row.source_id, row.virtual_path));
                if (file == source_file_ids.end()) return core::Result<void, core::Error>::failure(
                    {core::ErrorCode::ValidationFailed, "Hash index references an unregistered source file",
                     "ModEnvironmentDatabase", {{"mod", row.source_id}, {"path", row.virtual_path}}});
                auto r = s.bind(1, static_cast<std::int64_t>(core::dson::string_hash(row.id))); if (!r) return r;
                r = bind_text(s, 2, row.kind); if (!r) return r;
                r = bind_text(s, 3, row.id); if (!r) return r;
                r = bind_text(s, 4, row.source_id); if (!r) return r;
                return s.bind(5, file->second);
            });
        if (!inserted) return inserted;

        inserted = insert_rows("INSERT OR REPLACE INTO localization_entries(language,localization_key,localized_text,source_file_id) VALUES(?,?,?,?)",
            scan.content.localizations.size(), [&](Statement& s, std::size_t i) {
                const auto& row = scan.content.localizations[i];
                const auto file = source_file_ids.find(file_key(row.source_id, row.virtual_path));
                if (file == source_file_ids.end()) return core::Result<void, core::Error>::failure(
                    {core::ErrorCode::ValidationFailed, "Localization references an unregistered mod source file",
                     "ModEnvironmentDatabase", {{"mod", row.source_id}, {"path", row.virtual_path}}});
                auto r = bind_text(s, 1, row.language); if (!r) return r;
                r = bind_text(s, 2, row.key); if (!r) return r;
                r = bind_text(s, 3, row.value); if (!r) return r;
                return s.bind(4, file->second);
            });
        if (!inserted) return inserted;

        auto localization_count_result = database.prepare("SELECT count(*) FROM localization_entries");
        if (!localization_count_result)
            return core::Result<void, core::Error>::failure(localization_count_result.error());
        auto localization_count = std::move(localization_count_result.value());
        auto localization_count_row = localization_count.step();
        if (!localization_count_row)
            return core::Result<void, core::Error>::failure(localization_count_row.error());
        if (!localization_count_row.value())
            return core::Result<void, core::Error>::failure(
                {core::ErrorCode::DatabaseError, "Localization count query returned no row", "ModEnvironmentDatabase", {}});
        summary.localization_entries = static_cast<std::size_t>(localization_count.column_int64(0));

        inserted = insert_rows("INSERT INTO assets(source_file_id,virtual_path,extension,size_bytes) VALUES(?,?,?,?)",
            scan.content.assets.size(), [&](Statement& s, std::size_t i) {
                const auto& row = scan.content.assets[i];
                const auto file = source_file_ids.find(file_key(row.source_id, row.virtual_path));
                if (file == source_file_ids.end()) return core::Result<void, core::Error>::failure(
                    {core::ErrorCode::ValidationFailed, "Asset references an unregistered mod source file",
                     "ModEnvironmentDatabase", {{"mod", row.source_id}, {"path", row.virtual_path}}});
                auto r = s.bind(1, file->second); if (!r) return r;
                r = bind_text(s, 2, row.virtual_path); if (!r) return r;
                r = bind_text(s, 3, row.extension); if (!r) return r;
                return s.bind(4, static_cast<std::int64_t>(row.size_bytes));
            });
        if (!inserted) return inserted;

        std::vector<BaseAssetReference> effective_asset_references = base_asset_references;
        for (const auto& row : scan.content.asset_references) {
            effective_asset_references.push_back({row.definition_type, row.content_id, row.source_id,
                row.definition_virtual_path, row.asset_role, row.reference_type, row.virtual_path,
                row.reference_origin});
        }
        inserted = insert_rows("INSERT INTO content_asset_references(definition_type,content_id,definition_source_id,definition_virtual_path,asset_role,reference_type,virtual_path,reference_origin) VALUES(?,?,?,?,?,?,?,?)",
            effective_asset_references.size(), [&](Statement& s, std::size_t i) {
                const auto& row = effective_asset_references[i];
                auto r = bind_text(s, 1, row.definition_type); if (!r) return r;
                r = bind_text(s, 2, row.content_id); if (!r) return r;
                r = bind_text(s, 3, row.source_id); if (!r) return r;
                r = bind_text(s, 4, row.definition_virtual_path); if (!r) return r;
                r = bind_text(s, 5, row.asset_role); if (!r) return r;
                r = bind_text(s, 6, row.reference_type); if (!r) return r;
                r = bind_text(s, 7, row.virtual_path); if (!r) return r;
                return bind_text(s, 8, row.reference_origin);
            });
        if (!inserted) return inserted;

        std::vector<BaseRelationship> effective_relationships = base_relationships;
        for (const auto& row : scan.content.relationships) {
            effective_relationships.push_back({row.parent_type, row.parent_id, row.relationship_type,
                row.child_type, row.child_id, row.source_id, row.virtual_path});
        }
        inserted = insert_rows("INSERT INTO content_relationships(parent_type,parent_id,relationship_type,child_type,child_id,definition_source_id,definition_virtual_path) VALUES(?,?,?,?,?,?,?)",
            effective_relationships.size(), [&](Statement& s, std::size_t i) {
                const auto& row = effective_relationships[i];
                auto r = bind_text(s, 1, row.parent_type); if (!r) return r;
                r = bind_text(s, 2, row.parent_id); if (!r) return r;
                r = bind_text(s, 3, row.relationship_type); if (!r) return r;
                r = bind_text(s, 4, row.child_type); if (!r) return r;
                r = bind_text(s, 5, row.child_id); if (!r) return r;
                r = bind_text(s, 6, row.source_id); if (!r) return r;
                return bind_text(s, 7, row.virtual_path);
            });
        if (!inserted) return inserted;

        inserted = insert_rows("INSERT INTO mod_diagnostics(mod_id,virtual_path,message) VALUES(?,?,?)",
            all_diagnostics.size(), [&](Statement& s, std::size_t i) {
                const auto& row = all_diagnostics[i];
                auto r = bind_text(s, 1, row.mod_id); if (!r) return r;
                r = bind_text(s, 2, row.virtual_path); if (!r) return r;
                return bind_text(s, 3, row.message);
            });
        if (!inserted) return inserted;

        const std::vector<std::pair<std::string_view, std::string>> comparisons{
            {"save_available", scan.comparison.save_available ? "1" : "0"},
            {"manager_export_available", scan.comparison.manager_export_available ? "1" : "0"},
            {"exact_enabled_order_match", scan.comparison.exact_enabled_order_match ? "1" : "0"},
            {"shared_enabled_order_match", scan.comparison.shared_enabled_order_match ? "1" : "0"},
            {"save_only_count", std::to_string(scan.comparison.only_in_save.size())},
            {"manager_only_count", std::to_string(scan.comparison.only_in_manager_export.size())}};
        inserted = insert_rows("INSERT INTO mod_order_comparison(key,value) VALUES(?,?)", comparisons.size(),
            [&](Statement& s, std::size_t i) {
                auto r = bind_text(s, 1, comparisons[i].first); if (!r) return r;
                return bind_text(s, 2, comparisons[i].second);
            });
        if (!inserted) return inserted;

        std::vector<LayerCandidate> vfs_candidates;
        for (const auto& row : base_files) {
            vfs_candidates.push_back({row.virtual_path, "base", row.source_id, {}, row.file_kind, 0,
                                      row.source_file_id, layer_priority(row.source_type)});
        }
        const auto active_count = scan.effective_order.size();
        for (const auto& row : scan.content.source_files) {
            const auto mod = mods_by_id.find(row.source_id);
            if (mod == mods_by_id.end() || !mod->second->enabled || !mod->second->active_order) continue;
            const auto source_file_id = source_file_ids.at(file_key(row.source_id, row.virtual_path));
            const auto priority = static_cast<std::int64_t>(1000000 + active_count - *mod->second->active_order);
            vfs_candidates.push_back({row.virtual_path, "mod", mod->second->id, mod->second->id,
                                      row.is_asset ? "asset" : "content", source_file_id, 0, priority});
        }
        std::sort(vfs_candidates.begin(), vfs_candidates.end(), [](const auto& a, const auto& b) {
            if (a.virtual_path != b.virtual_path) return a.virtual_path < b.virtual_path;
            if (a.priority != b.priority) return a.priority > b.priority;
            return std::tie(a.layer, a.source_id, a.mod_id, a.base_source_file_id, a.source_file_id) <
                   std::tie(b.layer, b.source_id, b.mod_id, b.base_source_file_id, b.source_file_id);
        });
        std::vector<LayerCandidate> effective_files;
        std::vector<std::pair<std::string, std::vector<LayerCandidate>>> file_chains;
        for (std::size_t i = 0; i < vfs_candidates.size();) {
            std::size_t end = i + 1;
            while (end < vfs_candidates.size() && vfs_candidates[end].virtual_path == vfs_candidates[i].virtual_path) ++end;
            std::vector<LayerCandidate> chain(vfs_candidates.begin() + static_cast<std::ptrdiff_t>(i),
                                               vfs_candidates.begin() + static_cast<std::ptrdiff_t>(end));
            effective_files.push_back(chain.front());
            if (chain.size() > 1) ++summary.overridden_paths;
            file_chains.emplace_back(chain.front().virtual_path, std::move(chain));
            i = end;
        }
        summary.effective_paths = effective_files.size();
        std::size_t chain_count = 0;
        for (const auto& [path, rows] : file_chains) { (void)path; chain_count += rows.size(); }
        std::vector<std::tuple<std::string_view, std::size_t, const LayerCandidate*>> flattened_file_chain;
        flattened_file_chain.reserve(chain_count);
        for (const auto& [path, rows] : file_chains)
            for (std::size_t rank = 0; rank < rows.size(); ++rank)
                flattened_file_chain.emplace_back(path, rank, &rows[rank]);
        inserted = insert_rows("INSERT INTO effective_vfs_chain(virtual_path,chain_rank,layer_type,source_id,mod_id,source_file_id,base_source_file_id,file_kind,priority) VALUES(?,?,?,?,?,?,?,?,?)",
            flattened_file_chain.size(), [&](Statement& s, std::size_t i) {
                const auto& [path, rank, row_pointer] = flattened_file_chain[i];
                const auto& row = *row_pointer;
                auto r = bind_text(s, 1, path); if (!r) return r;
                r = s.bind(2, static_cast<std::int64_t>(rank)); if (!r) return r;
                r = bind_text(s, 3, row.layer); if (!r) return r;
                r = bind_text(s, 4, row.source_id); if (!r) return r;
                if (row.mod_id.empty()) r = s.bind_null(5); else r = bind_text(s, 5, row.mod_id);
                if (!r) return r;
                if (row.source_file_id == 0) r = s.bind_null(6); else r = s.bind(6, row.source_file_id);
                if (!r) return r;
                if (row.base_source_file_id == 0) r = s.bind_null(7); else r = s.bind(7, row.base_source_file_id);
                if (!r) return r;
                r = bind_text(s, 8, row.file_kind); if (!r) return r;
                return s.bind(9, row.priority);
            });
        if (!inserted) return inserted;
        inserted = insert_rows("INSERT INTO effective_vfs(virtual_path,layer_type,winner_source_id,winner_mod_id,source_file_id,base_source_file_id,file_kind,override_count) VALUES(?,?,?,?,?,?,?,?)",
            effective_files.size(), [&](Statement& s, std::size_t i) {
                const auto& row = effective_files[i];
                auto r = bind_text(s, 1, row.virtual_path); if (!r) return r;
                r = bind_text(s, 2, row.layer); if (!r) return r;
                r = bind_text(s, 3, row.source_id); if (!r) return r;
                if (row.mod_id.empty()) r = s.bind_null(4); else r = bind_text(s, 4, row.mod_id);
                if (!r) return r;
                if (row.source_file_id == 0) r = s.bind_null(5); else r = s.bind(5, row.source_file_id);
                if (!r) return r;
                if (row.base_source_file_id == 0) r = s.bind_null(6); else r = s.bind(6, row.base_source_file_id);
                if (!r) return r;
                r = bind_text(s, 7, row.file_kind); if (!r) return r;
                return s.bind(8, static_cast<std::int64_t>(file_chains[i].second.size() - 1));
            });
        if (!inserted) return inserted;

        std::vector<DefinitionCandidate> definitions;
        definitions.reserve(base_definitions.size() + scan.content.definitions.size());
        for (const auto& row : base_definitions)
            definitions.push_back({row.kind, row.id, "base", row.source_id, {}, row.virtual_path,
                                   row.payload_json, 0, 0, layer_priority(row.source_type)});
        for (std::size_t i = 0; i < scan.content.definitions.size(); ++i) {
            const auto& row = scan.content.definitions[i];
            const auto mod = mods_by_id.find(row.source_id);
            if (mod == mods_by_id.end() || !mod->second->enabled || !mod->second->active_order) continue;
            definitions.push_back({row.kind, row.id, "mod", row.source_id, mod->second->id, row.virtual_path,
                                   row.payload_json, definition_ids[i],
                                   source_file_ids.at(file_key(row.source_id, row.virtual_path)),
                                   static_cast<std::int64_t>(1000000 + active_count - *mod->second->active_order)});
        }
        std::sort(definitions.begin(), definitions.end(), [](const auto& a, const auto& b) {
            if (std::tie(a.kind, a.id) != std::tie(b.kind, b.id)) return std::tie(a.kind, a.id) < std::tie(b.kind, b.id);
            if (a.priority != b.priority) return a.priority > b.priority;
            return std::tie(a.layer, a.source_id, a.virtual_path) < std::tie(b.layer, b.source_id, b.virtual_path);
        });
        std::vector<std::pair<std::string, std::vector<DefinitionCandidate>>> definition_chains;
        for (std::size_t i = 0; i < definitions.size();) {
            std::size_t end = i + 1;
            while (end < definitions.size() && definitions[end].kind == definitions[i].kind && definitions[end].id == definitions[i].id) ++end;
            definition_chains.emplace_back(definitions[i].kind + "\n" + definitions[i].id,
                std::vector<DefinitionCandidate>(definitions.begin() + static_cast<std::ptrdiff_t>(i),
                                                 definitions.begin() + static_cast<std::ptrdiff_t>(end)));
            i = end;
        }
        std::size_t def_chain_count = 0;
        for (const auto& group : definition_chains) def_chain_count += group.second.size();
        std::vector<std::tuple<std::string_view, std::string_view, std::size_t, const DefinitionCandidate*>> flattened_definition_chain;
        flattened_definition_chain.reserve(def_chain_count);
        for (const auto& group : definition_chains) {
            const auto newline = group.first.find('\n');
            const std::string_view kind{group.first.data(), newline};
            const std::string_view id{group.first.data() + newline + 1, group.first.size() - newline - 1};
            for (std::size_t rank = 0; rank < group.second.size(); ++rank)
                flattened_definition_chain.emplace_back(kind, id, rank, &group.second[rank]);
        }
        inserted = insert_rows("INSERT INTO effective_definition_chain(definition_type,content_id,chain_rank,layer_type,source_id,mod_id,virtual_path,definition_id,payload_json,priority) VALUES(?,?,?,?,?,?,?,?,?,?)",
            flattened_definition_chain.size(), [&](Statement& s, std::size_t i) {
                const auto& [kind, id, rank, row_pointer] = flattened_definition_chain[i];
                const auto& row = *row_pointer;
                auto r = bind_text(s, 1, kind); if (!r) return r;
                r = bind_text(s, 2, id); if (!r) return r;
                r = s.bind(3, static_cast<std::int64_t>(rank)); if (!r) return r;
                r = bind_text(s, 4, row.layer); if (!r) return r;
                r = bind_text(s, 5, row.source_id); if (!r) return r;
                if (row.mod_id.empty()) r = s.bind_null(6); else r = bind_text(s, 6, row.mod_id);
                if (!r) return r;
                r = bind_text(s, 7, row.virtual_path); if (!r) return r;
                if (row.definition_id == 0) r = s.bind_null(8); else r = s.bind(8, row.definition_id);
                if (!r) return r;
                r = bind_text(s, 9, row.payload_json); if (!r) return r;
                return s.bind(10, row.priority);
            });
        if (!inserted) return inserted;
        inserted = insert_rows("INSERT INTO effective_definitions(definition_type,content_id,layer_type,winner_source_id,winner_mod_id,virtual_path,definition_id,payload_json,override_count) VALUES(?,?,?,?,?,?,?,?,?)",
            definition_chains.size(), [&](Statement& s, std::size_t i) {
                const auto& group = definition_chains[i];
                const auto& row = group.second.front();
                auto r = bind_text(s, 1, row.kind); if (!r) return r;
                r = bind_text(s, 2, row.id); if (!r) return r;
                r = bind_text(s, 3, row.layer); if (!r) return r;
                r = bind_text(s, 4, row.source_id); if (!r) return r;
                if (row.mod_id.empty()) r = s.bind_null(5); else r = bind_text(s, 5, row.mod_id);
                if (!r) return r;
                r = bind_text(s, 6, row.virtual_path); if (!r) return r;
                if (row.definition_id == 0) r = s.bind_null(7); else r = s.bind(7, row.definition_id);
                if (!r) return r;
                r = bind_text(s, 8, row.payload_json); if (!r) return r;
                return s.bind(9, static_cast<std::int64_t>(group.second.size() - 1));
            });
        if (!inserted) return inserted;

        std::vector<LocalizationCandidate> localizations;
        localizations.reserve(base_localizations.size() + scan.content.localizations.size());
        for (const auto& row : base_localizations)
            localizations.push_back({row.language, row.key, row.value, "base", row.source_id, {}, row.virtual_path,
                                     0, layer_priority(row.source_type)});
        for (const auto& row : scan.content.localizations) {
            const auto mod = mods_by_id.find(row.source_id);
            if (mod == mods_by_id.end() || !mod->second->enabled || !mod->second->active_order) continue;
            localizations.push_back({row.language, row.key, row.value, "mod", row.source_id, mod->second->id, row.virtual_path,
                                     source_file_ids.at(file_key(row.source_id, row.virtual_path)),
                                     static_cast<std::int64_t>(1000000 + active_count - *mod->second->active_order)});
        }
        std::sort(localizations.begin(), localizations.end(), [](const auto& a, const auto& b) {
            if (std::tie(a.language, a.key) != std::tie(b.language, b.key)) return std::tie(a.language, a.key) < std::tie(b.language, b.key);
            if (a.priority != b.priority) return a.priority > b.priority;
            return std::tie(a.layer, a.source_id, a.virtual_path) < std::tie(b.layer, b.source_id, b.virtual_path);
        });
        std::vector<std::pair<std::string, std::vector<LocalizationCandidate>>> localization_chains;
        for (std::size_t i = 0; i < localizations.size();) {
            std::size_t end = i + 1;
            while (end < localizations.size() && localizations[end].language == localizations[i].language &&
                   localizations[end].key == localizations[i].key) ++end;
            localization_chains.emplace_back(localizations[i].language + "\n" + localizations[i].key,
                std::vector<LocalizationCandidate>(localizations.begin() + static_cast<std::ptrdiff_t>(i),
                                                   localizations.begin() + static_cast<std::ptrdiff_t>(end)));
            i = end;
        }
        std::size_t loc_chain_count = 0;
        for (const auto& group : localization_chains) loc_chain_count += group.second.size();
        std::vector<std::tuple<std::string_view, std::string_view, std::size_t, const LocalizationCandidate*>> flattened_localization_chain;
        flattened_localization_chain.reserve(loc_chain_count);
        for (const auto& group : localization_chains) {
            const auto newline = group.first.find('\n');
            const std::string_view language{group.first.data(), newline};
            const std::string_view key{group.first.data() + newline + 1, group.first.size() - newline - 1};
            for (std::size_t rank = 0; rank < group.second.size(); ++rank)
                flattened_localization_chain.emplace_back(language, key, rank, &group.second[rank]);
        }
        inserted = insert_rows("INSERT INTO effective_localization_chain(language,localization_key,chain_rank,layer_type,source_id,mod_id,virtual_path,source_file_id,localized_text,priority) VALUES(?,?,?,?,?,?,?,?,?,?)",
            flattened_localization_chain.size(), [&](Statement& s, std::size_t i) {
                const auto& [language, key, rank, row_pointer] = flattened_localization_chain[i];
                const auto& row = *row_pointer;
                auto r = bind_text(s, 1, language); if (!r) return r;
                r = bind_text(s, 2, key); if (!r) return r;
                r = s.bind(3, static_cast<std::int64_t>(rank)); if (!r) return r;
                r = bind_text(s, 4, row.layer); if (!r) return r;
                r = bind_text(s, 5, row.source_id); if (!r) return r;
                if (row.mod_id.empty()) r = s.bind_null(6); else r = bind_text(s, 6, row.mod_id);
                if (!r) return r;
                r = bind_text(s, 7, row.virtual_path); if (!r) return r;
                if (row.source_file_id == 0) r = s.bind_null(8); else r = s.bind(8, row.source_file_id);
                if (!r) return r;
                r = bind_text(s, 9, row.value); if (!r) return r;
                return s.bind(10, row.priority);
            });
        if (!inserted) return inserted;
        inserted = insert_rows("INSERT INTO effective_localizations(language,localization_key,layer_type,winner_source_id,winner_mod_id,virtual_path,source_file_id,localized_text,override_count) VALUES(?,?,?,?,?,?,?,?,?)",
            localization_chains.size(), [&](Statement& s, std::size_t i) {
                const auto& group = localization_chains[i];
                const auto& row = group.second.front();
                auto r = bind_text(s, 1, row.language); if (!r) return r;
                r = bind_text(s, 2, row.key); if (!r) return r;
                r = bind_text(s, 3, row.layer); if (!r) return r;
                r = bind_text(s, 4, row.source_id); if (!r) return r;
                if (row.mod_id.empty()) r = s.bind_null(5); else r = bind_text(s, 5, row.mod_id);
                if (!r) return r;
                r = bind_text(s, 6, row.virtual_path); if (!r) return r;
                if (row.source_file_id == 0) r = s.bind_null(7); else r = s.bind(7, row.source_file_id);
                if (!r) return r;
                r = bind_text(s, 8, row.value); if (!r) return r;
                return s.bind(9, static_cast<std::int64_t>(group.second.size() - 1));
            });
        if (!inserted) return inserted;

        std::vector<LayerCandidate> effective_asset_candidates;
        for (const auto& row : effective_files) if (row.file_kind == "asset") effective_asset_candidates.push_back(row);
        std::map<std::string, const application::ScannedSourceFile*, std::less<>> mod_files_by_key;
        for (const auto& row : scan.content.source_files)
            mod_files_by_key.emplace(file_key(row.source_id, row.virtual_path), &row);
        std::map<std::int64_t, const BaseFile*> base_files_by_id;
        for (const auto& row : base_files) base_files_by_id.emplace(row.source_file_id, &row);
        inserted = insert_rows("INSERT INTO effective_assets(virtual_path,layer_type,winner_source_id,winner_mod_id,source_file_id,base_source_file_id,extension,size_bytes) "
                               "SELECT ?,?,?,?,?,?,?,?",
            effective_asset_candidates.size(), [&](Statement& s, std::size_t i) {
                const auto& row = effective_asset_candidates[i];
                auto r = bind_text(s, 1, row.virtual_path); if (!r) return r;
                r = bind_text(s, 2, row.layer); if (!r) return r;
                r = bind_text(s, 3, row.source_id); if (!r) return r;
                if (row.mod_id.empty()) r = s.bind_null(4); else r = bind_text(s, 4, row.mod_id);
                if (!r) return r;
                if (row.source_file_id == 0) r = s.bind_null(5); else r = s.bind(5, row.source_file_id);
                if (!r) return r;
                if (row.base_source_file_id == 0) r = s.bind_null(6); else r = s.bind(6, row.base_source_file_id);
                if (!r) return r;
                const auto mod_file = row.source_file_id != 0
                    ? mod_files_by_key.find(file_key(row.mod_id, row.virtual_path))
                    : mod_files_by_key.end();
                const auto base_file = row.base_source_file_id != 0
                    ? base_files_by_id.find(row.base_source_file_id)
                    : base_files_by_id.end();
                if (mod_file != mod_files_by_key.end()) {
                    r = bind_text(s, 7, mod_file->second->extension); if (!r) return r;
                    return s.bind(8, static_cast<std::int64_t>(mod_file->second->size_bytes));
                }
                if (base_file != base_files_by_id.end()) {
                    r = bind_text(s, 7, base_file->second->extension); if (!r) return r;
                    return s.bind(8, static_cast<std::int64_t>(base_file->second->size_bytes));
                }
                r = bind_text(s, 7, std::string_view{}); if (!r) return r;
                return s.bind(8, static_cast<std::int64_t>(0));
            });
        if (!inserted) return inserted;

        auto committed = transaction.commit();
        if (!committed) return committed;
        return core::Result<void, core::Error>::success();
    }();

    if (!built) {
        std::error_code remove_error;
        std::filesystem::remove(replacement, remove_error);
        return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(built.error());
    }
    auto replaced = replace_atomically(replacement, database_path);
    if (!replaced) {
        std::error_code remove_error;
        std::filesystem::remove(replacement, remove_error);
        return core::Result<ModEnvironmentBuildSummary, core::Error>::failure(replaced.error());
    }
    return core::Result<ModEnvironmentBuildSummary, core::Error>::success(summary);
}

} // namespace ddse::infrastructure
