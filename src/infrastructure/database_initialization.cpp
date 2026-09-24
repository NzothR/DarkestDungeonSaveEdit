#include "ddse/infrastructure/database_initialization.hpp"

#include "ddse/application/content_scanner.hpp"
#include "ddse/application/mod_environment.hpp"
#include "ddse/application/save_profile.hpp"
#include "ddse/infrastructure/base_content_database.hpp"
#include "ddse/infrastructure/mod_environment_database.hpp"
#include "ddse/infrastructure/sqlite/database.hpp"
#include "ddse/infrastructure/sqlite/statement.hpp"

#include <chrono>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <array>

namespace ddse::infrastructure {
namespace {

using Clock = std::chrono::steady_clock;

void set_progress(DatabaseInitializationState& state, std::string phase,
                  std::string work, int progress) {
    state.phase = std::move(phase);
    state.current_work = std::move(work);
    state.progress_percent = progress;
}

std::string error_text(const core::Error& error) {
    return error.module + ": " + error.message;
}

std::filesystem::path database_root_for(const application::AppConfiguration& configuration) {
    std::error_code ec;
    const auto data = std::filesystem::weakly_canonical(configuration.data_root, ec);
    const auto game = std::filesystem::weakly_canonical(configuration.game_root, ec);
    auto it = data.begin();
    auto root_it = game.begin();
    bool inside = !ec;
    for (; inside && root_it != game.end(); ++root_it, ++it) {
        if (it == data.end()) { inside = false; break; }
#ifdef _WIN32
        auto left = it->string();
        auto right = root_it->string();
        std::transform(left.begin(), left.end(), left.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(right.begin(), right.end(), right.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (left != right) inside = false;
#else
        if (*it != *root_it) inside = false;
#endif
    }
    if (inside) return std::filesystem::temp_directory_path() / "ddse-darkest-dungeon";
    return configuration.data_root;
}

bool has_base_schema(const std::filesystem::path& path) {
    auto opened = sqlite::ConnectionFactory{}.open(path);
    if (!opened) return false;
    auto query = opened.value().prepare(
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
        "('content_sources','source_files','content_definitions','localization_entries',"
        "'assets','content_asset_references','content_relationships')");
    if (!query) return false;
    auto statement = std::move(query.value());
    auto row = statement.step();
    if (!row || !row.value() || statement.column_int64(0) != 7) return false;
    auto version = opened.value().prepare("SELECT value FROM schema_info WHERE key='scanner_version'");
    if (!version) return false;
    auto version_statement = std::move(version.value());
    auto version_row = version_statement.step();
    return version_row && version_row.value() && version_statement.column_text(0) == "stage9";
}

bool has_mod_schema(const std::filesystem::path& path) {
    auto opened = sqlite::ConnectionFactory{}.open(path);
    if (!opened) return false;
    auto query = opened.value().prepare(
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
        "('environment_info','mod_sources','mod_order_entries','source_files','assets',"
        "'effective_vfs','effective_assets','effective_definitions')");
    if (!query) return false;
    auto statement = std::move(query.value());
    auto row = statement.step();
    if (!row || !row.value() || statement.column_int64(0) != 8) return false;
    auto version = opened.value().prepare("SELECT value FROM environment_info WHERE key='scanner_version'");
    if (!version) return false;
    auto version_statement = std::move(version.value());
    auto version_row = version_statement.step();
    return version_row && version_row.value() && version_statement.column_text(0) == "stage9";
}

} // namespace

DatabaseInitializationManager::~DatabaseInitializationManager() {
    if (worker_.joinable()) worker_.join();
}

void DatabaseInitializationManager::start(const application::AppConfiguration& configuration, bool force) {
    const auto database_root = database_root_for(configuration);
    const auto base_path = database_root / "base_content.db";
    const auto mod_path = database_root / "mod_environment.db";
    {
        std::lock_guard lock(mutex_);
        if (state_.status == "running") return;
    }
    if (!force && std::filesystem::is_regular_file(base_path) && has_base_schema(base_path) &&
        std::filesystem::is_regular_file(mod_path) && has_mod_schema(mod_path)) {
        if (worker_.joinable()) worker_.join();
        std::lock_guard lock(mutex_);
        state_ = {};
        state_.status = "completed";
        state_.phase = "complete";
        state_.current_work = "Existing databases are ready";
        state_.progress_percent = 100;
        state_.reused_existing = true;
        mod_database_path_ = mod_path;
        base_database_path_ = base_path;
        return;
    }
    {
        std::lock_guard lock(mutex_);
        state_ = {};
        state_.status = "running";
        state_.phase = "queued";
        state_.current_work = "Preparing database initialization";
        state_.progress_percent = 0;
        state_.reused_existing = false;
        mod_database_path_ = mod_path;
        base_database_path_ = base_path;
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, configuration] { run(configuration); });
}

DatabaseInitializationState DatabaseInitializationManager::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

std::filesystem::path DatabaseInitializationManager::mod_database_path() const {
    std::lock_guard lock(mutex_);
    return mod_database_path_;
}

std::filesystem::path DatabaseInitializationManager::base_database_path() const {
    std::lock_guard lock(mutex_);
    return base_database_path_;
}

void DatabaseInitializationManager::run(application::AppConfiguration configuration) {
    const auto total_started = Clock::now();
    const auto database_root = database_root_for(configuration);
    const auto base_path = database_root / "base_content.db";
    const auto mod_path = database_root / "mod_environment.db";
    {
        std::lock_guard lock(mutex_);
        mod_database_path_ = mod_path;
        base_database_path_ = base_path;
        set_progress(state_, "base", "Checking base content database", 5);
    }

    std::error_code ec;
    std::filesystem::create_directories(configuration.data_root, ec);
    if (ec) {
        std::lock_guard lock(mutex_);
        state_.status = "failed";
        state_.phase = "base";
        state_.current_work = "Unable to create database directory";
        state_.diagnostics.push_back(ec.message());
        return;
    }

    const auto base_started = Clock::now();
    bool base_ready = std::filesystem::is_regular_file(base_path, ec);
    if (base_ready) {
        if (!has_base_schema(base_path)) {
            std::filesystem::remove(base_path, ec);
            base_ready = false;
            std::lock_guard lock(mutex_);
            state_.diagnostics.push_back("Existing base database schema was incomplete and will be rebuilt.");
        }
    }
    if (!base_ready) {
        {
            std::lock_guard lock(mutex_);
            set_progress(state_, "base", "Scanning vanilla and DLC content", 15);
        }
        auto scan = application::BaseContentScanner{file_system_}.scan(
            application::BaseContentScanConfig::defaults(configuration.game_root));
        if (!scan) {
            std::lock_guard lock(mutex_);
            state_.status = "failed";
            state_.phase = "base";
            state_.current_work = "Base content scan failed";
            state_.diagnostics.push_back(error_text(scan.error()));
            return;
        }
        {
            std::lock_guard lock(mutex_);
            set_progress(state_, "base", "Writing base content database", 35);
        }
        auto built = BaseContentDatabaseBuilder{}.rebuild(base_path, scan.value());
        if (!built) {
            std::lock_guard lock(mutex_);
            state_.status = "failed";
            state_.phase = "base";
            state_.current_work = "Base content database failed";
            state_.diagnostics.push_back(error_text(built.error()));
            return;
        }
    } else {
        std::lock_guard lock(mutex_);
        set_progress(state_, "base", "Base content database is ready", 35);
    }
    {
        std::lock_guard lock(mutex_);
        state_.base_elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - base_started).count());
        set_progress(state_, "mods", "Scanning enabled Mod environment", 45);
    }

