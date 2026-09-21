#include "ddse/application/app_configuration.hpp"
#include "ddse/application/application_info.hpp"
#include "ddse/application/content_scanner.hpp"
#include "ddse/application/mod_environment.hpp"
#include "ddse/application/save_profile.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/dson/dson_reader.hpp"
#include "ddse/infrastructure/console_logger.hpp"
#include "ddse/infrastructure/base_content_database.hpp"
#include "ddse/infrastructure/mod_environment_database.hpp"
#include "ddse/infrastructure/native_file_system.hpp"
#include "ddse/infrastructure/platform_info.hpp"
#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/migration_runner.hpp"
#include "ddse/infrastructure/sqlite/statement.hpp"

#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::filesystem::path argument_path(std::string_view value) {
    return std::filesystem::path{std::u8string{
        reinterpret_cast<const char8_t*>(value.data()), value.size()}};
}

#ifdef _WIN32
std::string utf8_from_wide(std::wstring_view value) {
    if (value.empty()) return {};
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string converted(static_cast<std::size_t>(required), '\0');
    const auto written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), converted.data(), required, nullptr, nullptr);
    if (written != required) return {};
    return converted;
}
#endif

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

namespace {

int inspect_profile(const std::filesystem::path& profile_path) {
    ddse::infrastructure::NativeFileSystem file_system;
    ddse::application::SaveProfileDiscovery discovery{file_system};
    auto profile = discovery.load(profile_path);
    if (!profile) {
        std::cerr << ddse::core::to_string(profile.error().code) << ": " << profile.error().message << '\n';
        return 1;
    }

    const auto& descriptor = profile.value().descriptor;
    std::cout << "Profile " << descriptor.id << " status="
              << ddse::application::to_string(profile.value().status)
              << " documents=" << profile.value().documents.size() << " fingerprint=0x"
              << std::hex << profile.value().baseline_fingerprint << std::dec << " domains=";
    if (descriptor.detected_domains.empty()) std::cout << "none";
    for (std::size_t i = 0; i < descriptor.detected_domains.size(); ++i) {
        if (i != 0) std::cout << ',';
        std::cout << ddse::application::to_string(descriptor.detected_domains[i]);
    }
    std::cout << '\n';
    for (const auto& [id, document] : profile.value().documents)
        std::cout << "  " << id << " bytes=" << document.bytes.size()
                  << " decoded=" << (document.decoded ? "yes" : "no")
                  << " core=" << (document.core_document ? "yes" : "no") << '\n';
    for (const auto& diagnostic : descriptor.diagnostics)
        std::cout << "  diagnostic document=" << diagnostic.document_id
                  << " core=" << (diagnostic.core_document ? "yes" : "no")
                  << " message=\"" << diagnostic.message << "\"\n";
    return 0;
}

int scan_base_content(const std::filesystem::path& game_root, const std::filesystem::path& database_path) {
    ddse::infrastructure::NativeFileSystem file_system;
    auto config = ddse::application::BaseContentScanConfig::defaults(game_root);
    ddse::application::BaseContentScanner scanner{file_system};
    auto scan = scanner.scan(config);
    if (!scan) {
        std::cerr << ddse::core::to_string(scan.error().code) << ": " << scan.error().message << '\n';
        return 1;
    }
    ddse::infrastructure::BaseContentDatabaseBuilder builder;
    auto built = builder.rebuild(database_path, scan.value());
    if (!built) {
        std::cerr << ddse::core::to_string(built.error().code) << ": " << built.error().message << '\n';
        return 1;
    }
    const auto& summary = built.value();
    std::cout << "base_content.db rebuilt: sources=" << summary.sources
              << " source_files=" << summary.source_files
              << " hero_classes=" << summary.hero_classes << " skills=" << summary.skills
              << " trinkets=" << summary.trinkets << " quirks=" << summary.quirks
              << " diseases=" << summary.diseases << " resources=" << summary.resources
              << " buildings=" << summary.buildings << " localization=" << summary.localization_entries
              << " assets=" << summary.assets << " asset_references=" << summary.asset_references
              << " relationships=" << summary.relationships << " diagnostics=" << summary.diagnostics << '\n';
    for (const auto& source : scan.value().sources)
        std::cout << "  source " << source.id << " type=" << ddse::application::to_string(source.type)
                  << " name=" << source.name << '\n';
    for (std::size_t i = 0; i < std::min<std::size_t>(scan.value().diagnostics.size(), 10); ++i) {
        const auto& diagnostic = scan.value().diagnostics[i];
        std::cout << "  diagnostic " << diagnostic.source_id << ':' << diagnostic.virtual_path
                  << " " << diagnostic.message << '\n';
    }

    auto database = ddse::infrastructure::sqlite::ConnectionFactory{}.open(database_path);
    if (!database) {
        std::cerr << ddse::core::to_string(database.error().code) << ": " << database.error().message << '\n';
        return 1;
    }
    auto example = database.value().prepare(
        "SELECT d.content_id,s.display_name,s.source_type,f.virtual_path,d.localization_key,"
        "COALESCE((SELECT l.localized_text FROM localization_entries l WHERE l.localization_key=d.localization_key "
        "AND lower(l.language)=lower(?) ORDER BY l.source_file_id DESC LIMIT 1),'') "
        "FROM content_definitions d JOIN source_files f USING(source_file_id) "
        "JOIN content_sources s USING(source_id) WHERE d.definition_type='trinket' ORDER BY d.content_id LIMIT 1");
    if (!example) {
        std::cerr << ddse::core::to_string(example.error().code) << ": " << example.error().message << '\n';
        return 1;
    }
    auto statement = std::move(example.value());
    auto bound = statement.bind(1, config.preferred_language);
    if (!bound) return 1;
    auto row = statement.step();
    if (!row) {
        std::cerr << ddse::core::to_string(row.error().code) << ": " << row.error().message << '\n';
        return 1;
    }
    if (row.value()) {
        const auto id = std::string{statement.column_text(0)};
        std::cout << "sample trinket id=" << id << " source=" << statement.column_text(1)
                  << '/' << statement.column_text(2) << " file=" << statement.column_text(3)
                  << " localization_key=" << statement.column_text(4) << " name=";
        const auto name = statement.column_text(5);
        std::cout << (name.empty() ? "(unlocalized)" : std::string{name}) << '\n';
        auto asset = database.value().prepare(
            "SELECT a.virtual_path FROM assets a WHERE lower(a.virtual_path) LIKE '%'||lower(?)||'%' "
            "ORDER BY a.virtual_path LIMIT 1");
        if (asset) {
            auto asset_statement = std::move(asset.value());
            if (asset_statement.bind(1, id)) {
                auto asset_row = asset_statement.step();
                if (asset_row && asset_row.value()) std::cout << "  asset=" << asset_statement.column_text(0) << '\n';
            }
        }
    } else std::cout << "sample trinket: none found\n";
    return 0;
}

int scan_mod_environment(char* argv[]) {
    // argv[2..7] = save profile, Workshop root, local root, manager export,
    // base catalog, and output environment catalog. A dash omits either order source.
    ddse::infrastructure::NativeFileSystem file_system;
    ddse::application::ModEnvironmentScanConfig config;
    if (std::string_view{argv[2]} != "-") config.save_profile_root = argument_path(argv[2]);
    config.workshop_root = argument_path(argv[3]);
    if (std::string_view{argv[4]} != "-") config.local_mod_roots.push_back(argument_path(argv[4]));
    if (std::string_view{argv[5]} != "-") config.manager_order_json = argument_path(argv[5]);
    config.base_content_database = argument_path(argv[6]);

    ddse::application::ModEnvironmentScanner scanner{file_system};
    auto scan = scanner.scan(config);
    if (!scan) {
        std::cerr << ddse::core::to_string(scan.error().code) << ": " << scan.error().message << '\n';
        for (const auto& [key, value] : scan.error().context)
            std::cerr << "  " << key << '=' << value << '\n';
        return 1;
    }
    ddse::infrastructure::ModEnvironmentDatabaseBuilder builder;
    auto built = builder.rebuild(argument_path(argv[7]), config.base_content_database, scan.value());
    if (!built) {
        std::cerr << ddse::core::to_string(built.error().code) << ": " << built.error().message << '\n';
        for (const auto& [key, value] : built.error().context)
            std::cerr << "  " << key << '=' << value << '\n';
        return 1;
    }

    const auto& summary = built.value();
    const auto enabled_entries = [](const auto& entries) {
        return std::count_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.enabled; });
    };
    std::cout << "mod_environment.db rebuilt: installed=" << summary.installed_mods
              << " enabled=" << summary.enabled_mods << " workshop=" << summary.workshop_mods
              << " local=" << summary.local_mods << " source_files=" << summary.source_files
              << " definitions=" << summary.definitions << " localization=" << summary.localization_entries
              << " assets=" << summary.assets << " asset_references=" << summary.asset_references
              << " relationships=" << summary.relationships << " effective_paths=" << summary.effective_paths
              << " overridden_paths=" << summary.overridden_paths
              << " diagnostics=" << summary.diagnostics << '\n'
              << "  effective order source=" << scan.value().effective_order_source
              << " save/manager exact=" << (summary.save_order_matches_manager ? "yes" : "no")
              << " shared order=" << (summary.shared_order_matches ? "same" : "different") << '\n';
    if (scan.value().comparison.save_available && scan.value().comparison.manager_export_available) {
        std::cout << "  save enabled=" << enabled_entries(scan.value().save_order)
                  << " manager enabled=" << enabled_entries(scan.value().manager_order)
                  << " only in save=" << scan.value().comparison.only_in_save.size()
                  << " only in manager=" << scan.value().comparison.only_in_manager_export.size() << '\n';
        for (const auto& identity : scan.value().comparison.only_in_save)
            std::cout << "    save only: " << identity << '\n';
        for (const auto& identity : scan.value().comparison.only_in_manager_export)
            std::cout << "    manager only: " << identity << '\n';
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(scan.value().diagnostics.size(), 20); ++i) {
        const auto& diagnostic = scan.value().diagnostics[i];
        std::cout << "  diagnostic " << diagnostic.mod_id << ':' << diagnostic.virtual_path
                  << " " << diagnostic.message << '\n';
    }
    if (scan.value().diagnostics.size() > 20)
        std::cout << "  ... " << scan.value().diagnostics.size() - 20 << " more diagnostics in database\n";
    return 0;
}

} // namespace

