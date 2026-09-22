#include "ddse/application/configuration_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>

namespace ddse::application {
namespace {

using json = nlohmann::json;

core::Error config_error(core::ErrorCode code, std::string message,
                         const std::filesystem::path& path) {
    return {code, std::move(message), "AppConfigurationStore", {{"path", path.string()}}};
}

std::string path_text(const std::filesystem::path& path) {
    return path.generic_string();
}

void read_path(const json& object, const char* key, std::filesystem::path& target) {
    if (object.contains(key) && object.at(key).is_string()) target = object.at(key).get<std::string>();
}

void read_path_array(const json& object, const char* key, std::vector<std::filesystem::path>& target) {
    target.clear();
    if (!object.contains(key) || !object.at(key).is_array()) return;
    for (const auto& item : object.at(key)) {
        if (item.is_string()) target.emplace_back(item.get<std::string>());
    }
}

// A small helper avoids exposing ranges to the JSON library and keeps this
// code compatible with the C++20 toolchains used by CLion.
template <typename Range>
json path_array(const Range& paths) {
    json result = json::array();
    for (const auto& path : paths) result.push_back(path_text(path));
    return result;
}

json configuration_json(const AppConfiguration& value) {
    json result{
        {"gameRoot", path_text(value.game_root)},
        {"backupRoot", path_text(value.backup_root)},
        {"dataRoot", path_text(value.data_root)},
        {"language", value.language},
        {"maxBackupCount", value.max_backup_count},
        {"autoEditSaveEnabled", value.auto_edit_save_enabled},
        {"autoEditSaveIntervalSeconds", value.auto_edit_save_interval_seconds},
    };
    result["workshopRoots"] = path_array(value.workshop_roots);
    result["localModRoots"] = path_array(value.local_mod_roots);
    result["saveRoots"] = path_array(value.save_roots);
    return result;
}

core::Result<AppConfiguration, core::Error> parse_configuration(const std::string& bytes,
                                                                 const std::filesystem::path& path,
                                                                 const AppConfiguration& fallback) {
    try {
        const auto object = json::parse(bytes);
        if (!object.is_object())
            return core::Result<AppConfiguration, core::Error>::failure(
                config_error(core::ErrorCode::InvalidConfiguration, "Configuration root must be an object", path));
        auto result = fallback;
        read_path(object, "gameRoot", result.game_root);
        read_path(object, "backupRoot", result.backup_root);
        read_path(object, "dataRoot", result.data_root);
        read_path_array(object, "workshopRoots", result.workshop_roots);
        read_path_array(object, "localModRoots", result.local_mod_roots);
        read_path_array(object, "saveRoots", result.save_roots);
        if (object.contains("language") && object.at("language").is_string()) result.language = object.at("language").get<std::string>();
        if (object.contains("maxBackupCount") && object.at("maxBackupCount").is_number_unsigned()) result.max_backup_count = object.at("maxBackupCount").get<std::uint32_t>();
        if (object.contains("autoEditSaveEnabled") && object.at("autoEditSaveEnabled").is_boolean()) result.auto_edit_save_enabled = object.at("autoEditSaveEnabled").get<bool>();
        if (object.contains("autoEditSaveIntervalSeconds") && object.at("autoEditSaveIntervalSeconds").is_number_unsigned()) result.auto_edit_save_interval_seconds = object.at("autoEditSaveIntervalSeconds").get<std::uint32_t>();
        if (result.max_backup_count == 0 || result.max_backup_count > 10000 ||
            result.auto_edit_save_interval_seconds == 0 || result.auto_edit_save_interval_seconds > 86400)
            return core::Result<AppConfiguration, core::Error>::failure(
                config_error(core::ErrorCode::InvalidConfiguration, "Backup retention and auto-save interval are out of range", path));
        if (result.backup_root.empty() || result.data_root.empty())
            return core::Result<AppConfiguration, core::Error>::failure(
                config_error(core::ErrorCode::InvalidConfiguration, "dataRoot and backupRoot are required", path));
        return core::Result<AppConfiguration, core::Error>::success(std::move(result));
    } catch (const std::exception& error) {
        return core::Result<AppConfiguration, core::Error>::failure(
            config_error(core::ErrorCode::InvalidConfiguration, error.what(), path));
    }
}

} // namespace

AppConfigurationStore::AppConfigurationStore(IFileSystem& file_system,
                                             std::filesystem::path configuration_file,
                                             std::filesystem::path default_data_root)
    : file_system_(file_system), configuration_file_(std::move(configuration_file)),
      default_data_root_(std::move(default_data_root)), configuration_(defaults(default_data_root_)) {}

AppConfiguration AppConfigurationStore::defaults(const std::filesystem::path& data_root) {
    AppConfiguration result;
    result.data_root = data_root;
    result.backup_root = data_root / "backups";
    return result;
}

core::Result<void, core::Error> AppConfigurationStore::ensure_storage() const {
    auto data = file_system_.create_directories(configuration_.data_root);
    if (!data) return data;
    auto backup = file_system_.create_directories(configuration_.backup_root);
    if (!backup) return backup;
    return file_system_.create_directories(auto_edit_save_directory());
}

core::Result<AppConfiguration, core::Error> AppConfigurationStore::load() const {
    auto exists = file_system_.exists(configuration_file_);
    if (!exists) return core::Result<AppConfiguration, core::Error>::failure(exists.error());
    if (!exists.value()) return core::Result<AppConfiguration, core::Error>::success(defaults(default_data_root_));
    auto bytes = file_system_.read_file(configuration_file_);
    if (!bytes) return core::Result<AppConfiguration, core::Error>::failure(bytes.error());
    return parse_configuration(bytes.value(), configuration_file_, defaults(default_data_root_));
}

core::Result<void, core::Error> AppConfigurationStore::save(const AppConfiguration& configuration) {
    if (configuration.backup_root.empty() || configuration.data_root.empty() || configuration.max_backup_count == 0 ||
        configuration.max_backup_count > 10000 || configuration.auto_edit_save_interval_seconds == 0 ||
        configuration.auto_edit_save_interval_seconds > 86400)
        return core::Result<void, core::Error>::failure(
            config_error(core::ErrorCode::InvalidConfiguration, "Configuration contains an empty path or invalid limit", configuration_file_));
    configuration_ = configuration;
    auto parent = configuration_file_.parent_path();
    if (!parent.empty()) {
        auto made = file_system_.create_directories(parent);
        if (!made) return made;
    }
    auto storage = ensure_storage();
    if (!storage) return storage;
    const auto bytes = configuration_json(configuration_).dump(2) + "\n";
    auto written = file_system_.write_file_atomic(configuration_file_, bytes);
    if (!written) return written;
    initialized_ = true;
    return core::Result<void, core::Error>::success();
}

core::Result<AppConfiguration, core::Error> AppConfigurationStore::initialize() {
    auto loaded = load();
    if (!loaded) return loaded;
    configuration_ = loaded.value();
    auto saved = save(configuration_);
    if (!saved) return core::Result<AppConfiguration, core::Error>::failure(saved.error());
    return core::Result<AppConfiguration, core::Error>::success(configuration_);
}

std::filesystem::path AppConfigurationStore::auto_edit_save_directory() const {
    return configuration_.backup_root / "AutoEditSave";
}

core::Result<RecoveryStatus, core::Error> AppConfigurationStore::recovery_status() const {
    RecoveryStatus result;
    const auto marker = auto_edit_save_directory() / "recovery.json";
    auto exists = file_system_.exists(marker);
    if (!exists) return core::Result<RecoveryStatus, core::Error>::failure(exists.error());
    if (!exists.value()) return core::Result<RecoveryStatus, core::Error>::success(result);
    auto bytes = file_system_.read_file(marker);
    if (!bytes) return core::Result<RecoveryStatus, core::Error>::failure(bytes.error());
    try {
        const auto object = json::parse(bytes.value());
        if (object.value("discarded", false)) return core::Result<RecoveryStatus, core::Error>::success(result);
        result.available = true;
        result.profile_id = object.value("profileId", "");
        result.source_profile = object.value("sourceProfile", "");
        result.source_fingerprint = object.value("sourceFingerprint", std::uint64_t{});
        result.revision = object.value("revision", std::uint64_t{});
        result.saved_at = object.value("savedAt", "");
        result.path = marker.string();
        return core::Result<RecoveryStatus, core::Error>::success(std::move(result));
    } catch (const std::exception& error) {
        return core::Result<RecoveryStatus, core::Error>::failure(
            config_error(core::ErrorCode::InvalidConfiguration, error.what(), marker));
    }
}

core::Result<std::string, core::Error> AppConfigurationStore::recovery_snapshot() const {
    const auto marker = auto_edit_save_directory() / "recovery.json";
    auto bytes = file_system_.read_file(marker);
    if (!bytes) return core::Result<std::string, core::Error>::failure(bytes.error());
    try {
        const auto object = json::parse(bytes.value());
        if (object.value("discarded", false) || !object.contains("snapshot") || !object["snapshot"].is_string())
            return core::Result<std::string, core::Error>::failure(
                config_error(core::ErrorCode::FileNotFound, "No active recovery snapshot is available", marker));
        return core::Result<std::string, core::Error>::success(object["snapshot"].get<std::string>());
    } catch (const std::exception& error) {
        return core::Result<std::string, core::Error>::failure(
            config_error(core::ErrorCode::InvalidConfiguration, error.what(), marker));
    }
}

core::Result<void, core::Error> AppConfigurationStore::write_recovery_marker(
    std::string profile_id, const std::filesystem::path& source_profile,
    std::uint64_t source_fingerprint, std::uint64_t revision, std::string saved_at,
    std::string session_snapshot) {
    auto storage = ensure_storage();
    if (!storage) return storage;
    json marker{
        {"formatVersion", 1}, {"profileId", std::move(profile_id)},
        {"sourceProfile", path_text(source_profile)}, {"sourceFingerprint", source_fingerprint},
        {"revision", revision}, {"savedAt", std::move(saved_at)},
        {"snapshot", std::move(session_snapshot)}, {"discarded", false},
    };
    return file_system_.write_file_atomic(auto_edit_save_directory() / "recovery.json", marker.dump(2) + "\n");
}

core::Result<void, core::Error> AppConfigurationStore::discard_recovery() {
    auto storage = ensure_storage();
    if (!storage) return storage;
    json marker{{"formatVersion", 1}, {"discarded", true}};
    return file_system_.write_file_atomic(auto_edit_save_directory() / "recovery.json", marker.dump(2) + "\n");
}

} // namespace ddse::application
