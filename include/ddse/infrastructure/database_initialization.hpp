#pragma once

#include "ddse/application/app_configuration.hpp"
#include "ddse/application/file_system.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ddse::infrastructure {

struct DatabaseInitializationState {
    std::string status{"idle"};
    std::string phase{"idle"};
    std::string current_work;
    int progress_percent{};
    std::uint64_t base_elapsed_ms{};
    std::uint64_t mod_elapsed_ms{};
    std::uint64_t total_elapsed_ms{};
    std::size_t installed_mods{};
    std::size_t enabled_mods{};
    std::vector<std::string> diagnostics;
};

class DatabaseInitializationManager {
public:
    explicit DatabaseInitializationManager(application::IFileSystem& file_system)
        : file_system_(file_system) {}
    ~DatabaseInitializationManager();

    void start(const application::AppConfiguration& configuration);
    [[nodiscard]] DatabaseInitializationState state() const;
    [[nodiscard]] std::filesystem::path mod_database_path() const;
    [[nodiscard]] std::filesystem::path base_database_path() const;

private:
    void run(application::AppConfiguration configuration);

    application::IFileSystem& file_system_;
    mutable std::mutex mutex_;
    DatabaseInitializationState state_;
    std::filesystem::path mod_database_path_;
    std::filesystem::path base_database_path_;
    std::thread worker_;
};

} // namespace ddse::infrastructure
