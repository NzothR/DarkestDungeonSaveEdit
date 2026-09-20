#pragma once

#include <filesystem>
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
    std::string language{"english"};
};

} // namespace ddse::application