    application::ModEnvironmentScanConfig scan_config;
    scan_config.base_content_database = base_path;
    if (!configuration.workshop_roots.empty()) scan_config.workshop_root = configuration.workshop_roots.front();
    if (!configuration.local_mod_roots.empty()) scan_config.local_mod_roots.push_back(configuration.local_mod_roots.front());
    if (!configuration.save_roots.empty()) scan_config.save_profile_root = configuration.save_roots.front();
    auto scan = application::ModEnvironmentScanner{file_system_}.scan(scan_config);
    application::ModEnvironmentScanResult environment;
    if (scan) {
        environment = std::move(scan.value());
    } else {
        // A missing individual Mod/root must not prevent the rest of the
        // environment from being queryable. Preserve save order and rebuild
        // the database with whatever content could be discovered.
        {
            std::lock_guard lock(mutex_);
            state_.diagnostics.push_back(error_text(scan.error()));
        }
        if (!scan_config.save_profile_root.empty()) {
            auto profile = application::SaveProfileDiscovery{file_system_}.load(scan_config.save_profile_root);
            if (profile) {
                auto order = application::read_save_mod_order(profile.value());
                if (order) {
                    environment.save_order = order.value();
                    environment.effective_order_source = "save_profile";
                    for (const auto& entry : environment.save_order) {
                        if (entry.enabled) environment.effective_order.push_back(entry.identity);
                    }
                    environment.comparison.save_available = true;
                }
            }
        }
    }
    {
        std::lock_guard lock(mutex_);
        set_progress(state_, "mods", "Writing Mod environment database", 75);
    }
    auto built = ModEnvironmentDatabaseBuilder{}.rebuild(mod_path, base_path, environment);
    if (!built) {
        std::lock_guard lock(mutex_);
        state_.status = "failed";
        state_.phase = "mods";
        state_.current_work = "Mod environment database failed";
        state_.diagnostics.push_back(error_text(built.error()));
        return;
    }
    const auto mod_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - base_started).count();
    {
        std::lock_guard lock(mutex_);
        state_.mod_elapsed_ms = static_cast<std::uint64_t>(mod_elapsed);
        state_.installed_mods = built.value().installed_mods;
        state_.enabled_mods = built.value().enabled_mods;
        if (state_.enabled_mods == 0) {
            for (const auto& entry : environment.save_order)
                if (entry.enabled) ++state_.enabled_mods;
        }
        state_.status = "completed";
        state_.phase = "complete";
        state_.current_work = "Database initialization completed";
        state_.progress_percent = 100;
        state_.total_elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - total_started).count());
        if (built.value().diagnostics != 0)
            state_.diagnostics.push_back("Some Mods could not be fully indexed; other Mods remain available.");
    }
}

} // namespace ddse::infrastructure