int run_cli(int argc, char* argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << ddse::application::description() << '\n';
        return 0;
    }
    if (argc == 3 && std::string_view{argv[1]} == "--inspect-profile")
        return inspect_profile(argument_path(argv[2]));
    if (argc == 4 && std::string_view{argv[1]} == "--scan-base-content")
        return scan_base_content(argument_path(argv[2]), argument_path(argv[3]));
    if (argc == 8 && std::string_view{argv[1]} == "--scan-mod-environment")
        return scan_mod_environment(argv);
    if (argc == 3 && std::string_view{argv[1]} == "--discover-profiles") {
        ddse::infrastructure::NativeFileSystem file_system;
        ddse::application::SaveProfileDiscovery discovery{file_system};
        auto profiles = discovery.discover(argument_path(argv[2]));
        if (!profiles) {
            std::cerr << ddse::core::to_string(profiles.error().code) << ": " << profiles.error().message << '\n';
            return 1;
        }
        for (const auto& profile : profiles.value()) {
            std::cout << profile.descriptor.id << " status=" << ddse::application::to_string(profile.status)
                      << " documents=" << profile.documents.size() << " domains=";
            if (profile.descriptor.detected_domains.empty()) std::cout << "none";
            for (std::size_t i = 0; i < profile.descriptor.detected_domains.size(); ++i) {
                if (i != 0) std::cout << ',';
                std::cout << ddse::application::to_string(profile.descriptor.detected_domains[i]);
            }
            std::cout << '\n';
        }
        return 0;
    }
    if ((argc == 3 || argc == 4) && std::string_view{argv[1]} == "--inspect-dson") {
        const bool print_fields = argc == 4 && std::string_view{argv[3]} == "--fields";
        if (argc == 4 && !print_fields) {
            std::cerr << "Usage: ddse_cli --inspect-dson <file> [--fields]\n";
            return 2;
        }
        ddse::infrastructure::NativeFileSystem file_system;
        const auto dson_path = argument_path(argv[2]);
        auto bytes = file_system.read_file(dson_path);
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
        std::cerr << "Usage: ddse_cli [--version | --scan-base-content <game-root> <database-path> | "
                     "--scan-mod-environment <save-profile|-> <workshop-root> <local-root|-> "
                     "<manager-order.json|-> <base-content.db> <environment.db> | "
                     "--discover-profiles <directory> | --inspect-profile <directory> | "
                     "--inspect-dson <file> [--fields]]\n";
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

#ifdef _WIN32
int main() {
    int wide_argc = 0;
    auto wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (!wide_argv) {
        std::cerr << "Unable to read Unicode command-line arguments\n";
        return 2;
    }
    std::vector<std::string> utf8_arguments;
    std::vector<char*> narrow_argv;
    utf8_arguments.reserve(static_cast<std::size_t>(wide_argc));
    for (int i = 0; i < wide_argc; ++i) {
        auto converted = utf8_from_wide(wide_argv[i]);
        if (!wide_argv[i][0] || !converted.empty()) utf8_arguments.push_back(std::move(converted));
        else {
            LocalFree(wide_argv);
            std::cerr << "Unable to convert a command-line argument to UTF-8\n";
            return 2;
        }
    }
    LocalFree(wide_argv);
    for (auto& argument : utf8_arguments) narrow_argv.push_back(argument.data());
    return run_cli(wide_argc, narrow_argv.data());
}
#else
int main(int argc, char* argv[]) { return run_cli(argc, argv); }
#endif
