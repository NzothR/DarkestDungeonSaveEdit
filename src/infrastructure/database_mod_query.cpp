#include "ddse/infrastructure/database_mod_query.hpp"

#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/statement.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <system_error>
#include <limits>

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

core::Result<std::filesystem::path, core::Error>
find_town_asset(const std::filesystem::path& database_path, std::string_view role) {
    auto opened = sqlite::ConnectionFactory{}.open(database_path);
    if (!opened) return core::Result<std::filesystem::path, core::Error>::failure(opened.error());
    auto database = std::move(opened.value());
    auto prepared = database.prepare(
        "SELECT s.root_path,f.virtual_path,a.size_bytes FROM assets a "
        "JOIN source_files f USING(source_file_id) JOIN content_sources s USING(source_id) "
        "WHERE lower(f.virtual_path) LIKE '%town%' OR lower(f.virtual_path) LIKE '%hamlet%' "
        "OR lower(f.virtual_path) LIKE '%estate%'");
    if (!prepared) return core::Result<std::filesystem::path, core::Error>::failure(prepared.error());
    auto statement = std::move(prepared.value());
    std::filesystem::path best;
    int best_score = std::numeric_limits<int>::min();
    std::int64_t best_size = -1;
    while (true) {
        auto stepped = statement.step();
        if (!stepped) return core::Result<std::filesystem::path, core::Error>::failure(stepped.error());
        if (!stepped.value()) break;
        const auto root = path_from_utf8(statement.column_text(0));
        const auto virtual_path = std::string{statement.column_text(1)};
        const auto size_bytes = statement.column_int64(2);
        const auto lower = [&] {
            auto value = virtual_path;
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }();
        const auto candidate = root / std::filesystem::path{virtual_path};
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec)) continue;
        int score = 0;
        if (lower.find("town") != std::string::npos) score += 20;
        if (lower.find("hamlet") != std::string::npos) score += 18;
        if (lower.find("estate") != std::string::npos) score += 12;
        if (lower.find("fx/town_ground/town_ground.sprite.png") != std::string::npos ||
            lower.find("fx\\town_ground\\town_ground.sprite.png") != std::string::npos) score += 10000;
        if (lower.find("background") != std::string::npos) score += 30;
        if (lower.find("scene") != std::string::npos) score += 15;
        if (lower.ends_with(".png")) score += 5;
        if (role == "background" && lower.find("building") != std::string::npos) score -= 12;
        if (score > best_score || (score == best_score && size_bytes > best_size)) {
            best_score = score;
            best_size = size_bytes;
            best = candidate;
        }
    }
    if (!best.empty()) return core::Result<std::filesystem::path, core::Error>::success(best);
    return core::Result<std::filesystem::path, core::Error>::failure(
        {core::ErrorCode::FileNotFound, "Town asset was not found in the base content database", "DatabaseModQuery"});
}

} // namespace ddse::infrastructure
