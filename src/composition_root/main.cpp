#include "ddse/application/app_configuration.hpp"
#include "ddse/application/application_info.hpp"
#include "ddse/core/error.hpp"
#include "ddse/infrastructure/console_logger.hpp"
#include "ddse/infrastructure/native_file_system.hpp"
#include "ddse/infrastructure/platform_info.hpp"
#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/migration_runner.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << ddse::application::description() << '\n';
        return 0;
    }
    if (argc != 1) {
        std::cerr << "Usage: ddse_cli [--version]\n";
        return 2;
    }
    ddse::infrastructure::ConsoleLogger logger{std::clog};
    ddse::infrastructure::NativeFileSystem file_system;
    ddse::application::AppConfiguration config;
    config.data_root = std::filesystem::current_path() / "ddse-data";
    config.backup_root = config.data_root / "backups";

    auto directories = file_system.create_directories(config.data_root);
    if (!directories) {
        const auto& error = directories.error();
        std::cerr << ddse::core::to_string(error.code) << ": " << error.message << '\n';
        return 1;
    }
    ddse::infrastructure::sqlite::ConnectionFactory connections;
    auto database = connections.open(config.data_root / "base_content.db");
    if (!database) {
        const auto& error = database.error();
        std::cerr << ddse::core::to_string(error.code) << ": " << error.message << '\n';
        return 1;
    }
    const std::vector<ddse::infrastructure::sqlite::Migration> migrations{
        {1, "stage0_metadata", "CREATE TABLE IF NOT EXISTS app_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)"}};
    ddse::infrastructure::sqlite::MigrationRunner migration_runner;
    auto migrated = migration_runner.apply(database.value(), migrations);
    if (!migrated) {
        const auto& error = migrated.error();
        std::cerr << ddse::core::to_string(error.code) << ": " << error.message << '\n';
        return 1;
    }
    logger.log(ddse::application::LogLevel::Info, "Backend composition root ready",
               {{"module", "CompositionRoot"}, {"operation", "startup"},
                {"platform", std::string{ddse::infrastructure::platform_name()}}});
    std::cout << ddse::application::description() << " backend skeleton ("
              << ddse::infrastructure::platform_name() << ")\n";
    return 0;
}
