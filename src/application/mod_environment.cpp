#include "ddse/application/mod_environment.hpp"

#include "ddse/application/save_profile.hpp"
#include "ddse/core/dson/dson_reader.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace ddse::application {
namespace {

using Json = nlohmann::json;

std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string decode_xml_entities(std::string_view input) {
    std::string output;
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] != '&') { output.push_back(input[i++]); continue; }
        const auto end = input.find(';', i + 1);
        if (end == std::string_view::npos) { output.push_back(input[i++]); continue; }
        const auto entity = input.substr(i + 1, end - i - 1);
        if (entity == "amp") output.push_back('&');
        else if (entity == "lt") output.push_back('<');
        else if (entity == "gt") output.push_back('>');
        else if (entity == "quot") output.push_back('"');
        else if (entity == "apos") output.push_back('\'');
        else output.append(input.substr(i, end - i + 1));
        i = end + 1;
    }
    return output;
}

std::string xml_element(std::string_view xml, std::string_view element) {
    std::string opening = "<" + std::string{element} + ">";
    std::string closing = "</" + std::string{element} + ">";
    const auto start = xml.find(opening);
    if (start == std::string_view::npos) return {};
    const auto value_start = start + opening.size();
    const auto end = xml.find(closing, value_start);
    if (end == std::string_view::npos) return {};
    auto value = xml.substr(value_start, end - value_start);
    if (value.starts_with("<![CDATA[") && value.ends_with("]]>") && value.size() >= 12)
        value = value.substr(9, value.size() - 12);
    return trim(decode_xml_entities(value));
}

struct ProjectMetadata {
    std::string title;
    std::string published_file_id;
    std::string version;
};

ProjectMetadata parse_project_xml(std::string_view xml) {
    ProjectMetadata metadata;
    metadata.title = xml_element(xml, "Title");
    metadata.published_file_id = xml_element(xml, "PublishedFileId");
    const auto major = xml_element(xml, "VersionMajor");
    const auto minor = xml_element(xml, "VersionMinor");
    if (!major.empty() || !minor.empty()) metadata.version = major + "." + (minor.empty() ? "0" : minor);
    return metadata;
}

std::string entry_identity(std::string_view provider, std::string_view external_id,
                           std::string_view name) {
    if (lower_ascii(std::string{provider}) == "steam" && !external_id.empty())
        return "steam:" + std::string{external_id};
    if (!name.empty()) return "local-name:" + lower_ascii(std::string{name});
    return "unresolved:" + std::string{provider} + ":" + std::string{external_id};
}

