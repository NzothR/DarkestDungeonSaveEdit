#include "ddse/infrastructure/base_content_database.hpp"

#include "ddse/core/dson/dson_document.hpp"
#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/statement.hpp"
#include "ddse/infrastructure/sqlite/transaction.hpp"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <map>
#include <string>
#include <tuple>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#endif

namespace ddse::infrastructure {
namespace {

std::string file_key(std::string_view source_id, std::string_view virtual_path) {
    std::string key{source_id};
    key.push_back('\n');
    key.append(virtual_path);
    return key;
}

std::string hex_u64(std::uint64_t value) {
    char buffer[17]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

core::Result<void, core::Error> replace_atomically(const std::filesystem::path& replacement,
                                                    const std::filesystem::path& target) {
#ifdef _WIN32
    const BOOL ok = MoveFileExW(replacement.c_str(), target.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        const auto code = GetLastError();
        return core::Result<void, core::Error>::failure(
            {core::ErrorCode::IoError, "Atomic base database replacement failed", "BaseContentDatabase",
             {{"target", target.string()}, {"replacement", replacement.string()}, {"os_error", std::to_string(code)}}});
    }
#else
    if (std::rename(replacement.c_str(), target.c_str()) != 0)
        return core::Result<void, core::Error>::failure(
            {core::ErrorCode::IoError, "Atomic base database replacement failed", "BaseContentDatabase",
             {{"target", target.string()}, {"replacement", replacement.string()}, {"os_error", std::to_string(errno)}}});
#endif
    return core::Result<void, core::Error>::success();
}

bool is_within_path(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    auto candidate_it = candidate.begin();
    auto root_it = root.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end()) return false;
        auto candidate_component = candidate_it->generic_string();
        auto root_component = root_it->generic_string();
#ifdef _WIN32
        std::transform(candidate_component.begin(), candidate_component.end(), candidate_component.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(root_component.begin(), root_component.end(), root_component.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        if (candidate_component != root_component) return false;
    }
    return true;
}

template <typename T>
core::Result<void, core::Error> bind_text(sqlite::Statement& statement, int index, const T& value) {
    return statement.bind(index, std::string_view{value});
}

} // namespace

core::Result<BaseContentBuildSummary, core::Error>
BaseContentDatabaseBuilder::rebuild(const std::filesystem::path& database_path,
                                    const application::BaseContentScanResult& scan) const {
    std::error_code ec;
    const auto output_path_absolute = std::filesystem::absolute(database_path, ec);
    if (ec) return core::Result<BaseContentBuildSummary, core::Error>::failure(
        {core::ErrorCode::IoError, ec.message(), "BaseContentDatabase", {{"path", database_path.string()}}});
    const auto output_absolute = std::filesystem::weakly_canonical(output_path_absolute, ec);
    if (ec) return core::Result<BaseContentBuildSummary, core::Error>::failure(
        {core::ErrorCode::IoError, ec.message(), "BaseContentDatabase", {{"path", database_path.string()}}});
    for (const auto& source : scan.sources) {
        const auto source_absolute = std::filesystem::weakly_canonical(source.root_path, ec);
        if (ec) return core::Result<BaseContentBuildSummary, core::Error>::failure(
            {core::ErrorCode::IoError, ec.message(), "BaseContentDatabase", {{"path", source.root_path}}});
        if (is_within_path(output_absolute, source_absolute))
            return core::Result<BaseContentBuildSummary, core::Error>::failure(
                {core::ErrorCode::ValidationFailed,
                 "Database output must be outside all scanned game and DLC source roots",
                 "BaseContentDatabase", {{"database_path", output_absolute.string()}, {"source_root", source_absolute.string()}}});
    }
    const auto parent = database_path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) return core::Result<BaseContentBuildSummary, core::Error>::failure(
        {core::ErrorCode::IoError, ec.message(), "BaseContentDatabase", {{"path", parent.string()}}});

    auto replacement = database_path;
    replacement += ".new";
    std::filesystem::remove(replacement, ec);
    if (ec) return core::Result<BaseContentBuildSummary, core::Error>::failure(
        {core::ErrorCode::IoError, ec.message(), "BaseContentDatabase", {{"path", replacement.string()}}});

