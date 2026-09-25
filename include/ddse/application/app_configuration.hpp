#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ddse::application {

struct AppConfiguration {
    std::filesystem::path game_root;
    std::vector<std::filesystem::path> workshop_roots;
    std::vector<std::filesystem::path> local_mod_roots;
    std::vector<std::filesystem::path> save_roots;
    std::filesystem::path backup_root;
    std::filesystem::path data_root;
    // UI locale. Content localization keeps its own game-language selection.
    std::string language{"zh_cn"};
    // The regular backup retention policy applies to complete profile backups
    // created during an explicit commit.  Auto edit saves use a separate
    // recovery slot under backup_root/AutoEditSave.
    std::uint32_t max_backup_count{20};
    bool auto_edit_save_enabled{true};
    std::uint32_t auto_edit_save_interval_seconds{30};
    // Initialized once from the selected save's largest equipped trinket list.
    std::optional<std::uint32_t> hero_trinket_slot_limit;
    // Initialized once from the largest positive/negative quirk counts.
    std::optional<std::uint32_t> hero_positive_quirk_limit;
    std::optional<std::uint32_t> hero_negative_quirk_limit;
};

} // namespace ddse::application