std::string json_string(const Json& value, std::string_view key) {
    const auto found = value.find(std::string{key});
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

core::Result<std::vector<ModOrderEntry>, core::Error>
parse_manager_order(std::string_view bytes, std::string_view source_path) {
    try {
        const auto document = Json::parse(bytes);
        if (!document.is_object() || !document.contains("mods") || !document["mods"].is_array())
            return core::Result<std::vector<ModOrderEntry>, core::Error>::failure(
                {core::ErrorCode::ContentParseFailed, "Mod order JSON must contain a mods array", "ModEnvironmentScanner",
                 {{"path", std::string{source_path}}}});

        std::vector<ModOrderEntry> entries;
        std::size_t active_index = 0;
        std::set<std::string, std::less<>> seen_identities;
        for (std::size_t i = 0; i < document["mods"].size(); ++i) {
            const auto& item = document["mods"][i];
            if (!item.is_object() || !item.contains("enabled") || !item["enabled"].is_boolean())
                return core::Result<std::vector<ModOrderEntry>, core::Error>::failure(
                    {core::ErrorCode::ContentParseFailed, "Each mod order entry must have a boolean enabled field",
                     "ModEnvironmentScanner", {{"path", std::string{source_path}}, {"index", std::to_string(i)}}});

            ModOrderEntry entry;
            entry.source = ModOrderSource::ManagerExport;
            entry.position = i;
            entry.enabled = item["enabled"].get<bool>();
            entry.provider_id = json_string(item, "provider_id");
            entry.external_id = json_string(item, "external_id");
            entry.mod_guid = json_string(item, "mod_guid");
            entry.name = json_string(item, "name");
            entry.version = json_string(item, "version");
            entry.identity = entry_identity(entry.provider_id, entry.external_id, entry.name);
            if (!seen_identities.insert(entry.identity).second)
                return core::Result<std::vector<ModOrderEntry>, core::Error>::failure(
                    {core::ErrorCode::ValidationFailed, "Mod order JSON contains a duplicate mod identity",
                     "ModEnvironmentScanner", {{"path", std::string{source_path}}, {"identity", entry.identity}}});
            if (entry.enabled) entry.active_order = active_index++;
            entries.push_back(std::move(entry));
        }
        return core::Result<std::vector<ModOrderEntry>, core::Error>::success(std::move(entries));
    } catch (const Json::exception& error) {
        return core::Result<std::vector<ModOrderEntry>, core::Error>::failure(
            {core::ErrorCode::ContentParseFailed, error.what(), "ModEnvironmentScanner",
             {{"path", std::string{source_path}}}});
    }
}

const core::dson::DsonField* find_field(const core::dson::DsonDocument& document,
                                        std::string_view path) {
    const auto found = std::find_if(document.fields.begin(), document.fields.end(), [&](const auto& field) {
        return field.path == path;
    });
    return found == document.fields.end() ? nullptr : &*found;
}

const core::dson::DsonField* find_child(const core::dson::DsonDocument& document,
                                        std::size_t parent, std::string_view name) {
    for (const auto index : document.fields[parent].children)
        if (document.fields[index].name == name) return &document.fields[index];
    return nullptr;
}

std::string dson_string_child(const core::dson::DsonDocument& document,
                              std::size_t parent, std::string_view name) {
    const auto* field = find_child(document, parent, name);
    if (!field) return {};
    const auto* value = std::get_if<std::string>(&field->value);
    return value ? *value : std::string{};
}

core::Result<std::vector<ModOrderEntry>, core::Error>
extract_save_order(const RawSaveProfile& profile) {
    const auto game = profile.documents.find("persist.game.json");
    if (game == profile.documents.end() || !game->second.decoded)
        return core::Result<std::vector<ModOrderEntry>, core::Error>::failure(
            {core::ErrorCode::DsonMalformed, "Save profile has no decoded persist.game.json", "ModEnvironmentScanner",
             {{"profile", profile.descriptor.root_path.string()}}});

    const auto& document = *game->second.decoded;
    const auto* list = find_field(document, "base_root/applied_ugcs_1_0");
    if (!list || list->kind != core::dson::ValueKind::Object)
        return core::Result<std::vector<ModOrderEntry>, core::Error>::failure(
            {core::ErrorCode::DsonMalformed, "Save has no applied_ugcs_1_0 mod order list", "ModEnvironmentScanner",
             {{"document", game->second.path.string()}}});

    std::vector<ModOrderEntry> entries;
    std::size_t active_index = 0;
    std::set<std::string, std::less<>> seen_identities;
    for (std::size_t i = 0; i < list->children.size(); ++i) {
        const auto row_index = list->children[i];
        auto name = dson_string_child(document, row_index, "name");
        auto source = dson_string_child(document, row_index, "source");
        if (name.empty() || source.empty())
            return core::Result<std::vector<ModOrderEntry>, core::Error>::failure(
                {core::ErrorCode::DsonMalformed, "Save mod order entry is missing name or source",
                 "ModEnvironmentScanner", {{"document", game->second.path.string()}, {"index", std::to_string(i)}}});

        ModOrderEntry entry;
        entry.source = ModOrderSource::SaveProfile;
        entry.position = i;
        entry.active_order = active_index++;
        entry.enabled = true;
        entry.name = std::move(name);
        if (source == "Steam") {
            entry.provider_id = "steam";
            entry.external_id = entry.name;
        } else {
            entry.provider_id = source == "mod_local_source" ? "local" : source;
        }
        entry.identity = entry_identity(entry.provider_id, entry.external_id, entry.name);
        if (!seen_identities.insert(entry.identity).second)
            return core::Result<std::vector<ModOrderEntry>, core::Error>::failure(
                {core::ErrorCode::ValidationFailed, "Save mod order contains a duplicate identity",
                 "ModEnvironmentScanner", {{"identity", entry.identity}, {"index", std::to_string(i)}}});
        entries.push_back(std::move(entry));
    }
    return core::Result<std::vector<ModOrderEntry>, core::Error>::success(std::move(entries));
}

std::string stable_local_id(std::size_t root_index, std::string_view folder_name,
                            const ProjectMetadata& metadata) {
    if (!metadata.published_file_id.empty()) return "local-workshop:" + metadata.published_file_id;
    return "local-path:" + std::to_string(root_index) + ":" + std::string{folder_name};
}

core::Result<void, core::Error> add_directory_mods(const IFileSystem& file_system,
                                                   const std::filesystem::path& root,
                                                   bool workshop, std::size_t local_root_index,
                                                   std::vector<ModSourceRecord>& mods,
                                                   std::vector<ModScanDiagnostic>& diagnostics) {
    auto exists = file_system.exists(root);
    if (!exists) return core::Result<void, core::Error>::failure(exists.error());
    if (!exists.value())
        return core::Result<void, core::Error>::failure(
            {core::ErrorCode::FileNotFound, "Configured mod root does not exist", "ModEnvironmentScanner",
             {{"path", root.string()}}});
    auto directories = file_system.list_directories(root);
    if (!directories) return core::Result<void, core::Error>::failure(directories.error());
    for (const auto& directory : directories.value()) {
        const auto folder = path_utf8(directory.filename());
        const auto metadata_path = directory / "project.xml";
        ProjectMetadata metadata;
        auto has_metadata = file_system.exists(metadata_path);
        if (!has_metadata) return core::Result<void, core::Error>::failure(has_metadata.error());
        if (has_metadata.value()) {
            auto bytes = file_system.read_file(metadata_path);
            if (!bytes) diagnostics.push_back({{}, path_utf8(metadata_path), bytes.error().message});
            else metadata = parse_project_xml(bytes.value());
        } else {
            diagnostics.push_back({{}, path_utf8(metadata_path), "Mod directory has no project.xml metadata"});
        }

        ModSourceRecord mod;
        mod.provider_id = workshop ? "steam" : "local";
        mod.external_id = workshop ? folder : metadata.published_file_id;
        mod.display_name = metadata.title.empty() ? folder : metadata.title;
        mod.version = metadata.version;
        mod.root_path = directory;
        if (workshop) {
            mod.id = "steam:" + folder;
            if (!metadata.published_file_id.empty() && metadata.published_file_id != folder)
                diagnostics.push_back({mod.id, "project.xml",
                    "PublishedFileId does not match the Workshop directory name"});
        } else {
            mod.id = stable_local_id(local_root_index, folder, metadata);
        }
        mods.push_back(std::move(mod));
    }
    return core::Result<void, core::Error>::success();
}

ModSourceRecord* find_installed_mod(std::vector<ModSourceRecord>& mods,
                                    const ModOrderEntry& entry,
                                    std::vector<ModScanDiagnostic>& diagnostics) {
    std::vector<ModSourceRecord*> matches;
    if (entry.provider_id == "steam") {
        for (auto& mod : mods)
            if (mod.provider_id == "steam" && mod.external_id == entry.external_id) matches.push_back(&mod);
    } else {
        for (auto& mod : mods)
            if (mod.provider_id == "local" && lower_ascii(mod.display_name) == lower_ascii(entry.name))
                matches.push_back(&mod);
    }
    if (matches.size() > 1) {
        diagnostics.push_back({{}, entry.identity, "Load order entry matches multiple installed mod directories"});
        return nullptr;
    }
    return matches.empty() ? nullptr : matches.front();
}

void compare_orders(ModEnvironmentScanResult& result) {
    auto active_identities = [](const std::vector<ModOrderEntry>& entries) {
        std::vector<std::string> values;
        for (const auto& entry : entries) if (entry.enabled) values.push_back(entry.identity);
        return values;
    };
    const auto save = active_identities(result.save_order);
    const auto manager = active_identities(result.manager_order);
    const std::set<std::string, std::less<>> save_set(save.begin(), save.end());
    const std::set<std::string, std::less<>> manager_set(manager.begin(), manager.end());
    for (const auto& identity : save)
        if (!manager_set.contains(identity)) result.comparison.only_in_save.push_back(identity);
    for (const auto& identity : manager)
        if (!save_set.contains(identity)) result.comparison.only_in_manager_export.push_back(identity);

    result.comparison.exact_enabled_order_match = save == manager;
    std::vector<std::string> shared_save;
    std::vector<std::string> shared_manager;
    for (const auto& identity : save) if (manager_set.contains(identity)) shared_save.push_back(identity);
    for (const auto& identity : manager) if (save_set.contains(identity)) shared_manager.push_back(identity);
    result.comparison.shared_enabled_order_match = shared_save == shared_manager;
    if (!result.comparison.exact_enabled_order_match) {
        for (const auto& identity : result.comparison.only_in_save)
            result.diagnostics.push_back({{}, identity, "Enabled in save order but absent from manager export"});
        for (const auto& identity : result.comparison.only_in_manager_export)
            result.diagnostics.push_back({{}, identity, "Enabled in manager export but absent from save order"});
        if (!result.comparison.shared_enabled_order_match)
            result.diagnostics.push_back({{}, {}, "Shared mods have a different relative load order in save and manager export"});
    }
}

void remap_content_source(BaseContentScanResult& destination, BaseContentScanResult source,
                          const std::string& mod_id) {
    for (auto& row : source.source_files) { row.source_id = mod_id; destination.source_files.push_back(std::move(row)); }
    for (auto& row : source.definitions) { row.source_id = mod_id; destination.definitions.push_back(std::move(row)); }
    for (auto& row : source.localizations) { row.source_id = mod_id; destination.localizations.push_back(std::move(row)); }
    for (auto& row : source.assets) { row.source_id = mod_id; destination.assets.push_back(std::move(row)); }
    for (auto& row : source.asset_references) { row.source_id = mod_id; destination.asset_references.push_back(std::move(row)); }
    for (auto& row : source.relationships) { row.source_id = mod_id; destination.relationships.push_back(std::move(row)); }
    for (auto& row : source.diagnostics) { row.source_id = mod_id; destination.diagnostics.push_back(std::move(row)); }
}

} // namespace

