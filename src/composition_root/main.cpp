#include "ddse/application/app_configuration.hpp"
#include "ddse/application/application_info.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/dson/dson_reader.hpp"
#include "ddse/infrastructure/console_logger.hpp"
#include "ddse/infrastructure/native_file_system.hpp"
#include "ddse/infrastructure/platform_info.hpp"
#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/migration_runner.hpp"

#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

void print_dson_fields(const ddse::core::dson::DsonDocument& document,
                       std::string_view embedded_parent = {}, std::size_t indentation = 0) {
    for (const auto& field : document.fields) {
        std::cout << std::string(indentation, ' ');
        if (!embedded_parent.empty()) std::cout << embedded_parent << " -> ";
        std::cout << field.path << " kind=" << ddse::core::dson::to_string(field.kind)
                  << " evidence=" << ddse::core::dson::to_string(field.type_evidence)
                  << " meta2=" << field.meta2_entry_index
                  << " name_offset=" << field.source_name_offset
                  << " value_offset=" << field.source_value_offset;
        if (field.kind == ddse::core::dson::ValueKind::Object)
            std::cout << " children=" << field.children.size();
        if (field.kind == ddse::core::dson::ValueKind::Unknown) {
            std::cout << " raw=" << field.raw_data.size() << " bytes hex=";
            const auto count = std::min<std::size_t>(field.raw_data.size(), 16);
            for (std::size_t i = 0; i < count; ++i)
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << std::to_integer<unsigned int>(field.raw_data[i]);
            std::cout << std::dec;
            if (field.raw_data.size() > count) std::cout << "…";
        }
        std::cout << '\n';
        if (field.embedded_document) {
            std::cout << std::string(indentation + 2, ' ') << "embedded DSON from " << field.path << ": "
                      << ddse::core::dson::summarize(*field.embedded_document).to_string() << '\n';
            print_dson_fields(*field.embedded_document, field.path, indentation + 4);
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << ddse::application::description() << '\n';
        return 0;
    }
    if ((argc == 3 || argc == 4) && std::string_view{argv[1]} == "--inspect-dson") {
        const bool print_fields = argc == 4 && std::string_view{argv[3]} == "--fields";
        if (argc == 4 && !print_fields) {
            std::cerr << "Usage: ddse_cli --inspect-dson <file> [--fields]\n";
            return 2;
        }
        ddse::infrastructure::NativeFileSystem file_system;
        auto bytes = file_system.read_file(argv[2]);
        if (!bytes) {
            std::cerr << ddse::core::to_string(bytes.error().code) << ": " << bytes.error().message << '\n';
            return 1;
        }
        const auto* data = reinterpret_cast<const std::byte*>(bytes.value().data());
        ddse::core::dson::DsonReader reader;
        auto document = reader.parse(std::span<const std::byte>{data, bytes.value().size()}, argv[2]);
        if (!document) {
            const auto& error = document.error();
            std::cerr << ddse::core::to_string(error.code) << " module=" << error.module
                      << " document=" << error.context.at("document")
                      << " section=" << error.context.at("section")
                      << " offset=" << error.context.at("offset")
                      << " message=\"" << error.message << "\"\n";
            for (auto cause = error.cause; cause; cause = cause->cause)
                std::cerr << "  caused by " << cause->module << ": " << cause->message << '\n';
            return 1;
        }
        std::cout << "DSON " << argv[2] << ": "
                  << ddse::core::dson::summarize(document.value()).to_string() << '\n';
        if (print_fields) print_dson_fields(document.value());
        return 0;
    }
    if (argc != 1) {
        std::cerr << "Usage: ddse_cli [--version | --inspect-dson <file> [--fields]]\n";
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