    BaseContentBuildSummary summary;
    summary.sources = scan.sources.size();
    summary.source_files = scan.source_files.size();
    summary.assets = scan.assets.size();
    summary.asset_references = scan.asset_references.size();
    summary.relationships = scan.relationships.size();
    summary.diagnostics = scan.diagnostics.size();
    for (const auto& definition : scan.definitions) {
        if (definition.kind == "hero_class") ++summary.hero_classes;
        else if (definition.kind == "skill") ++summary.skills;
        else if (definition.kind == "trinket") ++summary.trinkets;
        else if (definition.kind == "quirk") ++summary.quirks;
        else if (definition.kind == "disease") ++summary.diseases;
        else if (definition.kind == "resource") ++summary.resources;
        else if (definition.kind == "building") ++summary.buildings;
    }

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
CREATE TABLE schema_info (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE content_sources (
 source_id TEXT PRIMARY KEY, source_type TEXT NOT NULL, display_name TEXT NOT NULL, root_path TEXT NOT NULL
);
CREATE TABLE source_files (
 source_file_id INTEGER PRIMARY KEY, source_id TEXT NOT NULL REFERENCES content_sources(source_id),
 virtual_path TEXT NOT NULL, extension TEXT NOT NULL, size_bytes INTEGER NOT NULL,
 content_fingerprint TEXT NOT NULL, file_kind TEXT NOT NULL,
 UNIQUE(source_id, virtual_path)
);
CREATE TABLE content_definitions (
 definition_id INTEGER PRIMARY KEY, definition_type TEXT NOT NULL, content_id TEXT NOT NULL,
 source_file_id INTEGER NOT NULL REFERENCES source_files(source_file_id),
 display_name TEXT NOT NULL, localization_key TEXT NOT NULL, payload_json TEXT NOT NULL
);
CREATE TABLE localization_entries (
 language TEXT NOT NULL, localization_key TEXT NOT NULL, localized_text TEXT NOT NULL,
 source_file_id INTEGER NOT NULL REFERENCES source_files(source_file_id),
 PRIMARY KEY(language, localization_key, source_file_id)
);
CREATE TABLE assets (
 asset_id INTEGER PRIMARY KEY, source_file_id INTEGER NOT NULL UNIQUE REFERENCES source_files(source_file_id),
 virtual_path TEXT NOT NULL, extension TEXT NOT NULL, size_bytes INTEGER NOT NULL
);
CREATE TABLE content_asset_references (
 asset_reference_id INTEGER PRIMARY KEY, definition_type TEXT NOT NULL, content_id TEXT NOT NULL,
 source_file_id INTEGER NOT NULL REFERENCES source_files(source_file_id), asset_role TEXT NOT NULL,
 reference_type TEXT NOT NULL, virtual_path TEXT NOT NULL, reference_origin TEXT NOT NULL,
 UNIQUE(definition_type,content_id,source_file_id,asset_role,reference_type,virtual_path)
);
CREATE TABLE content_relationships (
 relationship_id INTEGER PRIMARY KEY, parent_type TEXT NOT NULL, parent_id TEXT NOT NULL,
 relationship_type TEXT NOT NULL, child_type TEXT NOT NULL, child_id TEXT NOT NULL,
 source_file_id INTEGER NOT NULL REFERENCES source_files(source_file_id),
 UNIQUE(parent_type,parent_id,relationship_type,child_type,child_id,source_file_id)
);
CREATE TABLE content_hash_index (
 hash_value INTEGER NOT NULL, definition_type TEXT NOT NULL, content_id TEXT NOT NULL,
 source_file_id INTEGER NOT NULL REFERENCES source_files(source_file_id),
 PRIMARY KEY(hash_value, definition_type, content_id, source_file_id)
);
CREATE TABLE scan_diagnostics (
 diagnostic_id INTEGER PRIMARY KEY, source_id TEXT NOT NULL, virtual_path TEXT NOT NULL, message TEXT NOT NULL
);
CREATE INDEX idx_definitions_type_id ON content_definitions(definition_type, content_id);
CREATE INDEX idx_localization_language_key ON localization_entries(language, localization_key);
CREATE INDEX idx_hash_value ON content_hash_index(hash_value);
CREATE INDEX idx_assets_virtual_path ON assets(virtual_path);
CREATE INDEX idx_asset_references_entity ON content_asset_references(definition_type,content_id,source_file_id);
CREATE INDEX idx_content_relationships_parent ON content_relationships(parent_type,parent_id,relationship_type);
)SQL";
        auto created = database.execute(schema);
        if (!created) return core::Result<void, core::Error>::failure(created.error());

