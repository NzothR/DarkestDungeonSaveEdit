#include "ddse/infrastructure/database_mod_query.hpp"

#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/statement.hpp"

#include <array>
#include <system_error>

namespace ddse::infrastructure {
namespace {

std::filesystem::path path_from_utf8(std::string_view value) {
    return std::filesystem::u8path(value);
}

} // namespace

core::Result<std::vector<DatabaseModRecord>, core::Error>
read_enabled_mods(const std::filesystem::path& database_path) {
    auto opened = sqlite::ConnectionFactory{}.open(database_path);
    if (!opened) return core::Result<std::vector<DatabaseModRecord>, core::Error>::failure(opened.error());
    auto database = std::move(opened.value());
    auto prepared = database.prepare(
        "SELECT o.position,o.identity,o.provider_id,o.external_id,o.display_name,o.enabled,"
        "o.matched_mod_id,COALESCE(s.display_name,''),COALESCE(s.root_path,'') "
        "FROM mod_order_entries o LEFT JOIN mod_sources s ON s.mod_id=o.matched_mod_id "
        "WHERE o.order_source='save_profile' AND o.enabled=1 ORDER BY o.position");
    if (!prepared) return core::Result<std::vector<DatabaseModRecord>, core::Error>::failure(prepared.error());
    auto statement = std::move(prepared.value());
    std::vector<DatabaseModRecord> rows;
    while (true) {
        auto stepped = statement.step();
        if (!stepped) return core::Result<std::vector<DatabaseModRecord>, core::Error>::failure(stepped.error());
        if (!stepped.value()) break;
        DatabaseModRecord row;
        row.order = static_cast<std::size_t>(statement.column_int64(0));
        row.key = std::string{statement.column_text(1)};
        row.provider_id = std::string{statement.column_text(2)};
        row.external_id = std::string{statement.column_text(3)};
        row.fallback_name = std::string{statement.column_text(4)};
        row.enabled = statement.column_int64(5) != 0;
        if (!statement.column_is_null(6)) row.matched_mod_id = std::string{statement.column_text(6)};
        row.display_name = std::string{statement.column_text(7)};
        if (row.display_name.empty()) row.display_name = row.fallback_name;
        if (!statement.column_is_null(8)) row.root_path = path_from_utf8(statement.column_text(8));
        rows.push_back(std::move(row));
    }
    return core::Result<std::vector<DatabaseModRecord>, core::Error>::success(std::move(rows));
}

core::Result<std::filesystem::path, core::Error>
find_mod_cover(const std::filesystem::path& database_path, std::string_view mod_id) {
    auto opened = sqlite::ConnectionFactory{}.open(database_path);
    if (!opened) return core::Result<std::filesystem::path, core::Error>::failure(opened.error());
    auto database = std::move(opened.value());
    auto prepared = database.prepare("SELECT root_path FROM mod_sources WHERE mod_id=?1 AND enabled=1");
    if (!prepared) return core::Result<std::filesystem::path, core::Error>::failure(prepared.error());
    auto statement = std::move(prepared.value());
    auto bound = statement.bind(1, mod_id);
    if (!bound) return core::Result<std::filesystem::path, core::Error>::failure(bound.error());
    auto stepped = statement.step();
    if (!stepped) return core::Result<std::filesystem::path, core::Error>::failure(stepped.error());
    if (!stepped.value()) return core::Result<std::filesystem::path, core::Error>::failure(
        {core::ErrorCode::FileNotFound, "Mod was not found in the database", "DatabaseModQuery"});
    const auto root = path_from_utf8(statement.column_text(0));
    constexpr std::array<std::string_view, 9> names{
        "preview_icon.png", "preview.png", "preview.jpg", "preview.jpeg", "cover.png",
        "cover.jpg", "cover.jpeg", "mod_preview.png", "mod_preview.jpg"};
    std::error_code ec;
    for (const auto name : names) {
        const auto candidate = root / name;
        if (std::filesystem::is_regular_file(candidate, ec))
            return core::Result<std::filesystem::path, core::Error>::success(candidate);
    }
    return core::Result<std::filesystem::path, core::Error>::failure(
        {core::ErrorCode::FileNotFound, "Mod cover image was not found", "DatabaseModQuery"});
}

} // namespace ddse::infrastructure