core::Result<ModEnvironmentScanResult, core::Error>
ModEnvironmentScanner::scan(const ModEnvironmentScanConfig& config) const {
    ModEnvironmentScanResult result;
    result.protected_roots.push_back(config.workshop_root);
    result.protected_roots.insert(result.protected_roots.end(), config.local_mod_roots.begin(), config.local_mod_roots.end());
    if (!config.save_profile_root.empty()) result.protected_roots.push_back(config.save_profile_root);
    if (!config.manager_order_json.empty()) result.protected_roots.push_back(config.manager_order_json);
    if (!config.base_content_database.empty()) result.protected_roots.push_back(config.base_content_database);

    auto workshop = add_directory_mods(file_system_, config.workshop_root, true, 0,
                                       result.mods, result.diagnostics);
    if (!workshop) return core::Result<ModEnvironmentScanResult, core::Error>::failure(workshop.error());
    for (std::size_t i = 0; i < config.local_mod_roots.size(); ++i) {
        auto local = add_directory_mods(file_system_, config.local_mod_roots[i], false, i,
                                        result.mods, result.diagnostics);
        if (!local) return core::Result<ModEnvironmentScanResult, core::Error>::failure(local.error());
    }

    if (!config.manager_order_json.empty()) {
        auto exists = file_system_.exists(config.manager_order_json);
        if (!exists) return core::Result<ModEnvironmentScanResult, core::Error>::failure(exists.error());
        if (!exists.value()) return core::Result<ModEnvironmentScanResult, core::Error>::failure(
            {core::ErrorCode::FileNotFound, "Configured mod manager export does not exist", "ModEnvironmentScanner",
             {{"path", path_utf8(config.manager_order_json)}}});
        auto bytes = file_system_.read_file(config.manager_order_json);
        if (!bytes) return core::Result<ModEnvironmentScanResult, core::Error>::failure(bytes.error());
        auto parsed = parse_manager_order(bytes.value(), path_utf8(config.manager_order_json));
        if (!parsed) return core::Result<ModEnvironmentScanResult, core::Error>::failure(parsed.error());
        result.manager_order = std::move(parsed.value());
        result.comparison.manager_export_available = true;
    }

    if (!config.save_profile_root.empty()) {
        SaveProfileDiscovery discovery{file_system_};
        auto profile = discovery.load(config.save_profile_root);
        if (!profile) return core::Result<ModEnvironmentScanResult, core::Error>::failure(profile.error());
        auto parsed = extract_save_order(profile.value());
        if (!parsed) return core::Result<ModEnvironmentScanResult, core::Error>::failure(parsed.error());
        result.save_order = std::move(parsed.value());
        result.comparison.save_available = true;
    }

    if (result.comparison.save_available && result.comparison.manager_export_available)
        compare_orders(result);

    std::map<std::string, const ModOrderEntry*, std::less<>> manager_local_by_name;
    for (const auto& entry : result.manager_order)
        if (entry.provider_id != "steam" && !entry.name.empty())
            manager_local_by_name.try_emplace(lower_ascii(entry.name), &entry);
    for (auto& mod : result.mods) {
        if (mod.provider_id != "local") continue;
        const auto found = manager_local_by_name.find(lower_ascii(mod.display_name));
        if (found == manager_local_by_name.end()) continue;
        mod.mod_guid = found->second->mod_guid;
        if (!found->second->version.empty()) mod.version = found->second->version;
    }
    for (auto& mod : result.mods) {
        const auto found = std::find_if(result.manager_order.begin(), result.manager_order.end(), [&](const auto& entry) {
            return entry.provider_id == "steam" && entry.external_id == mod.external_id;
        });
        if (found != result.manager_order.end()) {
            mod.mod_guid = found->mod_guid;
            if (!found->version.empty()) mod.version = found->version;
        }
    }

    std::vector<ModOrderEntry>* chosen_order = nullptr;
    if (config.prefer_manager_export && result.comparison.manager_export_available) {
        chosen_order = &result.manager_order;
        result.effective_order_source = "manager_export";
    } else if (result.comparison.save_available) {
        chosen_order = &result.save_order;
        result.effective_order_source = "save_profile";
    } else if (result.comparison.manager_export_available) {
        chosen_order = &result.manager_order;
        result.effective_order_source = "manager_export";
    }

    if (chosen_order) {
        std::size_t active_order = 0;
        for (auto& entry : *chosen_order) {
            if (!entry.enabled) continue;
            auto* mod = find_installed_mod(result.mods, entry, result.diagnostics);
            if (!mod) {
                result.diagnostics.push_back({{}, entry.identity, "Enabled mod is not installed in configured roots"});
                continue;
            }
            if (mod->enabled) {
                result.diagnostics.push_back({mod->id, entry.identity, "Mod appears more than once in effective order"});
                continue;
            }
            mod->enabled = true;
            mod->active_order = active_order++;
            mod->order_source = result.effective_order_source;
            entry.matched_mod_id = mod->id;
            result.effective_order.push_back(mod->id);
        }
    } else {
        result.diagnostics.push_back({{}, {}, "No load order evidence supplied; all discovered mods remain disabled"});
    }

    std::sort(result.mods.begin(), result.mods.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    BaseContentScanner content_scanner{file_system_};
    for (const auto& mod : result.mods) {
        auto directories = file_system_.list_directories(mod.root_path);
        if (!directories) {
            result.diagnostics.push_back({mod.id, {}, directories.error().message});
            continue;
        }
        BaseContentScanConfig content_config = BaseContentScanConfig::defaults(mod.root_path);
        content_config.dlc_directory = "__ddse_mod_content_directory__";
        content_config.vanilla_directories.clear();
        for (const auto& directory : directories.value())
            content_config.vanilla_directories.push_back(path_utf8(directory.filename()));
        auto parsed = content_scanner.scan(content_config);
        if (!parsed) {
            result.diagnostics.push_back({mod.id, {}, parsed.error().message});
            continue;
        }
        remap_content_source(result.content, std::move(parsed.value()), mod.id);
    }

    std::sort(result.content.source_files.begin(), result.content.source_files.end(), [](const auto& a, const auto& b) {
        return std::tie(a.source_id, a.virtual_path) < std::tie(b.source_id, b.virtual_path);
    });
    std::sort(result.content.definitions.begin(), result.content.definitions.end(), [](const auto& a, const auto& b) {
        return std::tie(a.kind, a.id, a.source_id, a.virtual_path, a.display_name, a.localization_key, a.payload_json) <
               std::tie(b.kind, b.id, b.source_id, b.virtual_path, b.display_name, b.localization_key, b.payload_json);
    });
    std::stable_sort(result.content.localizations.begin(), result.content.localizations.end(), [](const auto& a, const auto& b) {
        return std::tie(a.language, a.key, a.source_id, a.virtual_path) <
               std::tie(b.language, b.key, b.source_id, b.virtual_path);
    });
    std::sort(result.content.assets.begin(), result.content.assets.end(), [](const auto& a, const auto& b) {
        return std::tie(a.source_id, a.virtual_path) < std::tie(b.source_id, b.virtual_path);
    });
    std::sort(result.content.asset_references.begin(), result.content.asset_references.end(), [](const auto& a, const auto& b) {
        return std::tie(a.definition_type, a.content_id, a.source_id, a.definition_virtual_path,
                        a.asset_role, a.reference_type, a.virtual_path, a.reference_origin) <
               std::tie(b.definition_type, b.content_id, b.source_id, b.definition_virtual_path,
                        b.asset_role, b.reference_type, b.virtual_path, b.reference_origin);
    });
    std::sort(result.content.relationships.begin(), result.content.relationships.end(), [](const auto& a, const auto& b) {
        return std::tie(a.parent_type, a.parent_id, a.relationship_type, a.child_type, a.child_id,
                        a.source_id, a.virtual_path) <
               std::tie(b.parent_type, b.parent_id, b.relationship_type, b.child_type, b.child_id,
                        b.source_id, b.virtual_path);
    });
    std::sort(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& a, const auto& b) {
        return std::tie(a.mod_id, a.virtual_path, a.message) < std::tie(b.mod_id, b.virtual_path, b.message);
    });
    return core::Result<ModEnvironmentScanResult, core::Error>::success(std::move(result));
}

std::string_view to_string(ModOrderSource source) noexcept {
    switch (source) {
    case ModOrderSource::SaveProfile: return "save_profile";
    case ModOrderSource::ManagerExport: return "manager_export";
    }
    return "unknown";
}

} // namespace ddse::application