        std::map<std::string, std::int64_t, std::less<>> source_file_ids;
        std::int64_t next_file_id = 1;
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
                    {core::ErrorCode::DatabaseError, "Insert unexpectedly returned a row", "BaseContentDatabase", {}});
                auto reset = statement.reset();
                if (!reset) return reset;
            }
            return core::Result<void, core::Error>::success();
        };

        auto inserted = insert_rows("INSERT INTO schema_info(key,value) VALUES(?,?)", 2,
            [&](sqlite::Statement& s, std::size_t row) {
                auto r = bind_text(s, 1, row == 0 ? std::string_view{"schema_version"} : std::string_view{"scanner_version"});
                if (!r) return r;
                return bind_text(s, 2, row == 0 ? std::string_view{"2"} : std::string_view{"stage9"});
            });
        if (!inserted) return inserted;

        inserted = insert_rows("INSERT INTO content_sources(source_id,source_type,display_name,root_path) VALUES(?,?,?,?)",
            scan.sources.size(), [&](sqlite::Statement& s, std::size_t i) {
                const auto& row = scan.sources[i];
                auto r = bind_text(s, 1, row.id); if (!r) return r;
                r = bind_text(s, 2, application::to_string(row.type)); if (!r) return r;
                r = bind_text(s, 3, row.name); if (!r) return r;
                return bind_text(s, 4, row.root_path);
            });
        if (!inserted) return inserted;

        inserted = insert_rows("INSERT INTO source_files(source_id,virtual_path,extension,size_bytes,content_fingerprint,file_kind) VALUES(?,?,?,?,?,?)",
            scan.source_files.size(), [&](sqlite::Statement& s, std::size_t i) {
                const auto& row = scan.source_files[i];
                const auto id = next_file_id++;
                source_file_ids.emplace(file_key(row.source_id, row.virtual_path), id);
                auto r = bind_text(s, 1, row.source_id); if (!r) return r;
                r = bind_text(s, 2, row.virtual_path); if (!r) return r;
                r = bind_text(s, 3, row.extension); if (!r) return r;
                r = s.bind(4, static_cast<std::int64_t>(row.size_bytes)); if (!r) return r;
                r = bind_text(s, 5, hex_u64(row.content_fingerprint)); if (!r) return r;
                return bind_text(s, 6, row.is_asset ? std::string_view{"asset"} : std::string_view{"content"});
            });
        if (!inserted) return inserted;

        inserted = insert_rows("INSERT INTO content_definitions(definition_type,content_id,source_file_id,display_name,localization_key,payload_json) VALUES(?,?,?,?,?,?)",
            scan.definitions.size(), [&](sqlite::Statement& s, std::size_t i) {
                const auto& row = scan.definitions[i];
                const auto found = source_file_ids.find(file_key(row.source_id, row.virtual_path));
                if (found == source_file_ids.end()) return core::Result<void, core::Error>::failure(
                    {core::ErrorCode::ValidationFailed, "Definition references an unregistered source file", "BaseContentDatabase",
                     {{"source", row.source_id}, {"path", row.virtual_path}}});
                auto r = bind_text(s, 1, row.kind); if (!r) return r;
                r = bind_text(s, 2, row.id); if (!r) return r;
                r = s.bind(3, found->second); if (!r) return r;
                r = bind_text(s, 4, row.display_name); if (!r) return r;
                r = bind_text(s, 5, row.localization_key); if (!r) return r;
                return bind_text(s, 6, row.payload_json);
            });
        if (!inserted) return inserted;

        inserted = insert_rows("INSERT OR IGNORE INTO content_hash_index(hash_value,definition_type,content_id,source_file_id) VALUES(?,?,?,?)",
            scan.definitions.size(), [&](sqlite::Statement& s, std::size_t i) {
                const auto& row = scan.definitions[i];
                const auto found = source_file_ids.find(file_key(row.source_id, row.virtual_path));
                if (found == source_file_ids.end()) return core::Result<void, core::Error>::failure(
                    {core::ErrorCode::ValidationFailed, "Hash entry references an unregistered source file", "BaseContentDatabase", {}});
                auto r = s.bind(1, static_cast<std::int64_t>(core::dson::string_hash(row.id))); if (!r) return r;
                r = bind_text(s, 2, row.kind); if (!r) return r;
                r = bind_text(s, 3, row.id); if (!r) return r;
                return s.bind(4, found->second);
            });
        if (!inserted) return inserted;

        inserted = insert_rows("INSERT OR REPLACE INTO localization_entries(language,localization_key,localized_text,source_file_id) VALUES(?,?,?,?)",
            scan.localizations.size(), [&](sqlite::Statement& s, std::size_t i) {
                const auto& row = scan.localizations[i];
                const auto found = source_file_ids.find(file_key(row.source_id, row.virtual_path));
                if (found == source_file_ids.end()) return core::Result<void, core::Error>::failure(
                    {core::ErrorCode::ValidationFailed, "Localization references an unregistered source file", "BaseContentDatabase", {}});
                auto r = bind_text(s, 1, row.language); if (!r) return r;
                r = bind_text(s, 2, row.key); if (!r) return r;
                r = bind_text(s, 3, row.value); if (!r) return r;
                return s.bind(4, found->second);
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
                {core::ErrorCode::DatabaseError, "Localization count query returned no row", "BaseContentDatabase", {}});
        summary.localization_entries = static_cast<std::size_t>(localization_count.column_int64(0));

        inserted = insert_rows("INSERT INTO assets(source_file_id,virtual_path,extension,size_bytes) VALUES(?,?,?,?)",
            scan.assets.size(), [&](sqlite::Statement& s, std::size_t i) {
                const auto& row = scan.assets[i];
                const auto found = source_file_ids.find(file_key(row.source_id, row.virtual_path));
                if (found == source_file_ids.end()) return core::Result<void, core::Error>::failure(
                    {core::ErrorCode::ValidationFailed, "Asset references an unregistered source file", "BaseContentDatabase", {}});
                auto r = s.bind(1, found->second); if (!r) return r;
                r = bind_text(s, 2, row.virtual_path); if (!r) return r;
                r = bind_text(s, 3, row.extension); if (!r) return r;
                return s.bind(4, static_cast<std::int64_t>(row.size_bytes));
            });
        if (!inserted) return inserted;

        inserted = insert_rows("INSERT INTO content_asset_references(definition_type,content_id,source_file_id,asset_role,reference_type,virtual_path,reference_origin) VALUES(?,?,?,?,?,?,?)",
            scan.asset_references.size(), [&](sqlite::Statement& s, std::size_t i) {
                const auto& row = scan.asset_references[i];
                const auto found = source_file_ids.find(file_key(row.source_id, row.definition_virtual_path));
                if (found == source_file_ids.end()) return core::Result<void, core::Error>::failure(
                    {core::ErrorCode::ValidationFailed, "Asset reference has no source definition file", "BaseContentDatabase",
                     {{"source", row.source_id}, {"path", row.definition_virtual_path}}});
                auto r = bind_text(s, 1, row.definition_type); if (!r) return r;
                r = bind_text(s, 2, row.content_id); if (!r) return r;
                r = s.bind(3, found->second); if (!r) return r;
                r = bind_text(s, 4, row.asset_role); if (!r) return r;
                r = bind_text(s, 5, row.reference_type); if (!r) return r;
                r = bind_text(s, 6, row.virtual_path); if (!r) return r;
                return bind_text(s, 7, row.reference_origin);
            });
        if (!inserted) return inserted;

        inserted = insert_rows("INSERT INTO content_relationships(parent_type,parent_id,relationship_type,child_type,child_id,source_file_id) VALUES(?,?,?,?,?,?)",
            scan.relationships.size(), [&](sqlite::Statement& s, std::size_t i) {
                const auto& row = scan.relationships[i];
                const auto found = source_file_ids.find(file_key(row.source_id, row.virtual_path));
                if (found == source_file_ids.end()) return core::Result<void, core::Error>::failure(
                    {core::ErrorCode::ValidationFailed, "Content relationship has no source definition file", "BaseContentDatabase",
                     {{"source", row.source_id}, {"path", row.virtual_path}}});
                auto r = bind_text(s, 1, row.parent_type); if (!r) return r;
                r = bind_text(s, 2, row.parent_id); if (!r) return r;
                r = bind_text(s, 3, row.relationship_type); if (!r) return r;
                r = bind_text(s, 4, row.child_type); if (!r) return r;
                r = bind_text(s, 5, row.child_id); if (!r) return r;
                return s.bind(6, found->second);
            });
        if (!inserted) return inserted;

        inserted = insert_rows("INSERT INTO scan_diagnostics(source_id,virtual_path,message) VALUES(?,?,?)",
            scan.diagnostics.size(), [&](sqlite::Statement& s, std::size_t i) {
                const auto& row = scan.diagnostics[i];
                auto r = bind_text(s, 1, row.source_id); if (!r) return r;
                r = bind_text(s, 2, row.virtual_path); if (!r) return r;
                return bind_text(s, 3, row.message);
            });
        if (!inserted) return inserted;

        auto committed = transaction.commit();
        if (!committed) return committed;
        return core::Result<void, core::Error>::success();
    }();

    if (!built) {
        std::error_code remove_error;
        std::filesystem::remove(replacement, remove_error);
        return core::Result<BaseContentBuildSummary, core::Error>::failure(built.error());
    }
    auto replaced = replace_atomically(replacement, database_path);
    if (!replaced) {
        std::error_code remove_error;
        std::filesystem::remove(replacement, remove_error);
        return core::Result<BaseContentBuildSummary, core::Error>::failure(replaced.error());
    }
    return core::Result<BaseContentBuildSummary, core::Error>::success(summary);
}

} // namespace ddse::infrastructure
