#include "ddse/infrastructure/http_drogon_server.hpp"

#include "ddse/application/save_profile.hpp"
#include "ddse/application/save_commit.hpp"
#include "ddse/application/campaign_model_builder.hpp"
#include "ddse/application/campaign_edit_session.hpp"
#include "ddse/application/hero_template.hpp"
#include "ddse/core/dson/dson_document.hpp"
#include "ddse/application/mod_environment.hpp"
#include "ddse/infrastructure/database_initialization.hpp"
#include "ddse/infrastructure/database_mod_query.hpp"
#include "ddse/infrastructure/sqlite_content_environment.hpp"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <mutex>
#include <memory>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <spawn.h>
#include <sys/socket.h>
#include <unistd.h>
extern "C" char** environ;
#endif

namespace ddse::infrastructure {
namespace {

#ifdef _WIN32
std::optional<std::filesystem::path> choose_directory(std::string_view title);
#endif

constexpr std::string_view kSessionCookie = "ddse_session";
constexpr std::string_view kClientHeader = "X-DDSE-Request";
constexpr std::string_view kContentSecurityPolicy =
    "default-src 'self'; connect-src 'self'; img-src 'self' data: blob:; "
    "style-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'; "
    "frame-ancestors 'none'";

struct ServerContext {
    ServerContext(std::filesystem::path root, std::string token,
                  const application::ApplicationStatusService& status,
                  application::AppConfigurationStore& store,
                  application::IFileSystem& files)
        : web_root(std::move(root)), session_token(std::move(token)),
          status_service(status), configuration_store(store), file_system(files),
          initialization(files) {}
    void invalidate_campaign() {
        std::lock_guard lock(campaign_mutex);
        campaign.reset();
    }
    std::filesystem::path web_root;
    std::string session_token;
    std::string origin;
    std::string host;
    const application::ApplicationStatusService& status_service;
    application::AppConfigurationStore& configuration_store;
    application::IFileSystem& file_system;
    DatabaseInitializationManager initialization;
    struct BuildingUpgradeTree {
        std::string building_id;
        std::string id;
        std::vector<char> codes;
        std::vector<std::string> descriptions;
    };
    struct CampaignSession {
        application::RawSaveProfile profile;
        std::unique_ptr<SqliteContentEnvironment> content;
        std::unique_ptr<application::CampaignEditSession> edits;
        std::filesystem::path mod_database_path;
        std::set<std::string, std::less<>> reserved_trinket_keys;
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>> reserved_hero_trinket_keys;
        std::uint32_t hero_trinket_slot_limit{0};
        std::uint32_t hero_positive_quirk_limit{0};
        std::uint32_t hero_negative_quirk_limit{0};
        std::vector<application::ContentDefinition> quirk_definitions;
        std::vector<Json::Value> cached_quirk_catalog;
        std::map<std::string, nlohmann::json, std::less<>> cached_buff_definitions;
        mutable std::optional<std::vector<DatabaseModRecord>> cached_mod_records;
        mutable std::map<std::string, Json::Value, std::less<>> cached_trinket_details;
        std::vector<Json::Value> cached_trinket_catalog;
        std::vector<Json::Value> cached_hero_catalog;
        std::map<std::string, std::string, std::less<>> cached_combat_skill_icons;
        std::map<std::string, Json::Value, std::less<>> cached_hero_equipment;
        std::map<std::string, std::map<std::string, std::int32_t, std::less<>>, std::less<>> cached_hero_upgrade_max;
        std::vector<std::int32_t> resolve_level_thresholds;
        std::uint64_t next_hero_guid{1};
        std::vector<application::ContentDefinition> trinket_definitions;
        std::vector<BuildingUpgradeTree> building_upgrade_trees;
        std::vector<std::string> official_district_ids;
    };
    std::mutex campaign_mutex;
    std::unique_ptr<CampaignSession> campaign;
};

std::string make_session_token() {
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    std::string token;
    token.reserve(64);
    for (int i = 0; i < 32; ++i) {
        const auto value = static_cast<unsigned int>(random()) & 0xffU;
        token.push_back(digits[value >> 4U]);
        token.push_back(digits[value & 0x0fU]);
    }
    return token;
}

bool constant_time_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left[i] ^ right[i]);
    return difference == 0;
}

bool has_expected_origin(const drogon::HttpRequestPtr& request,
                         const ServerContext& context) {
    const auto& origin = request->getHeader("Origin");
    return origin.empty() || origin == context.origin;
}

bool has_session_cookie(const drogon::HttpRequestPtr& request,
                        const ServerContext& context) {
    return constant_time_equal(request->getCookie(std::string{kSessionCookie}),
                               context.session_token);
}

bool has_expected_host(const drogon::HttpRequestPtr& request,
                       const ServerContext& context) {
    return request->getHeader("Host") == context.host;
}

drogon::HttpResponsePtr json_error(drogon::HttpStatusCode status,
                                   std::string_view code,
                                   std::string_view message) {
    Json::Value body(Json::objectValue);
    body["ok"] = false;
    Json::Value error(Json::objectValue);
    error["code"] = std::string{code};
    error["message"] = std::string{message};
    error["context"] = Json::Value(Json::objectValue);
    body["error"] = std::move(error);
    body["diagnostics"] = Json::Value(Json::arrayValue);
    auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(body));
    response->setStatusCode(status);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("X-Content-Type-Options", "nosniff");
    return response;
}

drogon::HttpResponsePtr forbidden_response(std::string_view code,
                                           std::string_view message) {
    return json_error(drogon::k403Forbidden, code, message);
}

drogon::HttpResponsePtr serve_index(const drogon::HttpRequestPtr& request,
                                    const std::shared_ptr<ServerContext>& context) {
    if (!has_expected_host(request, *context) ||
        !has_expected_origin(request, *context)) {
        return forbidden_response("LOCAL_ORIGIN_REJECTED",
                                  "The local editor only accepts its own origin.");
    }

    const auto index_path = context->web_root / "index.html";
    std::ifstream input(index_path, std::ios::binary);
    if (!input) {
        return json_error(drogon::k500InternalServerError, "FRONTEND_NOT_FOUND",
                          "The bundled frontend could not be loaded.");
    }
    const std::string html{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
    auto response = drogon::HttpResponse::newHttpResponse(
        drogon::k200OK, drogon::CT_TEXT_HTML);
    response->setBody(html);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Content-Security-Policy",
                        std::string{kContentSecurityPolicy});
    response->addHeader("Referrer-Policy", "no-referrer");
    response->addHeader("X-Content-Type-Options", "nosniff");

    drogon::Cookie session{std::string{kSessionCookie}, context->session_token};
    session.setPath("/");
    session.setHttpOnly(true);
    session.setSameSite(drogon::Cookie::SameSite::kStrict);
    response->addCookie(std::move(session));
    return response;
}

void handle_status(const drogon::HttpRequestPtr& request,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                   const std::shared_ptr<ServerContext>& context) {
    if (!has_expected_host(request, *context) ||
        !has_expected_origin(request, *context)) {
        callback(forbidden_response("LOCAL_ORIGIN_REJECTED",
                                    "The local editor only accepts its own origin."));
        return;
    }
    if (request->getHeader(std::string{kClientHeader}) != "1" ||
        !has_session_cookie(request, *context)) {
        callback(forbidden_response("LOCAL_SESSION_REQUIRED",
                                    "Open the editor page before calling the local API."));
        return;
    }

    const auto status = context->status_service.get_status();
    Json::Value value(Json::objectValue);
    value["apiVersion"] = static_cast<Json::UInt>(status.api_version);
    value["application"] = Json::Value(Json::objectValue);
    value["application"]["name"] = status.application_name;
    value["application"]["version"] = status.application_version;
    value["state"] = status.state;
    value["configuration"] = Json::Value(Json::objectValue);
    value["configuration"]["state"] = status.configuration_state;
    value["configuration"]["backupRoot"] = status.backup_root;
    value["configuration"]["autoEditSaveEnabled"] = status.auto_edit_save_enabled;
    value["configuration"]["autoEditSaveIntervalSeconds"] = status.auto_edit_save_interval_seconds;
    value["configuration"]["recoveryAvailable"] = status.recovery_available;

    Json::Value body(Json::objectValue);
    body["ok"] = true;
    body["value"] = std::move(value);
    body["diagnostics"] = Json::Value(Json::arrayValue);
    auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(body));
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("X-Content-Type-Options", "nosniff");
    callback(response);
}

bool authorized_api_request(const drogon::HttpRequestPtr& request,
                            const ServerContext& context,
                            std::function<void(const drogon::HttpResponsePtr&)>& callback) {
    if (!has_expected_host(request, context) || !has_expected_origin(request, context)) {
        callback(forbidden_response("LOCAL_ORIGIN_REJECTED",
                                    "The local editor only accepts its own origin."));
        return false;
    }
    if (request->getHeader(std::string{kClientHeader}) != "1" ||
        !has_session_cookie(request, context)) {
        callback(forbidden_response("LOCAL_SESSION_REQUIRED",
                                    "Open the editor page before calling the local API."));
        return false;
    }
    return true;
}

Json::Value configuration_value(const application::AppConfiguration& config) {
    Json::Value value(Json::objectValue);
    value["gameRoot"] = config.game_root.string();
    value["backupRoot"] = config.backup_root.string();
    value["dataRoot"] = config.data_root.string();
    value["language"] = config.language;
    value["maxBackupCount"] = static_cast<Json::UInt>(config.max_backup_count);
    value["autoEditSaveEnabled"] = config.auto_edit_save_enabled;
    value["autoEditSaveIntervalSeconds"] = static_cast<Json::UInt>(config.auto_edit_save_interval_seconds);
    value["heroTrinketSlotLimit"] = config.hero_trinket_slot_limit
        ? Json::Value(*config.hero_trinket_slot_limit) : Json::Value(Json::nullValue);
    value["heroPositiveQuirkLimit"] = config.hero_positive_quirk_limit
        ? Json::Value(*config.hero_positive_quirk_limit) : Json::Value(Json::nullValue);
    value["heroNegativeQuirkLimit"] = config.hero_negative_quirk_limit
        ? Json::Value(*config.hero_negative_quirk_limit) : Json::Value(Json::nullValue);
    Json::Value workshop(Json::arrayValue);
    for (const auto& path : config.workshop_roots) workshop.append(path.string());
    value["workshopRoots"] = std::move(workshop);
    Json::Value local(Json::arrayValue);
    for (const auto& path : config.local_mod_roots) local.append(path.string());
    value["localModRoots"] = std::move(local);
    Json::Value saves(Json::arrayValue);
    for (const auto& path : config.save_roots) saves.append(path.string());
    value["saveRoots"] = std::move(saves);
    return value;
}

drogon::HttpResponsePtr json_ok(Json::Value value) {
    Json::Value body(Json::objectValue);
    body["ok"] = true;
    body["value"] = std::move(value);
    body["diagnostics"] = Json::Value(Json::arrayValue);
    auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(body));
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("X-Content-Type-Options", "nosniff");
    return response;
}

std::vector<ServerContext::BuildingUpgradeTree> load_building_upgrade_trees(
    application::IContentEnvironment& content, application::IFileSystem& file_system);
std::vector<std::string> load_official_district_ids(application::IFileSystem& file_system,
                                                    const std::filesystem::path& game_root);
void warm_trinket_catalog(ServerContext& context, ServerContext::CampaignSession& campaign);
void warm_quirk_catalog(ServerContext::CampaignSession& campaign);
void warm_hero_catalog(ServerContext& context, ServerContext::CampaignSession& campaign);

std::optional<core::Error> ensure_campaign_locked(ServerContext& context) {
    const auto& configuration = context.configuration_store.current();
    if (configuration.save_roots.empty() || configuration.save_roots.front().empty())
        return core::Error{core::ErrorCode::InvalidConfiguration,
                           "A save profile must be selected before opening the town editor.",
                           "CampaignSession"};
    const auto initialization = context.initialization.state();
    if (initialization.status != "completed")
        return core::Error{core::ErrorCode::DatabaseError,
                           "The content databases are not ready yet.", "CampaignSession"};
    const auto save_root = configuration.save_roots.front();
    if (context.campaign && context.campaign->profile.descriptor.root_path == save_root)
        return std::nullopt;

    auto profile = application::SaveProfileDiscovery{context.file_system}.load(save_root);
    if (!profile) return profile.error();
    if (profile.value().status != application::ProfileReadStatus::Complete)
        return core::Error{core::ErrorCode::InvalidConfiguration,
                           "The selected save profile is not structurally complete.",
                           "CampaignSession"};

    infrastructure::SqliteContentEnvironmentConfig environment_config;
    environment_config.base_content_database = context.initialization.base_database_path();
    environment_config.mod_environment_database = context.initialization.mod_database_path();
    environment_config.selection.language = context.configuration_store.current().language == "zh_cn"
        ? "schinese" : "english";
    environment_config.selection.fallback_language = "english";
    const auto dlc_directories = context.file_system.list_directories(
        context.configuration_store.current().game_root / "dlc");
    if (dlc_directories) {
        for (const auto& directory : dlc_directories.value())
            environment_config.selection.enabled_dlc_sources.push_back(
                "dlc:" + directory.filename().string());
    }
    auto content = std::make_unique<SqliteContentEnvironment>(std::move(environment_config));
    auto model = application::CampaignModelBuilder{}.build(profile.value(), *content);
    if (!configuration.hero_trinket_slot_limit || !configuration.hero_positive_quirk_limit ||
        !configuration.hero_negative_quirk_limit) {
        auto initialized = configuration;
        std::size_t trinkets = 0, positive = 0, negative = 0;
        for (const auto& hero : model.heroes) {
            trinkets = std::max(trinkets, hero.trinkets.size());
            positive = std::max(positive, static_cast<std::size_t>(std::count_if(hero.quirks.begin(), hero.quirks.end(),
                [](const auto& quirk) { return !quirk.is_disease && quirk.polarity == domain::QuirkPolarity::Positive; })));
            negative = std::max(negative, static_cast<std::size_t>(std::count_if(hero.quirks.begin(), hero.quirks.end(),
                [](const auto& quirk) { return !quirk.is_disease && quirk.polarity == domain::QuirkPolarity::Negative; })));
        }
        if (!initialized.hero_trinket_slot_limit) initialized.hero_trinket_slot_limit = static_cast<std::uint32_t>(trinkets);
        if (!initialized.hero_positive_quirk_limit) initialized.hero_positive_quirk_limit = static_cast<std::uint32_t>(positive);
        if (!initialized.hero_negative_quirk_limit) initialized.hero_negative_quirk_limit = static_cast<std::uint32_t>(negative);
        if (const auto saved = context.configuration_store.save(initialized); !saved) return saved.error();
    }

    auto session = std::make_unique<ServerContext::CampaignSession>();
    session->building_upgrade_trees = load_building_upgrade_trees(*content, context.file_system);
    session->official_district_ids = load_official_district_ids(context.file_system,
        context.configuration_store.current().game_root);
    session->profile = std::move(profile.value());
    if (const auto roster = session->profile.documents.find("persist.roster.json");
        roster != session->profile.documents.end() && roster->second.decoded) {
        for (const auto& field : roster->second.decoded->fields) {
            if (field.path == "base_root/nextGuid") {
                if (const auto* value = std::get_if<std::int32_t>(&field.value); value && *value > 0)
                    session->next_hero_guid = static_cast<std::uint64_t>(*value);
                break;
            }
        }
    }
    session->mod_database_path = context.initialization.mod_database_path();
    session->hero_trinket_slot_limit = *context.configuration_store.current().hero_trinket_slot_limit;
    session->hero_positive_quirk_limit = *context.configuration_store.current().hero_positive_quirk_limit;
    session->hero_negative_quirk_limit = *context.configuration_store.current().hero_negative_quirk_limit;
    if (const auto roster_rules = context.file_system.read_file(
            context.configuration_store.current().game_root / "campaign/roster/roster.variables.json")) {
        try {
            const auto rules = nlohmann::json::parse(roster_rules.value());
            if (rules.contains("resolve_level_thresholds") && rules["resolve_level_thresholds"].is_array()) {
                for (const auto& threshold : rules["resolve_level_thresholds"]) {
                    if (!threshold.is_number_integer()) break;
                    const auto number = threshold.get<std::int32_t>();
                    if (!session->resolve_level_thresholds.empty() && number <= session->resolve_level_thresholds.back()) break;
                    session->resolve_level_thresholds.push_back(number);
                }
                if (session->resolve_level_thresholds.empty() || session->resolve_level_thresholds.front() != 0 ||
                    session->resolve_level_thresholds.size() != rules["resolve_level_thresholds"].size())
                    session->resolve_level_thresholds.clear();
            }
        } catch (const nlohmann::json::exception&) {
            session->resolve_level_thresholds.clear();
        }
    }
    for (const auto& item : model.trinket_inventory) session->reserved_trinket_keys.insert(item.raw_key);
    for (const auto& hero : model.heroes)
        for (const auto& item : hero.trinkets)
            session->reserved_hero_trinket_keys[hero.persistent_id].insert(
                item.raw.display_path.substr(item.raw.display_path.find_last_of('/') + 1));
    session->content = std::move(content);
    session->edits = std::make_unique<application::CampaignEditSession>(std::move(model));
    warm_trinket_catalog(context, *session);
    warm_quirk_catalog(*session);
    warm_hero_catalog(context, *session);
    context.campaign = std::move(session);
    return std::nullopt;
}

std::vector<ServerContext::BuildingUpgradeTree> load_building_upgrade_trees(
    application::IContentEnvironment& content, application::IFileSystem& file_system) {
    constexpr std::array<std::string_view, 8> buildings{
        "abbey", "blacksmith", "camping_trainer", "guild", "nomad_wagon", "sanitarium", "stage_coach", "tavern"};
    std::vector<ServerContext::BuildingUpgradeTree> result;
    for (const auto building : buildings) {
        const auto path = "upgrades/building/" + std::string{building} + ".upgrades.json";
        auto asset = content.resolve_asset(path);
        if (!asset || !asset.value() || asset.value()->source_id != "vanilla") continue;
        auto bytes = file_system.read_file(asset.value()->physical_path);
        if (!bytes) continue;
        try {
            const auto document = nlohmann::json::parse(bytes.value());
            if (!document.is_object() || !document.contains("trees") || !document["trees"].is_array()) continue;
            for (const auto& tree : document["trees"]) {
                if (!tree.is_object() || !tree.contains("id") || !tree["id"].is_string() ||
                    !tree.contains("requirements") || !tree["requirements"].is_array()) continue;
                ServerContext::BuildingUpgradeTree item;
                item.building_id = building;
                item.id = tree["id"].get<std::string>();
                bool valid = !tree["requirements"].empty();
                char expected = 'a';
                for (const auto& requirement : tree["requirements"]) {
                    if (!requirement.is_object() || !requirement.contains("code") ||
                        !requirement["code"].is_string()) { valid = false; break; }
                    const auto code = requirement["code"].get<std::string>();
                    if (code.size() != 1 || code.front() != expected++) { valid = false; break; }
                    item.codes.push_back(code.front());
                }
                if (valid && !item.codes.empty()) {
                    for (const auto code : item.codes) {
                        const auto localization_key = "str_" + item.id + "_upgrade_lvl_" +
                            std::to_string(static_cast<int>(code - 'a') + 1);
                        const auto description = content.resolve_localization(localization_key);
                        auto text = description && description.value() ? description.value()->value : std::string{};
                        std::size_t placeholder = 0;
                        const auto level = std::to_string(static_cast<int>(code - 'a') + 1);
                        while ((placeholder = text.find("%d", placeholder)) != std::string::npos) {
                            text.replace(placeholder, 2, level);
                            placeholder += level.size();
                        }
                        item.descriptions.push_back(std::move(text));
                    }
                    result.push_back(std::move(item));
                }
            }
        } catch (const nlohmann::json::exception&) {
        }
    }
    return result;
}

std::vector<std::string> load_official_district_ids(application::IFileSystem& file_system,
                                                    const std::filesystem::path& game_root) {
    std::vector<std::string> pending{(game_root / "dlc").string()};
    std::vector<std::string> ids;
    while (!pending.empty()) {
        auto directory = std::filesystem::path{std::move(pending.back())};
        pending.pop_back();
        auto children = file_system.list_directories(directory);
        if (children) for (const auto& child : children.value()) pending.push_back(child.string());
        auto files = file_system.list_files(directory);
        if (!files) continue;
        for (const auto& file : files.value()) {
            const auto filename = file.filename().string();
            if (!filename.ends_with(".json") || filename.find("district") == std::string::npos) continue;
            auto bytes = file_system.read_file(file);
            if (!bytes) continue;
            try {
                const auto document = nlohmann::json::parse(bytes.value());
                if (!document.is_object() || !document.contains("buildings") || !document["buildings"].is_array()) continue;
                for (const auto& building : document["buildings"])
                    if (building.is_object() && building.contains("name") && building["name"].is_string())
                        ids.push_back(building["name"].get<std::string>());
            } catch (const nlohmann::json::exception&) {
            }
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

Json::Value definition_assets(const domain::DefinitionReference& definition) {
    Json::Value assets(Json::arrayValue);
    for (const auto& asset : definition.assets) {
        Json::Value item(Json::objectValue);
        item["role"] = asset.role;
        item["path"] = asset.virtual_path;
        item["sourceId"] = asset.source_id;
        item["resolved"] = asset.resolved;
        if (!asset.virtual_path.empty()) item["url"] = "/api/content-asset?path=" + asset.virtual_path;
        assets.append(std::move(item));
    }
    return assets;
}

std::string strip_game_markup(std::string value);

Json::Value trinket_definition_json(application::IContentEnvironment& content,
                                    std::string_view id,
                                    const std::vector<DatabaseModRecord>& mods,
                                    const application::ContentDefinition* resolved_definition = nullptr) {
    Json::Value item(Json::objectValue);
    item["id"] = std::string{id};
    item["sourceId"] = "vanilla";
    item["modName"] = "";
    std::optional<application::ContentDefinition> owned_definition;
    if (resolved_definition == nullptr) {
        const auto found = content.find_content("trinket", id);
        if (found && found.value()) owned_definition = *found.value();
        if (owned_definition) resolved_definition = &*owned_definition;
    }
    if (resolved_definition == nullptr) {
        item["name"] = std::string{id};
        item["localizedName"] = std::string{id};
        item["assets"] = Json::Value(Json::arrayValue);
        return item;
    }
    const auto& definition = *resolved_definition;
    item["name"] = strip_game_markup(definition.localized_name.empty()
        ? (definition.display_name.empty() ? definition.id : definition.display_name)
        : definition.localized_name);
    item["localizedName"] = definition.localized_name;
    const auto english_name = content.resolve_localization(definition.localization_key, "english");
    item["englishName"] = english_name && english_name.value()
        ? english_name.value()->value : definition.display_name;
    item["localizationKey"] = definition.localization_key;
    item["sourceId"] = definition.provenance.source_id;
    item["layerType"] = definition.provenance.layer_type;
    for (const auto& mod : mods) {
        if (mod.matched_mod_id == definition.provenance.source_id) {
            item["modName"] = mod.display_name.empty() ? mod.fallback_name : mod.display_name;
            break;
        }
    }
    // Trinkets have no standalone description localization entry. Their
    // visible effects are built from the referenced buff definitions below.
    item["descriptionKey"] = "";
    item["description"] = "";
    item["englishDescription"] = "";
    Json::Value hero_classes(Json::arrayValue);
    Json::Value hero_class_names(Json::objectValue);
    std::string restriction;
    for (const auto& relation : definition.relationships) {
        if (relation.relationship_type != "restricted_to" || relation.type != "hero_class") continue;
        hero_classes.append(relation.id);
        auto hero_class = content.find_content("hero_class", relation.id);
        const auto name = hero_class && hero_class.value()
            ? hero_class.value()->localized_name : relation.id;
        hero_class_names[relation.id] = strip_game_markup(name);
        if (!restriction.empty()) restriction += " / ";
        restriction += strip_game_markup(name);
    }
    item["heroClasses"] = std::move(hero_classes);
    item["heroClassNames"] = std::move(hero_class_names);
    item["restriction"] = std::move(restriction);
    Json::Value assets(Json::arrayValue);
    for (const auto& asset : definition.asset_references) {
        Json::Value reference(Json::objectValue);
        reference["role"] = asset.role;
        reference["path"] = asset.virtual_path;
        reference["sourceId"] = asset.resolved_asset ? asset.resolved_asset->source_id : definition.provenance.source_id;
        reference["resolved"] = asset.resolved_asset.has_value();
        if (!asset.virtual_path.empty()) reference["url"] = "/api/content-asset?path=" + asset.virtual_path;
        assets.append(std::move(reference));
    }
    item["assets"] = std::move(assets);
    try {
        const auto payload = nlohmann::json::parse(definition.payload_json);
        if (payload.contains("set_id") && payload["set_id"].is_string())
            item["setId"] = payload["set_id"].get<std::string>();
        if (payload.contains("rarity") && (payload["rarity"].is_string() || payload["rarity"].is_number_integer())) {
            const auto rarity_id = payload["rarity"].is_string()
                ? payload["rarity"].get<std::string>()
                : std::to_string(payload["rarity"].get<int>());
            item["rarity"] = payload["rarity"].is_string()
                ? Json::Value(rarity_id) : Json::Value(payload["rarity"].get<int>());
            const auto rarity = content.resolve_localization("trinket_rarity_" + rarity_id);
            if (rarity && rarity.value() && !rarity.value()->value.empty())
                item["rarityName"] = strip_game_markup(rarity.value()->value);
        }
        if (payload.contains("tags") && payload["tags"].is_array()) {
            Json::Value tags(Json::arrayValue);
            for (const auto& tag : payload["tags"])
                if (tag.is_string()) tags.append(tag.get<std::string>());
            item["tags"] = std::move(tags);
        }
    } catch (const nlohmann::json::exception&) {
    }
    return item;
}

namespace {

using TrinketBuffMap = std::map<std::string, nlohmann::json, std::less<>>;
using TrinketSetMap = std::map<std::string, bool, std::less<>>;

void collect_trinket_buff_files(application::IFileSystem& file_system,
                                const std::filesystem::path& directory,
                                TrinketBuffMap& buffs,
                                unsigned int depth = 0) {
    if (depth > 16) return;
    const auto files = file_system.list_files(directory);
    if (files) {
        for (const auto& path : files.value()) {
            const auto filename = path.filename().string();
            if (!filename.ends_with(".buffs.json") && !filename.ends_with(".networkbuffs.json")) continue;
            const auto bytes = file_system.read_file(path);
            if (!bytes) continue;
            try {
                const auto document = nlohmann::json::parse(bytes.value());
                if (!document.is_object() || !document.contains("buffs") || !document["buffs"].is_array()) continue;
                for (const auto& buff : document["buffs"]) {
                    if (!buff.is_object() || !buff.contains("id") || !buff["id"].is_string()) continue;
                    buffs.insert_or_assign(buff["id"].get<std::string>(), buff);
                }
            } catch (const nlohmann::json::exception&) {
            }
        }
    }
    const auto directories = file_system.list_directories(directory);
    if (directories)
        for (const auto& child : directories.value())
            collect_trinket_buff_files(file_system, child, buffs, depth + 1);
}

void collect_trinket_set_files(application::IFileSystem& file_system,
                               const std::filesystem::path& directory,
                               TrinketSetMap& sets,
                               unsigned int depth = 0) {
    if (depth > 16) return;
    const auto files = file_system.list_files(directory);
    if (files) {
        for (const auto& path : files.value()) {
            if (!path.filename().string().ends_with(".sets.trinkets.json")) continue;
            const auto bytes = file_system.read_file(path);
            if (!bytes) continue;
            try {
                const auto document = nlohmann::json::parse(bytes.value());
                if (!document.is_object() || !document.contains("sets") || !document["sets"].is_array()) continue;
                for (const auto& set : document["sets"]) {
                    if (!set.is_object() || !set.contains("id") || !set["id"].is_string()) continue;
                    const bool has_bonus = set.contains("buffs") && set["buffs"].is_array() && !set["buffs"].empty();
                    sets.insert_or_assign(set["id"].get<std::string>(), has_bonus);
                }
            } catch (const nlohmann::json::exception&) {
            }
        }
    }
    const auto directories = file_system.list_directories(directory);
    if (directories)
        for (const auto& child : directories.value())
            collect_trinket_set_files(file_system, child, sets, depth + 1);
}

std::string format_trinket_effect(std::string format, double amount) {
    const auto marker = format.find("%+d");
    const auto plain_marker = marker == std::string::npos ? format.find("%d") : marker;
    if (plain_marker == std::string::npos) return format;
    const bool signed_value = format.compare(plain_marker, 3, "%+d") == 0;
    const auto token_size = signed_value ? 3U : 2U;
    const bool percent = plain_marker + token_size < format.size() && format[plain_marker + token_size] == '%';
    const double shown = percent && std::abs(amount) < 1.0 ? amount * 100.0 : amount;
    const auto rounded = static_cast<long long>(std::llround(shown));
    std::string value = std::to_string(rounded);
    if (signed_value && rounded > 0) value.insert(value.begin(), '+');
    format.replace(plain_marker, token_size, value);
    // Loc strings use %% to mean a literal percent in printf formatting.
    std::size_t escaped = 0;
    while ((escaped = format.find("%%", escaped)) != std::string::npos) {
        format.replace(escaped, 2, "%");
        ++escaped;
    }
    return format;
}

void append_trinket_detail_debug_log(const std::filesystem::path& data_root,
                                     const application::ContentDefinition& definition,
                                     std::size_t buff_reference_count,
                                     const std::vector<std::string>& missing_buffs,
                                     const std::vector<std::string>& missing_localizations,
                                     const std::vector<std::string>& unsupported_stats,
                                     std::size_t hidden_buff_count) noexcept {
    try {
        const auto directory = data_root / "logs";
        std::filesystem::create_directories(directory);
        std::ofstream output(directory / "trinket-detail-debug.jsonl", std::ios::binary | std::ios::app);
        if (!output) return;
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto strings = [](const std::vector<std::string>& values) {
            nlohmann::json result = nlohmann::json::array();
            for (const auto& value : values) result.push_back(value);
            return result;
        };
        const nlohmann::json entry{
            {"timestamp_unix_ms", timestamp},
            {"event", "trinket_tooltip_has_no_visible_effects"},
            {"trinket_id", definition.id},
            {"source_id", definition.provenance.source_id},
            {"buff_reference_count", buff_reference_count},
            {"missing_buff_ids", strings(missing_buffs)},
            {"hidden_buff_count", hidden_buff_count},
            {"untranslated_localization_keys", strings(missing_localizations)},
            {"unsupported_stat_types", strings(unsupported_stats)}};
        output << entry.dump() << '\n';
    } catch (...) {
        // Diagnostic logging must never interrupt catalog loading.
    }
}

} // namespace

void warm_trinket_catalog(ServerContext& context, ServerContext::CampaignSession& campaign) {
    if (!campaign.cached_mod_records) {
        const auto queried = read_enabled_mods(campaign.mod_database_path);
        campaign.cached_mod_records = queried ? queried.value() : std::vector<DatabaseModRecord>{};
    }

    const auto definitions = campaign.content->list_content("trinket");
    if (!definitions) return;
    campaign.trinket_definitions = definitions.value();

    auto& buffs = campaign.cached_buff_definitions;
    TrinketSetMap sets;
    const auto& config = context.configuration_store.current();
    std::vector<std::filesystem::path> roots{config.game_root};
    roots.insert(roots.end(), config.workshop_roots.begin(), config.workshop_roots.end());
    roots.insert(roots.end(), config.local_mod_roots.begin(), config.local_mod_roots.end());
    for (const auto& root : roots) {
        collect_trinket_buff_files(context.file_system, root, buffs);
        collect_trinket_set_files(context.file_system, root, sets);
    }

    std::map<std::string, std::string, std::less<>> localized_effect_formats;
    for (const auto& definition : definitions.value()) {
        auto item = trinket_definition_json(*campaign.content, definition.id,
                                            *campaign.cached_mod_records, &definition);
        if (item["setId"].isString()) {
            const auto set = sets.find(item["setId"].asString());
            item["hasSetBonus"] = set != sets.end() && set->second;
        }
        Json::Value effects(Json::arrayValue);
        std::string joined_effects;
        std::vector<std::string> missing_buffs;
        std::vector<std::string> missing_localizations;
        std::vector<std::string> unsupported_stats;
        std::size_t hidden_buff_count = 0;
        std::size_t buff_reference_count = 0;
        try {
            const auto payload = nlohmann::json::parse(definition.payload_json);
            if (payload.contains("buffs") && payload["buffs"].is_array()) {
                buff_reference_count = payload["buffs"].size();
                for (const auto& buff_id : payload["buffs"]) {
                    if (!buff_id.is_string()) continue;
                    const auto found = buffs.find(buff_id.get<std::string>());
                    if (found == buffs.end()) {
                        missing_buffs.push_back(buff_id.get<std::string>());
                        continue;
                    }
                    if (!found->second.contains("stat_type") ||
                        !found->second["stat_type"].is_string() || !found->second.contains("stat_sub_type") ||
                        !found->second["stat_sub_type"].is_string() || !found->second.contains("amount") ||
                        !found->second["amount"].is_number()) {
                        unsupported_stats.push_back(buff_id.get<std::string>());
                        continue;
                    }
                    // Mods often attach hidden/conditional implementation buffs
                    // to an item. The game marks these has_description=false;
                    // they affect combat but are intentionally omitted from its
                    // item tooltip, so showing them here creates long, misleading
                    // lists of repeated stats.
                    if (found->second.contains("has_description") &&
                        found->second["has_description"].is_boolean() &&
                        !found->second["has_description"].get<bool>()) {
                        ++hidden_buff_count;
                        continue;
                    }
                    const auto stat_type = found->second["stat_type"].get<std::string>();
                    const auto stat_sub_type = found->second["stat_sub_type"].get<std::string>();
                    const auto key = "buff_stat_tooltip_" + stat_type +
                                     (stat_sub_type.empty() ? std::string{} : "_" + stat_sub_type);
                    auto format = localized_effect_formats.find(key);
                    if (format == localized_effect_formats.end()) {
                        const auto localized = campaign.content->resolve_localization(key);
                        const auto value = localized && localized.value() ? localized.value()->value : std::string{};
                        format = localized_effect_formats.emplace(key, value).first;
                    }
                    const auto amount = found->second["amount"].get<double>();
                    // Never guess a user-facing label from internal stat IDs.
                    // Unknown mod stats have no reliable generic translation.
                    if (format->second.empty()) {
                        missing_localizations.push_back(key);
                        continue;
                    }
                    const auto effect = format_trinket_effect(format->second, amount);
                    effects.append(effect);
                    if (!joined_effects.empty()) joined_effects += "\n";
                    joined_effects += effect;
                }
            }

        } catch (const nlohmann::json::exception&) {
        }
        item["effects"] = std::move(effects);
        item["effectSearchText"] = joined_effects;
        if (!joined_effects.empty()) item["description"] = joined_effects;
        if (item["effects"].empty())
            append_trinket_detail_debug_log(config.data_root, definition,
                buff_reference_count,
                missing_buffs,
                missing_localizations, unsupported_stats, hidden_buff_count);
        campaign.cached_trinket_details.insert_or_assign(definition.id, item);
        campaign.cached_trinket_catalog.push_back(std::move(item));
    }
}

void warm_quirk_catalog(ServerContext::CampaignSession& campaign) {
    const auto listed = campaign.content->list_content("quirk");
    if (!listed) return;
    campaign.quirk_definitions = listed.value();
    for (const auto& definition : campaign.quirk_definitions) {
        const auto payload = nlohmann::json::parse(definition.payload_json, nullptr, false);
        if (!payload.is_object() || !payload.contains("is_positive") ||
            !payload["is_positive"].is_boolean() ||
            (payload.contains("is_disease") && payload["is_disease"].is_boolean() &&
             payload["is_disease"].get<bool>())) continue;
        Json::Value item(Json::objectValue);
        item["id"] = definition.id;
        item["name"] = strip_game_markup(definition.localized_name.empty()
            ? (definition.display_name.empty() ? definition.id : definition.display_name)
            : definition.localized_name);
        item["polarity"] = payload["is_positive"].get<bool>() ? "positive" : "negative";
        item["canLock"] = payload["is_positive"].get<bool>() &&
            payload.contains("can_modify_in_activity") &&
            payload["can_modify_in_activity"].is_boolean() &&
            payload["can_modify_in_activity"].get<bool>();
        item["sourceId"] = definition.provenance.source_id;
        item["modName"] = "";
        for (const auto& mod : *campaign.cached_mod_records)
            if (mod.matched_mod_id == definition.provenance.source_id) {
                item["modName"] = mod.display_name.empty() ? mod.fallback_name : mod.display_name;
                break;
            }
        Json::Value effects(Json::arrayValue);
        const auto description = campaign.content->resolve_localization("str_quirk_description_" + definition.id);
        if (description && description.value() && !description.value()->value.empty())
            effects.append(strip_game_markup(description.value()->value));
        if (payload.contains("buffs") && payload["buffs"].is_array())
            for (const auto& buff_id : payload["buffs"]) {
                if (!buff_id.is_string()) continue;
                const auto found = campaign.cached_buff_definitions.find(buff_id.get<std::string>());
                if (found == campaign.cached_buff_definitions.end()) continue;
                const auto& buff = found->second;
                if ((buff.contains("has_description") && buff["has_description"].is_boolean() &&
                     !buff["has_description"].get<bool>()) || !buff.contains("stat_type") ||
                    !buff["stat_type"].is_string() || !buff.contains("stat_sub_type") ||
                    !buff["stat_sub_type"].is_string() || !buff.contains("amount") ||
                    !buff["amount"].is_number()) continue;
                const auto subtype = buff["stat_sub_type"].get<std::string>();
                const auto key = "buff_stat_tooltip_" + buff["stat_type"].get<std::string>() +
                    (subtype.empty() ? std::string{} : "_" + subtype);
                const auto format = campaign.content->resolve_localization(key);
                if (format && format.value() && !format.value()->value.empty())
                    effects.append(strip_game_markup(format_trinket_effect(
                        format.value()->value, buff["amount"].get<double>())));
            }
        item["effects"] = std::move(effects);
        campaign.cached_quirk_catalog.push_back(std::move(item));
    }
}

std::optional<std::string> hero_roster_portrait(const domain::DefinitionReference& definition) {
    for (const auto& asset : definition.assets) {
        auto path = asset.virtual_path;
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (path.find("portrait_roster") != std::string::npos)
            return asset.virtual_path;
    }
    return std::nullopt;
}

std::string strip_game_markup(std::string value) {
    std::size_t start = 0;
    while (start < value.size()) {
        const auto square = value.find('[', start);
        const auto curly = value.find('{', start);
        if (square == std::string::npos && curly == std::string::npos) break;
        const auto opening = square == std::string::npos ? curly
            : curly == std::string::npos ? square : std::min(square, curly);
        const auto closing_character = value[opening] == '[' ? ']' : '}';
        const auto closing = value.find(closing_character, opening + 1);
        if (closing == std::string::npos) break;
        value.erase(opening, closing - opening + 1);
        start = opening;
    }

    // Compiled .loc2 tables encode colour markup as <c>XXXXXXtext</c>.
    // The six-character prefix is a game colour code, not part of the
    // localized label.  Some codes use only five ASCII characters before a
    // UTF-8 label starts, so stop at the first non-ASCII byte as well.
    std::size_t tag_start = 0;
    while ((tag_start = value.find("<c>", tag_start)) != std::string::npos) {
        const auto tag_end = value.find("</c>", tag_start + 3);
        if (tag_end == std::string::npos) {
            value.erase(tag_start, 3);
            break;
        }
        auto content = value.substr(tag_start + 3, tag_end - (tag_start + 3));
        std::size_t prefix = 0;
        while (prefix < content.size() && prefix < 6 &&
               static_cast<unsigned char>(content[prefix]) < 0x80) {
            ++prefix;
        }
        content.erase(0, prefix);
        value.replace(tag_start, tag_end + 4 - tag_start, content);
        tag_start += content.size();
    }
    return value;
}

std::optional<std::int32_t> hero_upgrade_instance(std::string_view hero_id) {
    std::int32_t instance{};
    const auto [end, error] = std::from_chars(hero_id.data(), hero_id.data() + hero_id.size(), instance);
    if (hero_id.empty() || error != std::errc{} || end != hero_id.data() + hero_id.size() || instance <= 0)
        return std::nullopt;
    return instance;
}

std::optional<std::int32_t> hero_upgrade_limit(const ServerContext::CampaignSession& campaign,
                                               std::string_view class_id, std::string_view suffix) {
    const auto found = campaign.cached_hero_upgrade_max.find(class_id);
    if (found == campaign.cached_hero_upgrade_max.end()) return std::nullopt;
    const auto limit = found->second.find(suffix);
    if (limit == found->second.end()) return std::nullopt;
    return limit->second;
}

std::int32_t hero_purchased_rank(const domain::CampaignModel& model, std::int32_t instance,
                                 std::string_view tree_id, std::int32_t maximum) {
    if (maximum <= 0) return 0;
    std::vector<bool> purchased(static_cast<std::size_t>(maximum), false);
    const auto tree_hash = static_cast<std::int32_t>(core::dson::string_hash(tree_id));
    for (const auto& node : model.upgrade_purchase_nodes) {
        const auto offset = static_cast<std::int32_t>(node.requirement_code - '0');
        if (node.instance_number == instance && node.tree_id == tree_hash &&
            offset >= 0 && offset < maximum && node.is_purchased.value.value_or(false))
            purchased[static_cast<std::size_t>(offset)] = true;
    }
    std::int32_t rank = 0;
    while (rank < maximum && purchased[static_cast<std::size_t>(rank)]) ++rank;
    return rank;
}

Json::Value campaign_value(const ServerContext::CampaignSession& campaign) {
    const auto& model = campaign.edits->model();
    Json::Value value(Json::objectValue);
    value["profileId"] = model.summary.profile_id;
    value["state"] = model.state == domain::ModelState::Complete ? "complete" :
                      model.state == domain::ModelState::Partial ? "partial" : "invalid";
    value["revision"] = Json::UInt64(campaign.edits->revision());
    value["dirty"] = !campaign.edits->pending_changes().empty();
    value["canUndo"] = campaign.edits->can_undo();
    value["canRedo"] = campaign.edits->can_redo();
    value["heroTrinketSlotLimit"] = campaign.hero_trinket_slot_limit;
    value["heroPositiveQuirkLimit"] = campaign.hero_positive_quirk_limit;
    value["heroNegativeQuirkLimit"] = campaign.hero_negative_quirk_limit;

    Json::Value resources(Json::arrayValue);
    for (const auto& resource : model.resources) {
        Json::Value item(Json::objectValue);
        item["index"] = static_cast<Json::UInt64>(resource.index);
        item["id"] = resource.id.value ? *resource.id.value : resource.definition.raw_id;
        item["name"] = strip_game_markup(resource.definition.display_name.empty()
            ? (resource.id.value ? *resource.id.value : resource.definition.raw_id)
            : resource.definition.display_name);
        item["amount"] = resource.amount.value ? Json::Value(*resource.amount.value) : Json::Value(Json::nullValue);
        item["editable"] = resource.amount.value.has_value() && resource.amount.raw.has_value();
        item["assets"] = definition_assets(resource.definition);
        resources.append(std::move(item));
    }
    value["resources"] = std::move(resources);

    if (!campaign.cached_mod_records) {
        const auto queried = read_enabled_mods(campaign.mod_database_path);
        campaign.cached_mod_records = queried ? queried.value() : std::vector<DatabaseModRecord>{};
    }
    const auto& mods = *campaign.cached_mod_records;
    const auto trinket_details = [&](const std::string& id) -> const Json::Value& {
        auto found = campaign.cached_trinket_details.find(id);
        if (found == campaign.cached_trinket_details.end())
            found = campaign.cached_trinket_details.emplace(id,
                trinket_definition_json(*campaign.content, id, mods)).first;
        return found->second;
    };

    Json::Value heroes(Json::arrayValue);
    for (const auto& hero : model.heroes) {
        Json::Value item(Json::objectValue);
        item["id"] = hero.persistent_id;
        item["name"] = strip_game_markup(hero.name.value.value_or(std::string{}));
        item["nameEditable"] = hero.name.value.has_value() && hero.name.raw.has_value();
        item["rosterPosition"] = static_cast<Json::UInt64>(hero.roster_position);
        item["classId"] = hero.class_id.value ? *hero.class_id.value : hero.definition.raw_id;
        const auto class_id = item["classId"].asString();
        const auto purchase_instance = hero_upgrade_instance(hero.persistent_id);
        item["className"] = strip_game_markup(hero.definition.display_name.empty()
            ? (hero.class_id.value ? *hero.class_id.value : hero.definition.raw_id)
            : hero.definition.display_name);
        item["state"] = hero.state == domain::EntityState::Resolved ? "resolved" : "partial";
        item["assets"] = definition_assets(hero.definition);
        if (const auto portrait = hero_roster_portrait(hero.definition)) item["portraitPath"] = *portrait;
        auto level = hero.level.value;
        if (!level && hero.resolve_xp.value && !campaign.resolve_level_thresholds.empty()) {
            const auto found = std::upper_bound(campaign.resolve_level_thresholds.begin(),
                campaign.resolve_level_thresholds.end(), *hero.resolve_xp.value);
            if (found != campaign.resolve_level_thresholds.begin())
                level = static_cast<std::int32_t>(found - campaign.resolve_level_thresholds.begin() - 1);
        }
        item["level"] = level ? Json::Value(*level) : Json::Value(Json::nullValue);
        item["maxLevel"] = campaign.resolve_level_thresholds.empty()
            ? Json::Value(Json::nullValue)
            : Json::Value(static_cast<Json::Int>(campaign.resolve_level_thresholds.size() - 1));
        item["resolveXp"] = hero.resolve_xp.value ? Json::Value(*hero.resolve_xp.value) : Json::Value(Json::nullValue);
        item["stress"] = hero.stress.value ? Json::Value(*hero.stress.value) : Json::Value(Json::nullValue);
        const auto catalog = std::find_if(campaign.cached_hero_catalog.begin(), campaign.cached_hero_catalog.end(),
            [&](const Json::Value& entry) { return entry["id"].asString() == item["classId"].asString(); });
        if (catalog != campaign.cached_hero_catalog.end()) {
            for (const auto* key : {"idleSpritePath", "idleAtlasPath", "idleSkeletonPath"})
                if (catalog->isMember(key)) item[key] = (*catalog)[key];
            if (!item.isMember("portraitPath") && catalog->isMember("portraitPath"))
                item["portraitPath"] = (*catalog)["portraitPath"];
        }
        Json::Value equipment(Json::objectValue);
        const auto art = campaign.cached_hero_equipment.find(item["classId"].asString());
        for (const auto& [kind, rank] : std::array{
                 std::pair{"weapon", hero.weapon_rank.value},
                 std::pair{"armour", hero.armour_rank.value}}) {
            Json::Value entry(Json::objectValue);
            entry["rank"] = rank ? Json::Value(*rank) : Json::Value(Json::nullValue);
            if (const auto maximum = hero_upgrade_limit(campaign, class_id, kind))
                entry["maxRank"] = *maximum;
            if (art != campaign.cached_hero_equipment.end() && art->second.isMember(kind)) {
                const auto& levels = art->second[kind];
                if (rank && *rank >= 0 && static_cast<Json::ArrayIndex>(*rank) < levels.size() &&
                    levels[static_cast<Json::ArrayIndex>(*rank)].isObject()) {
                    const auto& level = levels[static_cast<Json::ArrayIndex>(*rank)];
                    if (level.isMember("name")) entry["name"] = level["name"];
                    if (level.isMember("iconPath")) entry["iconPath"] = level["iconPath"];
                }
            }
            equipment[kind] = std::move(entry);
        }
        item["equipment"] = std::move(equipment);
        Json::Value quirks(Json::arrayValue);
        for (const auto& quirk : hero.quirks) {
            Json::Value entry(Json::objectValue);
            entry["id"] = quirk.id;
            entry["name"] = strip_game_markup(quirk.definition.display_name.empty()
                ? quirk.id : quirk.definition.display_name);
            entry["isDisease"] = quirk.is_disease;
            entry["isLocked"] = quirk.is_locked.value.value_or(false);
            entry["polarity"] = quirk.polarity == domain::QuirkPolarity::Positive ? "positive" :
                quirk.polarity == domain::QuirkPolarity::Negative ? "negative" : "unknown";
            entry["assets"] = definition_assets(quirk.definition);
            const auto details = std::find_if(campaign.cached_quirk_catalog.begin(), campaign.cached_quirk_catalog.end(),
                [&](const Json::Value& value) { return value["id"].asString() == quirk.id; });
            if (details != campaign.cached_quirk_catalog.end()) {
                entry["name"] = (*details)["name"];
                entry["effects"] = (*details)["effects"];
                entry["sourceId"] = (*details)["sourceId"];
                entry["modName"] = (*details)["modName"];
                entry["canLock"] = (*details)["canLock"];
                entry["polarity"] = (*details)["polarity"];
            }
            quirks.append(std::move(entry));
        }
        item["quirks"] = std::move(quirks);
        const auto skills_value = [&](const std::vector<domain::HeroSkillSelection>& skills, bool camping) {
            Json::Value entries(Json::arrayValue);
            for (const auto& skill : skills) {
                Json::Value entry(Json::objectValue);
                entry["id"] = skill.id;
                std::string name = skill.definition.display_name;
                if (camping) {
                    const auto localized = campaign.content->resolve_localization("camping_skill_name_" + skill.id);
                    if (localized && localized.value() && !localized.value()->value.empty())
                        name = localized.value()->value;
                }
                entry["name"] = strip_game_markup(name.empty() ? skill.id : name);
                entry["assets"] = definition_assets(skill.definition);
                if (camping) {
                    const auto icon = std::find_if(skill.definition.assets.begin(), skill.definition.assets.end(),
                        [](const domain::AssetReference& asset) {
                            return asset.role == "skill_icon" && asset.resolved;
                        });
                    if (icon != skill.definition.assets.end()) entry["iconPath"] = icon->virtual_path;
                    else {
                        const auto path = "raid/camping/skill_icons/camp_skill_" + skill.id + ".png";
                        const auto resolved = campaign.content->resolve_asset(path);
                        if (resolved && resolved.value()) entry["iconPath"] = path;
                    }
                } else {
                    const auto separator = skill.id.find(':');
                    const auto suffix = separator == std::string::npos ? skill.id : skill.id.substr(separator + 1);
                    if (const auto maximum = hero_upgrade_limit(campaign, class_id, suffix)) {
                        entry["maxRank"] = *maximum;
                        entry["rank"] = purchase_instance
                            ? std::max(1, hero_purchased_rank(model, *purchase_instance,
                                class_id + "." + suffix, *maximum)) : 1;
                    }
                    const auto found = campaign.cached_combat_skill_icons.find(item["classId"].asString() + ":" + skill.id);
                    if (found != campaign.cached_combat_skill_icons.end()) entry["iconPath"] = found->second;
                }
                entries.append(std::move(entry));
            }
            return entries;
        };
        item["combatSkills"] = skills_value(hero.combat_skills, false);
        Json::Value camping_skills(Json::arrayValue);
        if (catalog != campaign.cached_hero_catalog.end() && (*catalog).isMember("availableCampingSkills"))
            camping_skills = (*catalog)["availableCampingSkills"];
        const auto selected_camping = skills_value(hero.camping_skills, true);
        for (Json::ArrayIndex selected_index = 0; selected_index < selected_camping.size(); ++selected_index) {
            const auto& selected = selected_camping[selected_index];
            bool found = false;
            for (auto& available : camping_skills) {
                if (available["id"].asString() != selected["id"].asString()) continue;
                available["selected"] = true;
                available["selectedOrder"] = selected_index;
                if (!available.isMember("iconPath") && selected.isMember("iconPath"))
                    available["iconPath"] = selected["iconPath"];
                found = true;
                break;
            }
            if (!found) {
                auto entry = selected;
                entry["selected"] = true;
                entry["selectedOrder"] = selected_index;
                camping_skills.append(std::move(entry));
            }
        }
        for (auto& skill : camping_skills) {
            bool learned = skill.get("selected", false).asBool();
            if (purchase_instance && hero_purchased_rank(model, *purchase_instance,
                class_id + "." + skill["id"].asString(), 1) > 0) learned = true;
            skill["learned"] = learned;
            if (!skill.isMember("selected")) skill["selected"] = false;
        }
        item["campingSkills"] = std::move(camping_skills);
        Json::Value equipped(Json::arrayValue);
        for (const auto& trinket : hero.trinkets) {
            auto entry = trinket_details(trinket.id);
            entry["rawKey"] = trinket.raw.display_path.substr(trinket.raw.display_path.find_last_of('/') + 1);
            equipped.append(std::move(entry));
        }
        item["trinkets"] = std::move(equipped);
        heroes.append(std::move(item));
    }
    value["heroes"] = std::move(heroes);

    Json::Value trinkets(Json::arrayValue);
    for (const auto& trinket : model.trinket_inventory) {
        Json::Value item(Json::objectValue);
        item["index"] = static_cast<Json::UInt64>(trinket.index);
        const auto id = trinket.id.value ? *trinket.id.value : trinket.definition.raw_id;
        item["rawKey"] = trinket.raw_key;
        const auto& details = trinket_details(id);
        item["id"] = id;
        item["name"] = details["name"];
        item["restriction"] = details["restriction"];
        item["rarity"] = details["rarity"];
        item["rarityName"] = details["rarityName"];
        item["description"] = details["description"];
        item["effects"] = details["effects"];
        item["effectSearchText"] = details["effectSearchText"];
        item["englishName"] = details["englishName"];
        item["localizationKey"] = details["localizationKey"];
        item["sourceId"] = details["sourceId"];
        item["modName"] = details["modName"];
        item["setId"] = details["setId"];
        item["hasSetBonus"] = details["hasSetBonus"];
        item["amount"] = trinket.amount.value ? Json::Value(*trinket.amount.value) : Json::Value(Json::nullValue);
        item["assets"] = details["assets"];
        trinkets.append(std::move(item));
    }
    value["trinkets"] = std::move(trinkets);

    constexpr std::array<std::string_view, 9> building_order{
        "camping_trainer", "stage_coach", "tavern", "sanitarium", "abbey", "graveyard",
        "nomad_wagon", "guild", "blacksmith"};
    Json::Value buildings(Json::arrayValue);
    for (const auto building_id : building_order) {
        Json::Value building(Json::objectValue);
        building["id"] = std::string{building_id};
        const auto found = std::find_if(model.town_buildings.begin(), model.town_buildings.end(), [&](const auto& item) {
            return item.id == building_id;
        });
        building["name"] = found == model.town_buildings.end() || found->definition.display_name.empty()
            ? std::string{building_id} : strip_game_markup(found->definition.display_name);
        Json::Value trees(Json::arrayValue);
        for (const auto& tree : campaign.building_upgrade_trees) {
            if (tree.building_id != building_id) continue;
            const auto hash = static_cast<std::int32_t>(core::dson::string_hash(tree.id));
            Json::Value item(Json::objectValue);
            item["id"] = tree.id;
            std::uint32_t rank = 0;
            Json::Value nodes(Json::arrayValue);
            for (std::size_t node_index = 0; node_index < tree.codes.size(); ++node_index) {
                const auto code = tree.codes[node_index];
                const auto node = std::find_if(model.upgrade_purchase_nodes.begin(), model.upgrade_purchase_nodes.end(),
                    [&](const auto& purchase) {
                        return purchase.instance_number == 0 && purchase.tree_id == hash &&
                               purchase.requirement_code == code;
                    });
                const bool purchased = node != model.upgrade_purchase_nodes.end() &&
                    node->is_purchased.value.value_or(false);
                if (purchased) ++rank;
                Json::Value node_json(Json::objectValue);
                node_json["code"] = std::string(1, code);
                node_json["purchased"] = purchased;
                if (node_index < tree.descriptions.size())
                    node_json["description"] = strip_game_markup(tree.descriptions[node_index]);
                nodes.append(std::move(node_json));
            }
            item["rank"] = rank;
            item["maxRank"] = static_cast<Json::UInt>(tree.codes.size());
            item["nodes"] = std::move(nodes);
            trees.append(std::move(item));
        }
        building["upgradeTrees"] = std::move(trees);
        buildings.append(std::move(building));
    }
    value["buildings"] = std::move(buildings);

    value["districtSystemOpen"] = model.district_system_open;
    Json::Value districts(Json::arrayValue);
    for (const auto& district_id : campaign.official_district_ids) {
        Json::Value item(Json::objectValue);
        item["id"] = district_id;
        const auto key = "str_" + district_id + "_title";
        const auto localized = campaign.content->resolve_localization(key);
        const auto localized_name = localized && localized.value()
            ? localized.value()->value : district_id;
        item["name"] = strip_game_markup(localized_name);
        const auto state = std::find_if(model.districts.begin(), model.districts.end(), [&](const auto& entry) {
            return entry.id == district_id;
        });
        item["built"] = state != model.districts.end() && state->built.value.value_or(false);
        item["editable"] = model.district_system_open && state != model.districts.end() &&
                            state->built.value.has_value();
        districts.append(std::move(item));
    }
    value["districts"] = std::move(districts);
    return value;
}

void handle_campaign(const drogon::HttpRequestPtr& request,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    callback(json_ok(campaign_value(*context->campaign)));
}

void handle_campaign_reload(const drogon::HttpRequestPtr& request,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                            const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    std::lock_guard lock(context->campaign_mutex);
    context->campaign.reset();
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    callback(json_ok(campaign_value(*context->campaign)));
}

void handle_campaign_resource(const drogon::HttpRequestPtr& request,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                              const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    const auto body = request->getJsonObject();
    const auto is_integer = [](const Json::Value& value) {
        return value.isInt() || value.isUInt() || value.isInt64() || value.isUInt64();
    };
    if (!body || !body->isObject() || !is_integer((*body)["index"]) || !is_integer((*body)["amount"]) ||
        !is_integer((*body)["revision"]) || (*body)["index"].asInt64() < 0 || (*body)["revision"].asInt64() < 0) {
        callback(json_error(drogon::k400BadRequest, "INVALID_CAMPAIGN_OPERATION",
                            "Resource edit requires index, non-negative amount, and revision."));
        return;
    }
    const auto amount = (*body)["amount"].asInt64();
    if (amount < 0 || amount > std::numeric_limits<std::int32_t>::max()) {
        callback(json_error(drogon::k400BadRequest, "INVALID_CAMPAIGN_OPERATION",
                            "Resource amount is outside the supported range."));
        return;
    }
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    application::SetCampaignValueOperation operation{
        application::CampaignOperationTarget{"Estate.Resource.Amount", {},
                                             std::optional<std::size_t>{static_cast<std::size_t>((*body)["index"].asUInt64())}},
        static_cast<std::int32_t>(amount)};
    auto applied = context->campaign->edits->apply(application::CampaignOperation{std::move(operation)},
                                                    (*body)["revision"].asUInt64());
    if (!applied) {
        const auto status = applied.error().code == core::ErrorCode::StaleSessionRevision
            ? drogon::k409Conflict : drogon::k400BadRequest;
        callback(json_error(status, std::string{core::to_string(applied.error().code)}, applied.error().message));
        return;
    }
    callback(json_ok(campaign_value(*context->campaign)));
}

struct HeroStarterData {
    std::vector<std::string> combat_skills;
    std::vector<std::string> camping_skills;
    float base_hit_points{};
};

void append_hero_catalog_debug_log(const std::filesystem::path& data_root,
                                  std::string_view event,
                                  const application::ContentDefinition* definition,
                                  const core::Error* error,
                                  std::size_t listed_count = 0,
                                  std::size_t usable_count = 0) noexcept {
    try {
        const auto directory = data_root / "logs";
        std::filesystem::create_directories(directory);
        std::ofstream output(directory / "hero-catalog-debug.jsonl", std::ios::binary | std::ios::app);
        if (!output) return;
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        nlohmann::json context = nlohmann::json::object();
        if (error) for (const auto& [key, value] : error->context) context[key] = value;
        nlohmann::json entry{
            {"timestamp_unix_ms", timestamp}, {"event", event},
            {"listed_count", listed_count}, {"usable_count", usable_count},
            {"error_code", error ? std::string{core::to_string(error->code)} : std::string{}},
            {"error_module", error ? error->module : std::string{}},
            {"error", error ? error->message : std::string{}}, {"context", std::move(context)}};
        if (definition) {
            entry["class_id"] = definition->id;
            entry["source_id"] = definition->provenance.source_id;
            entry["virtual_path"] = definition->provenance.virtual_path;
        }
        output << entry.dump() << '\n';
    } catch (...) {
        // Catalog diagnostics must never interrupt hero selection.
    }
}

std::vector<std::string> definition_field_values(const nlohmann::json& record, std::string_view name) {
    std::vector<std::string> result;
    if (!record.is_object() || !record.contains("fields") || !record["fields"].is_object()) return result;
    const auto& fields = record["fields"];
    const auto found = fields.find(std::string{name});
    if (found == fields.end()) return result;
    if (found->is_array()) {
        for (const auto& value : *found) {
            if (value.is_string()) result.push_back(value.get<std::string>());
            else if (value.is_number()) result.push_back(value.dump());
        }
    } else if (found->is_string()) result.push_back(found->get<std::string>());
    return result;
}

std::optional<std::int32_t> definition_int_field(const nlohmann::json& record, std::string_view name) {
    const auto values = definition_field_values(record, name);
    if (values.empty()) return std::nullopt;
    std::int32_t result{};
    const auto& text = values.front();
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return result;
}

core::Result<HeroStarterData, core::Error> hero_starter_data(
    const application::ContentDefinition& definition,
    const std::vector<application::ContentDefinition>& skill_definitions) {
    HeroStarterData starter;
    std::vector<std::string> combat_order;
    std::vector<std::string> guaranteed_combat;
    std::int32_t combat_count = 4;
    std::int32_t class_camping_count = 2;
    std::int32_t shared_camping_count = 1;
    try {
        const auto payload = nlohmann::json::parse(definition.payload_json);
        if (!payload.is_object() || !payload.contains("records") || !payload["records"].is_array())
            return core::Result<HeroStarterData, core::Error>::failure(
                {core::ErrorCode::ContentParseFailed, "Hero class definition has no parsed records", "HeroFactory",
                 {{"class_id", definition.id}}});
        for (const auto& record : payload["records"]) {
            if (!record.is_object() || !record.contains("type") || !record["type"].is_string()) continue;
            const auto type = record["type"].get<std::string>();
            if (type == "generation") {
                if (const auto value = definition_int_field(record, "number_of_random_combat_skills")) combat_count = *value;
                if (const auto value = definition_int_field(record, "number_of_class_specific_camping_skills")) class_camping_count = *value;
                if (const auto value = definition_int_field(record, "number_of_shared_camping_skills")) shared_camping_count = *value;
            } else if (type == "armour") {
                if (starter.base_hit_points <= 0.0F) {
                    const auto values = definition_field_values(record, "hp");
                    if (!values.empty()) {
                        try { starter.base_hit_points = std::stof(values.front()); }
                        catch (const std::exception&) {}
                    }
                }
            } else if (type == "combat_skill") {
                const auto ids = definition_field_values(record, "id");
                const auto levels = definition_field_values(record, "level");
                if (ids.empty() || (!levels.empty() && levels.front() != "0")) continue;
                const auto& id = ids.front();
                if (std::find(combat_order.begin(), combat_order.end(), id) == combat_order.end()) {
                    combat_order.push_back(id);
                    const auto guaranteed = definition_field_values(record, "generation_guaranteed");
                    if (!guaranteed.empty() && (guaranteed.front() == "true" || guaranteed.front() == "True"))
                        guaranteed_combat.push_back(id);
                }
            }
        }
    } catch (const nlohmann::json::exception& error) {
        return core::Result<HeroStarterData, core::Error>::failure(
            {core::ErrorCode::ContentParseFailed, "Hero class definition payload is invalid", "HeroFactory",
             {{"class_id", definition.id}, {"reason", error.what()}}});
    }
    if (starter.base_hit_points <= 0.0F || combat_count <= 0 || class_camping_count < 0 || shared_camping_count < 0)
        return core::Result<HeroStarterData, core::Error>::failure(
            {core::ErrorCode::ContentParseFailed, "Hero class is missing valid resolve-level-zero equipment or skill rules", "HeroFactory",
             {{"class_id", definition.id}}});
    const auto wanted_combat = static_cast<std::size_t>(combat_count);
    for (const auto& id : guaranteed_combat)
        if (starter.combat_skills.size() < wanted_combat) starter.combat_skills.push_back(id);
    for (const auto& id : combat_order)
        if (starter.combat_skills.size() < wanted_combat &&
            std::find(starter.combat_skills.begin(), starter.combat_skills.end(), id) == starter.combat_skills.end())
            starter.combat_skills.push_back(id);
    if (starter.combat_skills.empty())
        return core::Result<HeroStarterData, core::Error>::failure(
            {core::ErrorCode::ContentParseFailed, "Hero class has no level-zero combat skills", "HeroFactory",
             {{"class_id", definition.id}}});

    std::vector<std::string> class_camping;
    std::vector<std::string> shared_camping;
    const auto class_prefix = definition.id + ":";
    for (const auto& skill : skill_definitions) {
        auto path = skill.provenance.virtual_path;
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (path.find("camping_skills") == std::string::npos) continue;
        const auto separator = skill.id.find(':');
        if (separator == std::string::npos) continue;
        const auto raw_id = skill.id.substr(separator + 1);
        const bool class_prefixed = skill.id.starts_with(class_prefix);
        bool applicable = class_prefixed;
        bool is_class_specific = class_prefixed;
        try {
            const auto data = nlohmann::json::parse(skill.payload_json);
            if (!data.is_object()) continue;
            if (data.contains("level")) {
                const auto& level = data["level"];
                if ((level.is_number_integer() && level.get<std::int32_t>() != 0) ||
                    (level.is_string() && level.get<std::string>() != "0")) continue;
            }
            if (data.is_object() && data.contains("hero_classes") && data["hero_classes"].is_array()) {
                std::size_t class_count{};
                for (const auto& hero_class : data["hero_classes"]) if (hero_class.is_string()) {
                    ++class_count;
                    applicable = applicable || hero_class.get<std::string>() == definition.id;
                }
                if (!applicable) continue;
                // The game's default.camping_skills.json uses this threshold to
                // divide class-specific skills from shared skills.
                is_class_specific = class_count <= 4;
            } else if (!class_prefixed) {
                continue;
            }
        } catch (const nlohmann::json::exception&) {
            continue;
        }
        auto& destination = is_class_specific ? class_camping : shared_camping;
        if (!raw_id.empty() && std::find(destination.begin(), destination.end(), raw_id) == destination.end())
            destination.push_back(raw_id);
    }
    std::sort(class_camping.begin(), class_camping.end());
    std::sort(shared_camping.begin(), shared_camping.end());
    const auto append_unique = [&](const std::vector<std::string>& source, std::size_t limit) {
        for (const auto& id : source) {
            if (starter.camping_skills.size() >= limit) break;
            if (std::find(starter.camping_skills.begin(), starter.camping_skills.end(), id) == starter.camping_skills.end())
                starter.camping_skills.push_back(id);
        }
    };
    const auto wanted_camping = static_cast<std::size_t>(class_camping_count + shared_camping_count);
    append_unique(class_camping, static_cast<std::size_t>(class_camping_count));
    append_unique(shared_camping, static_cast<std::size_t>(class_camping_count + shared_camping_count));
    // Many class mods put every camping skill in the hero's own file even when
    // their generation rule requests a shared skill. Use the remaining class
    // skills as a safe fallback instead of hiding an otherwise valid class.
    append_unique(class_camping, wanted_camping);
    append_unique(shared_camping, wanted_camping);
    return core::Result<HeroStarterData, core::Error>::success(std::move(starter));
}

std::string art_field(std::string_view line, std::string_view field) {
    const auto marker = "." + std::string{field};
    const auto at = line.find(marker);
    if (at == std::string_view::npos) return {};
    auto start = line.find_first_not_of(" \t", at + marker.size());
    if (start == std::string_view::npos) return {};
    if (line[start] == '"') {
        const auto end = line.find('"', ++start);
        return end == std::string_view::npos ? std::string{} : std::string{line.substr(start, end - start)};
    }
    const auto end = line.find_first_of(" \t\r\n", start);
    return std::string{line.substr(start, end == std::string_view::npos ? end : end - start)};
}

void cache_hero_art(ServerContext& context, ServerContext::CampaignSession& campaign,
                    const application::ContentDefinition& definition) {
    const auto& config = context.configuration_store.current();
    std::filesystem::path source_root;
    const auto& source = definition.provenance.source_id;
    if (source == "vanilla") source_root = config.game_root;
    else if (source.starts_with("dlc:")) source_root = config.game_root / "dlc" / source.substr(4);
    else if (campaign.cached_mod_records) {
        const auto found = std::find_if(campaign.cached_mod_records->begin(), campaign.cached_mod_records->end(),
            [&](const DatabaseModRecord& mod) { return mod.matched_mod_id == source; });
        if (found != campaign.cached_mod_records->end()) source_root = found->root_path;
    }
    auto art_path = definition.provenance.virtual_path;
    constexpr std::string_view info_suffix{".info.darkest"};
    if (source_root.empty() || !art_path.ends_with(info_suffix)) return;
    art_path.replace(art_path.size() - info_suffix.size(), info_suffix.size(), ".art.darkest");
    const auto utf8_path = std::u8string{reinterpret_cast<const char8_t*>(art_path.data()), art_path.size()};
    const auto bytes = context.file_system.read_file(source_root / std::filesystem::path{utf8_path});
    if (!bytes) return;
    std::istringstream input{bytes.value()};
    std::string line;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        const auto record = std::string_view{line}.substr(first);
        if (record.starts_with("combat_skill:")) {
            const auto skill_id = art_field(line, "id");
            const auto icon_id = art_field(line, "icon");
            if (skill_id.empty() || icon_id.empty() || icon_id.find_first_of("/\\.") != std::string::npos) continue;
            const auto virtual_path = "heroes/" + definition.id + "/" + definition.id + ".ability." + icon_id + ".png";
            const auto resolved = campaign.content->resolve_asset(virtual_path);
            if (resolved && resolved.value())
                campaign.cached_combat_skill_icons.insert_or_assign(definition.id + ":" + skill_id, virtual_path);
            continue;
        }
        const auto kind = record.starts_with("weapon:") ? std::string{"weapon"} :
            record.starts_with("armour:") ? std::string{"armour"} : std::string{};
        if (kind.empty()) continue;
        const auto name_key = art_field(line, "name");
        const auto icon_file = art_field(line, "icon");
        const auto suffix = name_key.find_last_of('_');
        if (suffix == std::string::npos || !name_key.starts_with(definition.id + "_" + kind + "_") ||
            icon_file.empty() || !icon_file.ends_with(".png") ||
            icon_file.find_first_of("/\\") != std::string::npos) continue;
        std::int32_t rank{};
        const auto number = std::string_view{name_key}.substr(suffix + 1);
        const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), rank);
        if (error != std::errc{} || end != number.data() + number.size() || rank < 0 || rank > 50) continue;
        Json::Value entry(Json::objectValue);
        entry["rank"] = rank;
        const auto localized = campaign.content->resolve_localization(name_key);
        if (localized && localized.value() && !localized.value()->value.empty() &&
            localized.value()->value != name_key)
            entry["name"] = strip_game_markup(localized.value()->value);
        const auto virtual_path = "heroes/" + definition.id + "/icons_equip/" + icon_file;
        const auto resolved = campaign.content->resolve_asset(virtual_path);
        if (resolved && resolved.value()) entry["iconPath"] = virtual_path;
        auto& equipment = campaign.cached_hero_equipment[definition.id];
        if (equipment.isNull()) equipment = Json::Value(Json::objectValue);
        auto& levels = equipment[kind];
        if (levels.isNull()) levels = Json::Value(Json::arrayValue);
        levels[static_cast<Json::ArrayIndex>(rank)] = std::move(entry);
    }
}

void cache_hero_upgrade_limits(ServerContext& context, ServerContext::CampaignSession& campaign,
                               const application::ContentDefinition& definition) {
    const auto path = "upgrades/heroes/" + definition.id + ".upgrades.json";
    const auto asset = campaign.content->resolve_asset(path);
    if (!asset || !asset.value()) return;
    const auto bytes = context.file_system.read_file(asset.value()->physical_path);
    if (!bytes) return;
    try {
        const auto document = nlohmann::json::parse(bytes.value());
        if (!document.is_object() || !document.contains("trees") || !document["trees"].is_array()) return;
        const auto prefix = definition.id + ".";
        auto& limits = campaign.cached_hero_upgrade_max[definition.id];
        for (const auto& tree : document["trees"]) {
            if (!tree.is_object() || !tree.contains("id") || !tree["id"].is_string() ||
                !tree.contains("requirements") || !tree["requirements"].is_array()) continue;
            const auto id = tree["id"].get<std::string>();
            if (!id.starts_with(prefix)) continue;
            const auto& requirements = tree["requirements"];
            if (requirements.empty() || requirements.size() > 32) continue;
            bool contiguous = true;
            for (std::size_t index = 0; index < requirements.size(); ++index) {
                const auto& requirement = requirements[index];
                if (!requirement.is_object() || !requirement.contains("code") ||
                    !requirement["code"].is_string() ||
                    requirement["code"].get<std::string>() != std::string(1, static_cast<char>('0' + index))) {
                    contiguous = false;
                    break;
                }
            }
            if (contiguous) limits[id.substr(prefix.size())] = static_cast<std::int32_t>(requirements.size());
        }
    } catch (const nlohmann::json::exception&) {
        return;
    }
}

Json::Value available_camping_skills(const application::ContentDefinition& hero_class,
                                     const std::vector<application::ContentDefinition>& skills,
                                     const application::IContentEnvironment& content) {
    Json::Value result(Json::arrayValue);
    std::set<std::string, std::less<>> included;
    for (const auto& skill : skills) {
        auto path = skill.provenance.virtual_path;
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (path.find("camping_skills") == std::string::npos) continue;
        const auto separator = skill.id.find(':');
        if (separator == std::string::npos || separator + 1 == skill.id.size()) continue;
        const auto raw_id = skill.id.substr(separator + 1);
        bool applicable = skill.id.starts_with(hero_class.id + ":");
        try {
            const auto payload = nlohmann::json::parse(skill.payload_json);
            if (!payload.is_object()) continue;
            if (payload.contains("level")) {
                const auto& level = payload["level"];
                if ((level.is_number_integer() && level.get<std::int32_t>() != 0) ||
                    (level.is_string() && level.get<std::string>() != "0")) continue;
            }
            if (payload.contains("hero_classes") && payload["hero_classes"].is_array()) {
                applicable = std::any_of(payload["hero_classes"].begin(), payload["hero_classes"].end(),
                    [&](const auto& value) { return value.is_string() && value.template get<std::string>() == hero_class.id; });
            }
        } catch (const nlohmann::json::exception&) { continue; }
        if (!applicable || !included.insert(raw_id).second) continue;
        Json::Value entry(Json::objectValue);
        entry["id"] = raw_id;
        const auto localized = content.resolve_localization("camping_skill_name_" + raw_id);
        const auto name = localized && localized.value() && !localized.value()->value.empty()
            ? localized.value()->value
            : (skill.localized_name.empty() ? skill.display_name : skill.localized_name);
        entry["name"] = strip_game_markup(name.empty() ? raw_id : name);
        const auto icon = std::find_if(skill.asset_references.begin(), skill.asset_references.end(),
            [](const auto& asset) { return asset.role == "skill_icon" && asset.resolved_asset.has_value(); });
        if (icon != skill.asset_references.end()) entry["iconPath"] = icon->virtual_path;
        else {
            const auto icon_path = "raid/camping/skill_icons/camp_skill_" + raw_id + ".png";
            const auto resolved = content.resolve_asset(icon_path);
            if (resolved && resolved.value()) entry["iconPath"] = icon_path;
        }
        result.append(std::move(entry));
    }
    return result;
}

void warm_hero_catalog(ServerContext& context, ServerContext::CampaignSession& campaign) {
    const auto started = std::chrono::steady_clock::now();
    const auto listed = campaign.content->list_content("hero_class");
    if (!listed) {
        append_hero_catalog_debug_log(context.configuration_store.current().data_root,
            "hero_class_catalog_failed", nullptr, &listed.error());
        return;
    }
    const auto skills = campaign.content->list_content("skill");
    if (!skills) {
        append_hero_catalog_debug_log(context.configuration_store.current().data_root,
            "hero_skill_catalog_failed", nullptr, &skills.error(), listed.value().size());
        return;
    }
    if (!campaign.cached_mod_records) {
        const auto queried = read_enabled_mods(campaign.mod_database_path);
        campaign.cached_mod_records = queried ? queried.value() : std::vector<DatabaseModRecord>{};
    }

    std::size_t rejected_count{};
    for (const auto& definition : listed.value()) {
        cache_hero_art(context, campaign, definition);
        cache_hero_upgrade_limits(context, campaign, definition);
        const auto starter = hero_starter_data(definition, skills.value());
        if (!starter) {
            ++rejected_count;
            append_hero_catalog_debug_log(context.configuration_store.current().data_root,
                "hero_class_rejected", &definition, &starter.error());
            continue;
        }
        Json::Value item(Json::objectValue);
        item["id"] = definition.id;
        item["name"] = strip_game_markup(definition.localized_name.empty()
            ? (definition.display_name.empty() ? definition.id : definition.display_name)
            : definition.localized_name);
        item["sourceId"] = definition.provenance.source_id;
        item["modName"] = definition.provenance.source_id;
        for (const auto& mod : *campaign.cached_mod_records) {
            if (mod.matched_mod_id == definition.provenance.source_id) {
                item["modName"] = mod.display_name.empty() ? mod.fallback_name : mod.display_name;
                break;
            }
        }
        for (const auto& asset : definition.asset_references) {
            auto asset_path = asset.virtual_path;
            std::transform(asset_path.begin(), asset_path.end(), asset_path.begin(), [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (asset_path.find("portrait_roster") != std::string::npos && !asset.virtual_path.empty()) {
                item["portraitPath"] = asset.virtual_path;
                break;
            }
        }
        for (const auto& asset : definition.asset_references) {
            auto path = asset.virtual_path;
            std::transform(path.begin(), path.end(), path.begin(), [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (path.ends_with(".sprite.idle.png") && !item.isMember("idleSpritePath"))
                item["idleSpritePath"] = asset.virtual_path;
            else if (path.ends_with(".sprite.idle.atlas") && !item.isMember("idleAtlasPath"))
                item["idleAtlasPath"] = asset.virtual_path;
            else if (path.ends_with(".sprite.idle.skel") && !item.isMember("idleSkeletonPath"))
                item["idleSkeletonPath"] = asset.virtual_path;
        }
        Json::Value combat_skills(Json::arrayValue);
        for (const auto& skill : starter.value().combat_skills) combat_skills.append(skill);
        Json::Value camping_skills(Json::arrayValue);
        for (const auto& skill : starter.value().camping_skills) camping_skills.append(skill);
        item["starterCombatSkills"] = std::move(combat_skills);
        item["starterCampingSkills"] = std::move(camping_skills);
        item["availableCampingSkills"] = available_camping_skills(definition, skills.value(), *campaign.content);
        item["baseHitPoints"] = starter.value().base_hit_points;
        campaign.cached_hero_catalog.push_back(std::move(item));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    append_hero_catalog_debug_log(context.configuration_store.current().data_root,
        "hero_class_catalog_summary", nullptr, nullptr, listed.value().size(), campaign.cached_hero_catalog.size());
    LOG_INFO << "Hero class catalog initialized " << campaign.cached_hero_catalog.size() << "/"
             << listed.value().size() << " classes; rejected=" << rejected_count
             << ", skills=" << skills.value().size() << ", elapsed_ms=" << elapsed;
}

void handle_campaign_hero_classes(const drogon::HttpRequestPtr& request,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                 const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    Json::Value result(Json::arrayValue);
    for (const auto& item : context->campaign->cached_hero_catalog) result.append(item);
    callback(json_ok(std::move(result)));
}

void handle_campaign_hero(const drogon::HttpRequestPtr& request,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                          const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    const auto body = request->getJsonObject();
    if (!body || !body->isObject() || !(*body)["action"].isString() ||
        !((*body)["revision"].isUInt() || (*body)["revision"].isUInt64())) {
        callback(json_error(drogon::k400BadRequest, "INVALID_HERO_OPERATION", "Hero operation requires an action and revision."));
        return;
    }
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    auto& campaign = *context->campaign;
    const auto& model = campaign.edits->model();
    const auto action = (*body)["action"].asString();
    const auto apply_progression = [&](application::CampaignOperation operation) {
        auto applied = campaign.edits->apply(operation, (*body)["revision"].asUInt64());
        if (!applied) {
            const auto status = applied.error().code == core::ErrorCode::StaleSessionRevision
                ? drogon::k409Conflict : drogon::k400BadRequest;
            callback(json_error(status, std::string{core::to_string(applied.error().code)}, applied.error().message));
        } else callback(json_ok(campaign_value(campaign)));
    };
    if (action == "level" || action == "equipment" || action == "combat" ||
        action == "camping_learn" || action == "camping_equip" || action == "maximize") {
        if (!(*body)["heroId"].isString()) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_OPERATION", "Hero progression requires a hero ID."));
            return;
        }
        const auto hero_id = (*body)["heroId"].asString();
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
            return item.persistent_id == hero_id;
        });
        if (hero == model.heroes.end() || !hero->class_id.value) {
            callback(json_error(drogon::k404NotFound, "HERO_NOT_FOUND", "The hero or class is unavailable."));
            return;
        }
        const auto class_id = *hero->class_id.value;
        const auto reject = [&](std::string message) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_PROGRESSION", std::move(message)));
        };
        const auto apply_planned = [&](core::Result<application::CampaignOperation, core::Error> planned) {
            if (!planned) {
                callback(json_error(drogon::k400BadRequest,
                    std::string{core::to_string(planned.error().code)}, planned.error().message));
            } else apply_progression(std::move(planned.value()));
        };
        if (action == "level") {
            const auto& requested = (*body)["level"];
            if (!requested.isInt() || requested.asInt() < 0 ||
                static_cast<std::size_t>(requested.asInt()) >= campaign.resolve_level_thresholds.size() ||
                !hero->resolve_xp.raw) {
                reject("Resolve level is outside the effective progression thresholds.");
                return;
            }
            apply_progression(application::SetCampaignValueOperation{
                {"Hero.ResolveXp", hero_id}, campaign.resolve_level_thresholds[requested.asInt()]});
            return;
        }
        if (action == "equipment") {
            const auto kind = (*body)["kind"].asString();
            const auto& requested = (*body)["rank"];
            const auto weapon_max = hero_upgrade_limit(campaign, class_id, "weapon");
            const auto armour_max = hero_upgrade_limit(campaign, class_id, "armour");
            if ((kind != "weapon" && kind != "armour") || !requested.isInt() || !weapon_max || !armour_max ||
                requested.asInt() < 0 || requested.asInt() > (kind == "weapon" ? *weapon_max : *armour_max) ||
                !hero->weapon_rank.value || !hero->armour_rank.value) {
                reject("Equipment rank is outside the effective hero upgrade trees.");
                return;
            }
            apply_planned(application::make_set_hero_equipment_ranks_operation(model, hero_id,
                kind == "weapon" ? requested.asInt() : *hero->weapon_rank.value,
                kind == "armour" ? requested.asInt() : *hero->armour_rank.value,
                *weapon_max, *armour_max));
            return;
        }
        if (action == "combat") {
            const auto skill_id = (*body)["skillId"].asString();
            const auto separator = skill_id.find(':');
            const auto suffix = separator == std::string::npos ? skill_id : skill_id.substr(separator + 1);
            const auto maximum = hero_upgrade_limit(campaign, class_id, suffix);
            const auto& requested = (*body)["rank"];
            if (skill_id.empty() || !requested.isInt() || !maximum || requested.asInt() < 1 ||
                requested.asInt() > *maximum ||
                std::none_of(hero->combat_skills.begin(), hero->combat_skills.end(),
                    [&](const auto& skill) { return skill.id == skill_id; })) {
                reject("Combat skill rank is outside its effective class upgrade tree.");
                return;
            }
            apply_planned(application::make_set_hero_combat_skill_rank_operation(
                model, hero_id, skill_id, requested.asInt(), *maximum));
            return;
        }
        if (action == "camping_learn" || action == "camping_equip") {
            const auto skill_id = (*body)["skillId"].asString();
            const auto catalog = std::find_if(campaign.cached_hero_catalog.begin(), campaign.cached_hero_catalog.end(),
                [&](const auto& item) { return item["id"].asString() == class_id; });
            const bool available = catalog != campaign.cached_hero_catalog.end() &&
                std::any_of((*catalog)["availableCampingSkills"].begin(),
                    (*catalog)["availableCampingSkills"].end(),
                    [&](const auto& skill) { return skill["id"].asString() == skill_id; });
            if (!available) {
                reject("Camping skill is not available to this hero class.");
                return;
            }
            if (action == "camping_learn") {
                apply_planned(application::make_set_hero_camping_skill_learned_operation(
                    model, hero_id, skill_id, true));
                return;
            }
            if (!(*body)["equipped"].isBool()) {
                reject("Camping equipment requires an equipped state.");
                return;
            }
            const bool equipped = (*body)["equipped"].asBool();
            const bool selected = std::any_of(hero->camping_skills.begin(), hero->camping_skills.end(),
                [&](const auto& skill) { return skill.id == skill_id; });
            const auto instance = hero_upgrade_instance(hero_id);
            const bool learned = selected || (instance && hero_purchased_rank(model, *instance,
                class_id + "." + skill_id, 1) > 0);
            if (equipped && !learned) {
                reject("Learn the camping skill before equipping it.");
                return;
            }
            if (equipped == selected) { callback(json_ok(campaign_value(campaign))); return; }
            apply_planned(application::make_set_hero_camping_skill_equipped_operation(
                model, hero_id, skill_id, equipped));
            return;
        }
        const auto weapon_max = hero_upgrade_limit(campaign, class_id, "weapon");
        const auto armour_max = hero_upgrade_limit(campaign, class_id, "armour");
        const auto catalog = std::find_if(campaign.cached_hero_catalog.begin(), campaign.cached_hero_catalog.end(),
            [&](const auto& item) { return item["id"].asString() == class_id; });
        if (!weapon_max || !armour_max || catalog == campaign.cached_hero_catalog.end()) {
            reject("Effective hero progression definitions are unavailable.");
            return;
        }
        std::vector<std::pair<std::string, std::int32_t>> combat_maxima;
        for (const auto& skill : hero->combat_skills) {
            const auto separator = skill.id.find(':');
            const auto suffix = separator == std::string::npos ? skill.id : skill.id.substr(separator + 1);
            const auto maximum = hero_upgrade_limit(campaign, class_id, suffix);
            if (maximum) combat_maxima.emplace_back(skill.id, *maximum);
        }
        std::vector<std::string> camping_ids;
        for (const auto& skill : (*catalog)["availableCampingSkills"])
            if (skill["id"].isString()) camping_ids.push_back(skill["id"].asString());
        apply_planned(application::make_maximize_hero_progression_operation(
            model, hero_id, *weapon_max, *armour_max, combat_maxima, camping_ids));
        return;
    }
    if (action == "rename") {
        if (!(*body)["heroId"].isString() || !(*body)["name"].isString()) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_OPERATION", "Rename requires a hero ID and name."));
            return;
        }
        const auto found = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& hero) {
            return hero.persistent_id == (*body)["heroId"].asString();
        });
        if (found == model.heroes.end() || !found->name.raw) {
            callback(json_error(drogon::k404NotFound, "HERO_NOT_FOUND", "The selected hero name cannot be edited."));
            return;
        }
        application::SetCampaignValueOperation operation{
            application::CampaignOperationTarget{"Hero.Name", found->persistent_id}, (*body)["name"].asString()};
        auto applied = campaign.edits->apply(application::CampaignOperation{std::move(operation)}, (*body)["revision"].asUInt64());
        if (!applied) {
            const auto status = applied.error().code == core::ErrorCode::StaleSessionRevision ? drogon::k409Conflict : drogon::k400BadRequest;
            callback(json_error(status, std::string{core::to_string(applied.error().code)}, applied.error().message));
            return;
        }
        callback(json_ok(campaign_value(campaign)));
        return;
    }

    using Kind = application::CampaignDocumentMutationKind;
    using Mutation = application::CampaignDocumentMutation;
    std::vector<Mutation> mutations;
    std::optional<std::uint64_t> pending_next_hero_guid;
    const auto root_path = [](std::string_view id) { return "base_root/heroes/" + std::string{id}; };
    if (action == "delete") {
        if (!(*body)["heroId"].isString()) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_OPERATION", "Delete requires a hero ID."));
            return;
        }
        const auto found = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& hero) {
            return hero.persistent_id == (*body)["heroId"].asString();
        });
        if (found == model.heroes.end()) {
            callback(json_error(drogon::k404NotFound, "HERO_NOT_FOUND", "The selected hero is no longer in the roster."));
            return;
        }
        const auto target = found->raw.display_path;
        for (const auto& [document_id, document] : campaign.profile.documents) {
            if (!document.decoded) continue;
            for (const auto& field : document.decoded->fields) {
                if (field.kind != core::dson::ValueKind::String ||
                    !std::holds_alternative<std::string>(field.value) ||
                    std::get<std::string>(field.value) != found->persistent_id) continue;
                if (document_id != "persist.roster.json" ||
                    (!field.path.starts_with(target + "/") && field.path != target)) {
                    callback(json_error(drogon::k409Conflict, "HERO_REFERENCED",
                        "This hero is referenced by another save field and cannot be safely removed."));
                    return;
                }
            }
        }
        mutations.emplace_back(Kind::Erase, "Hero.PersistentId", "persist.roster.json", target,
            std::string{}, std::string{}, core::dson::ValueKind::Object);
    } else if (action == "reorder") {
        const auto& ids = (*body)["heroIds"];
        if (!ids.isArray() || ids.size() != model.heroes.size()) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_ORDER", "The submitted hero order must contain every roster entry."));
            return;
        }
        std::set<std::string, std::less<>> requested;
        for (const auto& id : ids) if (!id.isString() || !requested.insert(id.asString()).second) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_ORDER", "The submitted hero order contains invalid or repeated IDs."));
            return;
        }
        if (requested.size() != model.heroes.size() || std::any_of(model.heroes.begin(), model.heroes.end(), [&](const auto& hero) {
                return !requested.contains(hero.persistent_id);
            })) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_ORDER", "The submitted hero order does not match the current roster."));
            return;
        }
        const auto suffix = std::to_string((*body)["revision"].asUInt64());
        for (Json::ArrayIndex i = 0; i < ids.size(); ++i) {
            const auto found = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& hero) {
                return hero.persistent_id == ids[i].asString();
            });
            const auto temporary_id = "ddse_order_" + suffix + "_" + std::to_string(i);
            mutations.emplace_back(Kind::AppendClone, "Hero.PersistentId", "persist.roster.json",
                root_path(temporary_id), found->raw.display_path, temporary_id, core::dson::ValueKind::Object);
        }
        for (const auto& hero : model.heroes)
            mutations.emplace_back(Kind::Erase, "Hero.PersistentId", "persist.roster.json",
                hero.raw.display_path, std::string{}, std::string{}, core::dson::ValueKind::Object);
        for (Json::ArrayIndex i = 0; i < ids.size(); ++i) {
            const auto original = ids[i].asString();
            const auto temporary_id = "ddse_order_" + suffix + "_" + std::to_string(i);
            mutations.emplace_back(Kind::Rename, "Hero.PersistentId", "persist.roster.json",
                root_path(temporary_id), std::string{}, original, core::dson::ValueKind::Object);
        }
    } else if (action == "add") {
        const auto& requested = (*body)["classIds"];
        if (!requested.isArray() || requested.empty()) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_SELECTION", "Select at least one hero class."));
            return;
        }
        std::uint64_t max_id = 0;
        for (const auto& hero : model.heroes) {
            std::uint64_t parsed{};
            const auto [end, error] = std::from_chars(hero.persistent_id.data(), hero.persistent_id.data() + hero.persistent_id.size(), parsed);
            if (error == std::errc{} && end == hero.persistent_id.data() + hero.persistent_id.size()) max_id = std::max(max_id, parsed);
        }
        std::set<std::string, std::less<>> added_classes;
        auto next_guid = std::max(max_id + 1, campaign.next_hero_guid);
        for (const auto& id : requested) {
            if (!id.isString() || !added_classes.insert(id.asString()).second) continue;
            const auto definition = std::find_if(campaign.cached_hero_catalog.begin(), campaign.cached_hero_catalog.end(),
                [&](const auto& item) { return item["id"].asString() == id.asString(); });
            if (definition == campaign.cached_hero_catalog.end()) continue;
            if (next_guid >= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) break;
            std::vector<std::string> combat_skills;
            if ((*definition)["starterCombatSkills"].isArray())
                for (const auto& skill : (*definition)["starterCombatSkills"])
                    if (skill.isString()) combat_skills.push_back(skill.asString());
            std::vector<std::string> camping_skills;
            if ((*definition)["starterCampingSkills"].isArray())
                for (const auto& skill : (*definition)["starterCampingSkills"])
                    if (skill.isString()) camping_skills.push_back(skill.asString());
            const auto class_id = (*definition)["id"].asString();
            const auto class_name = (*definition)["name"].asString();
            const auto source_id = (*definition)["sourceId"].asString();
            const auto base_hit_points = (*definition)["baseHitPoints"].asFloat();
            const auto template_document = application::build_blank_level_zero_hero_template(
                class_id, combat_skills, camping_skills, base_hit_points);
            if (!template_document) continue;
            const auto new_id = std::to_string(next_guid++);
            const auto target = root_path(new_id);
            application::CampaignDocumentMutation append{Kind::AppendTemplate, "Hero.PersistentId",
                "persist.roster.json", target, "base_root/heroes/1", new_id,
                core::dson::ValueKind::Object};
            append.template_document = template_document.value();
            append.template_hero_class = class_id;
            append.template_class_name = class_name;
            append.template_source_id = source_id;
            append.template_base_hit_points = base_hit_points;
            append.template_portrait_path = (*definition)["portraitPath"].asString();
            append.template_combat_skills = std::move(combat_skills);
            append.template_camping_skills = std::move(camping_skills);
            mutations.push_back(std::move(append));
        }
        if (mutations.empty()) {
            callback(json_error(drogon::k409Conflict, "HERO_TEMPLATE_UNAVAILABLE",
                "No selected class has enough effective resolve-level-zero equipment and skill definitions to build a hero."));
            return;
        }
        pending_next_hero_guid = next_guid;
    } else {
        callback(json_error(drogon::k400BadRequest, "INVALID_HERO_OPERATION", "Unknown hero operation."));
        return;
    }

    application::ApplyCampaignDocumentMutationsOperation operation{"campaign.hero.add", std::move(mutations)};
    if (action == "delete") operation.operation_id = "campaign.hero.delete";
    else if (action == "reorder") operation.operation_id = "campaign.hero.reorder";
    auto applied = campaign.edits->apply(application::CampaignOperation{std::move(operation)}, (*body)["revision"].asUInt64());
    if (!applied) {
        const auto status = applied.error().code == core::ErrorCode::StaleSessionRevision ? drogon::k409Conflict : drogon::k400BadRequest;
        callback(json_error(status, std::string{core::to_string(applied.error().code)}, applied.error().message));
        return;
    }
    if (pending_next_hero_guid) campaign.next_hero_guid = *pending_next_hero_guid;
    callback(json_ok(campaign_value(campaign)));
}

bool request_nonnegative_integer(const Json::Value& value);

void handle_campaign_hero_trinket(const drogon::HttpRequestPtr& request,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                 const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    const auto body = request->getJsonObject();
    if (!body || !body->isObject() || !(*body)["action"].isString() ||
        !((*body)["revision"].isUInt() || (*body)["revision"].isUInt64())) {
        callback(json_error(drogon::k400BadRequest, "INVALID_HERO_TRINKET", "Action and revision are required."));
        return;
    }
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    auto& campaign = *context->campaign;
    const auto& model = campaign.edits->model();
    const auto action = (*body)["action"].asString();
    const auto revision = (*body)["revision"].asUInt64();
    if (revision != campaign.edits->revision()) {
        callback(json_error(drogon::k409Conflict, "STALE_SESSION_REVISION", "Campaign revision has changed."));
        return;
    }
    using Kind = application::CampaignDocumentMutationKind;
    using Mutation = application::CampaignDocumentMutation;
    std::vector<Mutation> mutations;
    const auto erase_hero = [&](const domain::HeroTrinket& item) {
        mutations.emplace_back(Kind::Erase, "Hero.Trinkets", "persist.roster.json",
            item.raw.display_path, std::string{}, std::string{}, core::dson::ValueKind::Object);
    };
    const auto append_metadata = [&](const std::string& property, const std::string& document,
                                     const std::string& target,
                                     const domain::TrinketRecordMetadata& metadata) {
        const auto add = [&](const char* field, core::dson::ValueKind kind,
                             application::CampaignValue before, application::CampaignValue after) {
            if (before == after) return;
            mutations.emplace_back(Kind::SetValue, property, document, target + "/" + field,
                std::string{}, std::string{}, kind, std::move(before), std::move(after));
        };
        add("type", core::dson::ValueKind::String, std::string{"trinket"}, metadata.type);
        add("amount", core::dson::ValueKind::Integer, std::int32_t{1}, metadata.amount);
        add("added_buffs", core::dson::ValueKind::Integer, std::int32_t{0}, metadata.added_buffs);
        add("hero_name", core::dson::ValueKind::String, std::string{}, metadata.hero_name);
        add("previous_trinket_id", core::dson::ValueKind::String, std::string{}, metadata.previous_trinket_id);
        add("did_transform", core::dson::ValueKind::Boolean, false, metadata.did_transform);
        add("trinkets_gained_count", core::dson::ValueKind::Integer,
            std::int32_t{0}, metadata.trinkets_gained_count);
    };
    std::optional<std::uint32_t> new_limit;
    std::optional<std::pair<std::string, std::string>> reserved_hero_key;
    std::optional<std::string> reserved_chest_key;
    if (action == "set_limit") {
        const auto& requested = (*body)["limit"];
        if (!request_nonnegative_integer(requested) || requested.asUInt64() > 1000) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_TRINKET_LIMIT", "Slot limit must be between 0 and 1000."));
            return;
        }
        new_limit = requested.asUInt();
        for (const auto& hero : model.heroes)
            for (std::size_t i = hero.trinkets.size(); i > *new_limit; --i)
                erase_hero(hero.trinkets[i - 1]);
    } else {
        const auto hero_id = (*body)["heroId"].isString() ? (*body)["heroId"].asString() : std::string{};
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(),
            [&](const auto& item) { return item.persistent_id == hero_id; });
        if (hero == model.heroes.end()) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_TRINKET", "Hero is required."));
            return;
        }
        if (action == "reorder") {
            const auto& keys = (*body)["rawKeys"];
            std::vector<std::string> desired;
            std::set<std::string, std::less<>> unique;
            if (!keys.isArray() || keys.size() != hero->trinkets.size()) {
                callback(json_error(drogon::k400BadRequest, "INVALID_HERO_TRINKET_ORDER", "A complete trinket order is required."));
                return;
            }
            for (const auto& key : keys) {
                if (!key.isString() || !unique.insert(key.asString()).second) {
                    callback(json_error(drogon::k400BadRequest, "INVALID_HERO_TRINKET_ORDER", "Trinket keys must be unique."));
                    return;
                }
                desired.push_back(key.asString());
            }
            std::set<std::string, std::less<>> current;
            auto& reserved = campaign.reserved_hero_trinket_keys[hero_id];
            std::size_t temporary = 0;
            for (const auto& item : hero->trinkets) {
                const auto key = item.raw.display_path.substr(item.raw.display_path.find_last_of('/') + 1);
                current.insert(key);
            }
            if (current != unique) {
                callback(json_error(drogon::k400BadRequest, "INVALID_HERO_TRINKET_ORDER", "Trinket order does not match the hero."));
                return;
            }
            bool unchanged = true;
            for (std::size_t i = 0; i < desired.size(); ++i)
                if (desired[i] != hero->trinkets[i].raw.display_path.substr(
                    hero->trinkets[i].raw.display_path.find_last_of('/') + 1)) unchanged = false;
            if (unchanged) { callback(json_ok(campaign_value(campaign))); return; }
            for (const auto& key : reserved) {
                std::size_t parsed{};
                const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), parsed);
                if (error == std::errc{} && end == key.data() + key.size()) temporary = std::max(temporary, parsed + 1);
            }
            std::map<std::string, std::string, std::less<>> temporary_by_original;
            for (const auto& item : hero->trinkets) {
                const auto original = item.raw.display_path.substr(item.raw.display_path.find_last_of('/') + 1);
                const auto key = std::to_string(temporary++);
                temporary_by_original[original] = key;
                mutations.emplace_back(Kind::Rename, "Hero.Trinkets", "persist.roster.json",
                    item.raw.display_path, std::string{}, key, core::dson::ValueKind::Object);
            }
            const auto base = "base_root/heroes/" + hero_id +
                "/hero_file_data/raw_data => base_root/trinkets/items/";
            for (std::size_t i = 0; i < desired.size(); ++i)
                mutations.emplace_back(Kind::Rename, "Hero.Trinkets", "persist.roster.json",
                    base + temporary_by_original.at(desired[i]), std::string{}, std::to_string(i),
                    core::dson::ValueKind::Object);
        } else {
        if (!(*body)["slotIndex"].isUInt()) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_TRINKET", "Slot is required."));
            return;
        }
        const auto slot = static_cast<std::size_t>((*body)["slotIndex"].asUInt());
        const bool occupied = slot < hero->trinkets.size();
        if (slot >= campaign.hero_trinket_slot_limit && (action == "equip" || !occupied)) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_TRINKET_SLOT", "Slot is outside the configured limit."));
            return;
        }
        if (action == "return" || action == "destroy") {
            if (!occupied) {
                callback(json_error(drogon::k400BadRequest, "EMPTY_HERO_TRINKET_SLOT", "The selected slot is empty."));
                return;
            }
            if (action == "return") {
                std::set<std::string, std::less<>> keys = campaign.reserved_trinket_keys;
                for (const auto& item : model.trinket_inventory) keys.insert(item.raw_key);
                std::size_t next = 0;
                for (const auto& key : keys) {
                    std::size_t parsed{};
                    const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), parsed);
                    if (error == std::errc{} && end == key.data() + key.size()) next = std::max(next, parsed + 1);
                }
                while (keys.contains(std::to_string(next))) ++next;
                const auto key = std::to_string(next);
                reserved_chest_key = key;
                const auto target = "base_root/trinkets/items/" + key;
                mutations.emplace_back(Kind::CreateObject, "TrinketInventory.Items", "persist.estate.json",
                    target, std::string{}, key, core::dson::ValueKind::Object);
                mutations.emplace_back(Kind::SetValue, "TrinketInventory.Items", "persist.estate.json",
                    target + "/id", std::string{}, std::string{}, core::dson::ValueKind::String,
                    application::CampaignValue{std::string{}}, application::CampaignValue{hero->trinkets[slot].id});
                append_metadata("TrinketInventory.Items", "persist.estate.json", target,
                    hero->trinkets[slot].metadata);
            }
            erase_hero(hero->trinkets[slot]);
        } else if (action == "equip") {
            const auto source = (*body).get("source", "inventory").asString();
            std::string id;
            std::optional<domain::TrinketInventoryEntry> inventory_item;
            if (source == "inventory") {
                const auto key = (*body)["rawKey"].isString() ? (*body)["rawKey"].asString() : std::string{};
                const auto found = std::find_if(model.trinket_inventory.begin(), model.trinket_inventory.end(),
                    [&](const auto& item) { return item.raw_key == key; });
                if (found == model.trinket_inventory.end() || !found->id.value) {
                    callback(json_error(drogon::k404NotFound, "TRINKET_NOT_FOUND", "Inventory trinket is unavailable."));
                    return;
                }
                inventory_item = *found;
                id = *found->id.value;
            } else if (source == "catalog") {
                id = (*body)["trinketId"].isString() ? (*body)["trinketId"].asString() : std::string{};
            } else {
                callback(json_error(drogon::k400BadRequest, "INVALID_TRINKET_SOURCE", "Unknown trinket source."));
                return;
            }
            const auto definition = std::find_if(campaign.trinket_definitions.begin(), campaign.trinket_definitions.end(),
                [&](const auto& item) { return item.id == id; });
            if (id.empty() || (source == "catalog" && definition == campaign.trinket_definitions.end())) {
                callback(json_error(drogon::k404NotFound, "TRINKET_NOT_FOUND", "Effective trinket definition is unavailable."));
                return;
            }
            if (definition != campaign.trinket_definitions.end()) {
                std::vector<std::string> required_classes;
                for (const auto& relation : definition->relationships)
                    if (relation.relationship_type == "restricted_to" && relation.type == "hero_class")
                        required_classes.push_back(relation.id);
                if (!required_classes.empty() && (!hero->class_id.value ||
                    std::find(required_classes.begin(), required_classes.end(), *hero->class_id.value) == required_classes.end())) {
                    callback(json_error(drogon::k400BadRequest, "TRINKET_CLASS_RESTRICTED", "This trinket cannot be equipped by the hero class."));
                    return;
                }
            }
            if (std::any_of(hero->trinkets.begin(), hero->trinkets.end(),
                [&](const auto& item) { return item.id == id; })) {
                callback(json_error(drogon::k400BadRequest, "DUPLICATE_HERO_TRINKET", "The hero already has this trinket equipped."));
                return;
            }
            std::string target;
            if (occupied) {
                target = hero->trinkets[slot].raw.display_path;
                const auto key = target.substr(target.find_last_of('/') + 1);
                erase_hero(hero->trinkets[slot]);
                mutations.emplace_back(Kind::CreateObject, "Hero.Trinkets", "persist.roster.json",
                    target, std::string{}, key, core::dson::ValueKind::Object);
                mutations.emplace_back(Kind::SetValue, "Hero.Trinkets", "persist.roster.json",
                    target + "/id", std::string{}, std::string{}, core::dson::ValueKind::String,
                    application::CampaignValue{std::string{}}, application::CampaignValue{id});
            } else {
                auto& keys = campaign.reserved_hero_trinket_keys[hero_id];
                std::size_t next = 0;
                for (const auto& item : hero->trinkets) {
                    const auto key = item.raw.display_path.substr(item.raw.display_path.find_last_of('/') + 1);
                    keys.insert(key);
                }
                for (const auto& key : keys) {
                    std::size_t parsed{};
                    const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), parsed);
                    if (error == std::errc{} && end == key.data() + key.size()) next = std::max(next, parsed + 1);
                }
                while (keys.contains(std::to_string(next))) ++next;
                const auto key = std::to_string(next);
                reserved_hero_key = std::pair{hero_id, key};
                target = "base_root/heroes/" + hero_id +
                    "/hero_file_data/raw_data => base_root/trinkets/items/" + key;
                mutations.emplace_back(Kind::CreateObject, "Hero.Trinkets", "persist.roster.json",
                    target, std::string{}, key, core::dson::ValueKind::Object);
                mutations.emplace_back(Kind::SetValue, "Hero.Trinkets", "persist.roster.json",
                    target + "/id", std::string{}, std::string{}, core::dson::ValueKind::String,
                    application::CampaignValue{std::string{}}, application::CampaignValue{id});
            }
            if (inventory_item) {
                append_metadata("Hero.Trinkets", "persist.roster.json", target,
                    inventory_item->metadata);
                mutations.emplace_back(Kind::Erase, "TrinketInventory.Items", "persist.estate.json",
                    inventory_item->raw.display_path, std::string{}, std::string{}, core::dson::ValueKind::Object);
            }
        } else {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_TRINKET", "Unknown trinket action."));
            return;
        }
        }
    }
    if (new_limit) {
        auto configuration = context->configuration_store.current();
        configuration.hero_trinket_slot_limit = *new_limit;
        const auto saved = context->configuration_store.save(configuration);
        if (!saved) {
            callback(json_error(drogon::k400BadRequest, "INVALID_CONFIGURATION", saved.error().message));
            return;
        }
    }
    if (!mutations.empty()) {
        application::ApplyCampaignDocumentMutationsOperation operation{
            "campaign.hero.edit_trinkets", std::move(mutations)};
        const auto applied = campaign.edits->apply(application::CampaignOperation{std::move(operation)}, revision);
        if (!applied) {
            if (new_limit) {
                auto previous = context->configuration_store.current();
                previous.hero_trinket_slot_limit = campaign.hero_trinket_slot_limit;
                (void)context->configuration_store.save(previous);
            }
            const auto status = applied.error().code == core::ErrorCode::StaleSessionRevision
                ? drogon::k409Conflict : drogon::k400BadRequest;
            callback(json_error(status, std::string{core::to_string(applied.error().code)}, applied.error().message));
            return;
        }
    }
    if (new_limit) campaign.hero_trinket_slot_limit = *new_limit;
    if (reserved_hero_key) campaign.reserved_hero_trinket_keys[reserved_hero_key->first].insert(reserved_hero_key->second);
    if (reserved_chest_key) campaign.reserved_trinket_keys.insert(*reserved_chest_key);
    callback(json_ok(campaign_value(campaign)));
}

void handle_campaign_quirk_catalog(const drogon::HttpRequestPtr& request,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                   const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    Json::Value result(Json::arrayValue);
    for (const auto& quirk : context->campaign->cached_quirk_catalog) result.append(quirk);
    callback(json_ok(std::move(result)));
}

void handle_campaign_hero_quirk(const drogon::HttpRequestPtr& request,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    const auto body = request->getJsonObject();
    if (!body || !(*body)["action"].isString() ||
        !((*body)["revision"].isUInt() || (*body)["revision"].isUInt64())) {
        callback(json_error(drogon::k400BadRequest, "INVALID_HERO_QUIRK", "Action and revision are required."));
        return;
    }
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    auto& campaign = *context->campaign;
    const auto revision = (*body)["revision"].asUInt64();
    if (revision != campaign.edits->revision()) {
        callback(json_error(drogon::k409Conflict, "STALE_SESSION_REVISION", "Campaign revision has changed."));
        return;
    }
    const auto action = (*body)["action"].asString();
    const auto polarity = (*body)["polarity"].asString();
    if (polarity != "positive" && polarity != "negative") {
        callback(json_error(drogon::k400BadRequest, "INVALID_HERO_QUIRK", "Positive or negative polarity is required."));
        return;
    }
    const bool positive = polarity == "positive";
    const auto matches = [&](const domain::HeroQuirk& quirk) {
        return !quirk.is_disease && quirk.polarity == (positive
            ? domain::QuirkPolarity::Positive : domain::QuirkPolarity::Negative);
    };
    const auto& model = campaign.edits->model();
    using Kind = application::CampaignDocumentMutationKind;
    using Mutation = application::CampaignDocumentMutation;
    std::vector<Mutation> mutations;
    std::optional<std::uint32_t> new_limit;
    if (action == "set_limit") {
        const auto& requested = (*body)["limit"];
        if (!request_nonnegative_integer(requested) || requested.asUInt64() > 1000) {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_QUIRK_LIMIT", "Quirk limit must be between 0 and 1000."));
            return;
        }
        new_limit = requested.asUInt();
        for (const auto& hero : model.heroes) {
            std::size_t count = static_cast<std::size_t>(std::count_if(hero.quirks.begin(), hero.quirks.end(), matches));
            for (auto it = hero.quirks.rbegin(); it != hero.quirks.rend() && count > *new_limit; ++it)
                if (matches(*it)) {
                    mutations.emplace_back(Kind::Erase, "Hero.Quirks", "persist.roster.json",
                        it->raw.display_path, std::string{}, std::string{}, core::dson::ValueKind::Object);
                    --count;
                }
        }
    } else {
        const auto hero_id = (*body)["heroId"].asString();
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(),
            [&](const auto& item) { return item.persistent_id == hero_id; });
        if (hero == model.heroes.end()) {
            callback(json_error(drogon::k404NotFound, "HERO_NOT_FOUND", "Hero is unavailable."));
            return;
        }
        const auto old_id = (*body)["oldId"].asString();
        const auto existing = std::find_if(hero->quirks.begin(), hero->quirks.end(),
            [&](const auto& item) { return item.id == old_id && matches(item); });
        if (action == "remove" || action == "lock") {
            if (existing == hero->quirks.end()) {
                callback(json_error(drogon::k404NotFound, "QUIRK_NOT_FOUND", "Quirk is unavailable in this category."));
                return;
            }
            if (action == "lock" && !positive) {
                callback(json_error(drogon::k400BadRequest, "QUIRK_NOT_LOCKABLE", "Only eligible positive quirks can be locked."));
                return;
            }
            if (action == "remove") {
                mutations.emplace_back(Kind::Erase, "Hero.Quirks", "persist.roster.json",
                    existing->raw.display_path, std::string{}, std::string{}, core::dson::ValueKind::Object);
            } else {
                const auto catalog = std::find_if(campaign.cached_quirk_catalog.begin(), campaign.cached_quirk_catalog.end(),
                    [&](const auto& item) { return item["id"].asString() == old_id; });
                if (catalog == campaign.cached_quirk_catalog.end() || !(*catalog)["canLock"].asBool() ||
                    !existing->is_locked.value) {
                    callback(json_error(drogon::k400BadRequest, "QUIRK_NOT_LOCKABLE", "The quirk cannot be locked."));
                    return;
                }
                mutations.emplace_back(Kind::SetValue, "Hero.Quirks", "persist.roster.json",
                    existing->raw.display_path + "/is_locked", std::string{}, std::string{},
                    core::dson::ValueKind::Boolean, *existing->is_locked.value,
                    !*existing->is_locked.value);
            }
        }
        if (action != "add" && action != "replace" && action != "lock" && action != "remove") {
            callback(json_error(drogon::k400BadRequest, "INVALID_HERO_QUIRK", "Unknown quirk action."));
            return;
        }
        if (action == "add" || action == "replace") {
        if (action == "replace" && existing == hero->quirks.end()) {
            callback(json_error(drogon::k404NotFound, "QUIRK_NOT_FOUND", "Replacement target is unavailable."));
            return;
        }
        const auto new_id = (*body)["quirkId"].asString();
        const auto definition = std::find_if(campaign.quirk_definitions.begin(), campaign.quirk_definitions.end(),
            [&](const auto& item) { return item.id == new_id; });
        const auto catalog = std::find_if(campaign.cached_quirk_catalog.begin(), campaign.cached_quirk_catalog.end(),
            [&](const auto& item) { return item["id"].asString() == new_id &&
                item["polarity"].asString() == polarity; });
        if (definition == campaign.quirk_definitions.end() || catalog == campaign.cached_quirk_catalog.end() ||
            std::any_of(hero->quirks.begin(), hero->quirks.end(),
                [&](const auto& item) { return item.id == new_id; })) {
            callback(json_error(drogon::k400BadRequest, "INVALID_QUIRK_SELECTION", "Quirk is unavailable, has the wrong polarity, or is already present."));
            return;
        }
        const auto count = std::count_if(hero->quirks.begin(), hero->quirks.end(), matches);
        const auto limit = positive ? campaign.hero_positive_quirk_limit : campaign.hero_negative_quirk_limit;
        if (action == "add" && static_cast<std::uint32_t>(count) >= limit) {
            callback(json_error(drogon::k400BadRequest, "HERO_QUIRK_LIMIT", "The configured quirk limit has been reached."));
            return;
        }
        const auto target = hero->raw.display_path +
            "/hero_file_data/raw_data => base_root/quirks/" + new_id;
        if (positive && action == "replace") {
            Mutation renamed{Kind::Rename, "Hero.Quirks", "persist.roster.json",
                existing->raw.display_path, {}, new_id, core::dson::ValueKind::Object};
            renamed.quirk_positive = true;
            renamed.quirk_name = (*catalog)["name"].asString();
            renamed.quirk_source_id = definition->provenance.source_id;
            renamed.quirk_payload_json = definition->payload_json;
            mutations.push_back(std::move(renamed));
            const auto reset = [&](std::string_view name, core::dson::ValueKind kind,
                                   std::optional<application::CampaignValue> before,
                                   application::CampaignValue after) {
                if (!before || *before == after) return;
                mutations.emplace_back(Kind::SetValue, "Hero.Quirks", "persist.roster.json",
                    target + "/" + std::string{name}, std::string{}, std::string{}, kind, *before, std::move(after));
            };
            reset("is_new", core::dson::ValueKind::Boolean, existing->is_new.value, true);
            reset("is_locked", core::dson::ValueKind::Boolean, existing->is_locked.value, false);
            reset("trinketId", core::dson::ValueKind::Integer, existing->trinket_id.value, std::int32_t{0});
            reset("mission_count", core::dson::ValueKind::Integer, existing->mission_count.value, std::int32_t{0});
            reset("replaces_quirk", core::dson::ValueKind::Integer, existing->replaces_quirk.value, std::int32_t{0});
            reset("replaces_quirk_viewed", core::dson::ValueKind::Boolean, existing->replaces_quirk_viewed.value, false);
            reset("evolution_duration_remaining", core::dson::ValueKind::Integer,
                existing->evolution_duration_remaining.value, std::int32_t{0});
        } else {
            if (action == "replace")
                mutations.emplace_back(Kind::Erase, "Hero.Quirks", "persist.roster.json",
                    existing->raw.display_path, std::string{}, std::string{}, core::dson::ValueKind::Object);
            Mutation created{Kind::CreateObject, "Hero.Quirks", "persist.roster.json",
                target, {}, new_id, core::dson::ValueKind::Object};
            if (action == "replace")
                created.insertion_index = static_cast<std::size_t>(
                    std::distance(hero->quirks.begin(), existing));
            created.quirk_positive = positive;
            created.quirk_name = (*catalog)["name"].asString();
            created.quirk_source_id = definition->provenance.source_id;
            created.quirk_payload_json = definition->payload_json;
            mutations.push_back(std::move(created));
        }
        }
    }
    if (new_limit) {
        auto configuration = context->configuration_store.current();
        if (positive) configuration.hero_positive_quirk_limit = *new_limit;
        else configuration.hero_negative_quirk_limit = *new_limit;
        const auto saved = context->configuration_store.save(configuration);
        if (!saved) {
            callback(json_error(drogon::k400BadRequest, "INVALID_CONFIGURATION", saved.error().message));
            return;
        }
    }
    if (!mutations.empty()) {
        auto applied = campaign.edits->apply(application::CampaignOperation{
            application::ApplyCampaignDocumentMutationsOperation{
                "campaign.hero.add_or_replace_quirk", std::move(mutations)}}, revision);
        if (!applied) {
            if (new_limit) {
                auto previous = context->configuration_store.current();
                if (positive) previous.hero_positive_quirk_limit = campaign.hero_positive_quirk_limit;
                else previous.hero_negative_quirk_limit = campaign.hero_negative_quirk_limit;
                (void)context->configuration_store.save(previous);
            }
            callback(json_error(drogon::k400BadRequest,
                std::string{core::to_string(applied.error().code)}, applied.error().message));
            return;
        }
    }
    if (new_limit) {
        if (positive) campaign.hero_positive_quirk_limit = *new_limit;
        else campaign.hero_negative_quirk_limit = *new_limit;
    }
    callback(json_ok(campaign_value(campaign)));
}

void handle_campaign_trinket_catalog(const drogon::HttpRequestPtr& request,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                     const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    auto& campaign = *context->campaign;
    const auto search = request->getParameter("search");
    const auto mod_filter = request->getParameter("mod");
    const auto class_filter = request->getParameter("class");
    Json::Value result(Json::arrayValue);
    for (const auto& cached_item : campaign.cached_trinket_catalog) {
        Json::Value item = cached_item;
        const auto search_lower = [](std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        };
        const auto needle = search_lower(search);
        if (!needle.empty()) {
            std::string haystack = item["id"].asString() + " " + item["localizationKey"].asString() + " " +
                item["name"].asString() + " " + item["englishName"].asString() + " " +
                item["description"].asString() + " " + item["englishDescription"].asString() + " " +
                item["modName"].asString() + " " + item["effectSearchText"].asString();
            if (search_lower(std::move(haystack)).find(needle) == std::string::npos) continue;
        }
        if (!mod_filter.empty() && item["sourceId"].asString() != mod_filter) continue;
        if (!class_filter.empty()) {
            const auto& classes = item["heroClasses"];
            bool matches = false;
            for (const auto& hero_class : classes)
                if (hero_class.asString() == class_filter) matches = true;
            if (!matches) continue;
        }
        result.append(std::move(item));
    }
    callback(json_ok(std::move(result)));
}

void handle_campaign_trinket_edit(const drogon::HttpRequestPtr& request,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                  const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    const auto body = request->getJsonObject();
    if (!body || !body->isObject() || !(*body)["action"].isString() ||
        !request_nonnegative_integer((*body)["revision"])) {
        callback(json_error(drogon::k400BadRequest, "INVALID_CAMPAIGN_OPERATION",
                            "Trinket operation requires an action and revision."));
        return;
    }
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    auto& campaign = *context->campaign;
    const auto& model = campaign.edits->model();
    const auto action = (*body)["action"].asString();
    using Kind = application::CampaignDocumentMutationKind;
    using Mutation = application::CampaignDocumentMutation;
    std::vector<Mutation> mutations;
    std::string operation_id;
    Json::UInt added = 0;
    Json::UInt deleted = 0;

    if (action == "add" || action == "batch_add") {
        operation_id = "campaign.trinket.add_inventory";
        std::set<std::string, std::less<>> existing_ids;
        std::set<std::string, std::less<>> occupied_keys;
        std::size_t next_index = 0;
        for (const auto& entry : model.trinket_inventory) {
            if (entry.id.value) existing_ids.insert(*entry.id.value);
            occupied_keys.insert(entry.raw_key);
        }
        // Fill the first actual hole in the loaded inventory after preserving
        // any keys reserved by pending deletes in this editing session.
        occupied_keys.insert(campaign.reserved_trinket_keys.begin(), campaign.reserved_trinket_keys.end());
        const auto mod_id = (*body)["modId"].isString() ? (*body)["modId"].asString() : std::string{};
        const auto hero_class = (*body)["heroClass"].isString() ? (*body)["heroClass"].asString() : std::string{};
        const bool only_new = (*body)["onlyNew"].isBool() ? (*body)["onlyNew"].asBool() : false;
        const auto requested_id = (*body)["trinketId"].isString() ? (*body)["trinketId"].asString() : std::string{};
        const bool has_requested_ids = action == "batch_add" && (*body).isMember("trinketIds");
        std::set<std::string, std::less<>> requested_ids;
        if (has_requested_ids) {
            const auto& ids = (*body)["trinketIds"];
            if (!ids.isArray()) {
                callback(json_error(drogon::k400BadRequest, "INVALID_TRINKET_SELECTION",
                                    "The selected trinket IDs must be an array."));
                return;
            }
            for (const auto& id : ids) {
                if (!id.isString() || id.asString().empty()) {
                    callback(json_error(drogon::k400BadRequest, "INVALID_TRINKET_SELECTION",
                                        "The selected trinket IDs contain an invalid value."));
                    return;
                }
                requested_ids.insert(id.asString());
            }
        }
        for (const auto& definition : campaign.trinket_definitions) {
            if (action == "add" && definition.id != requested_id) continue;
            if (has_requested_ids && !requested_ids.contains(definition.id)) continue;
            if (!has_requested_ids && !mod_id.empty() && definition.provenance.source_id != mod_id) continue;
            if (!has_requested_ids && !hero_class.empty() && std::none_of(definition.relationships.begin(), definition.relationships.end(),
                [&](const auto& relation) {
                    return relation.relationship_type == "restricted_to" && relation.type == "hero_class" &&
                           relation.id == hero_class;
                })) continue;
            if (only_new && existing_ids.contains(definition.id)) continue;
            while (occupied_keys.contains(std::to_string(next_index))) ++next_index;
            const auto key = std::to_string(next_index++);
            occupied_keys.insert(key);
            campaign.reserved_trinket_keys.insert(key);
            const auto target = "base_root/trinkets/items/" + key;
            // Trinket entries use the reverse-engineered built-in DSON schema
            // for every add, so an empty inventory and populated inventory take
            // the same path and never borrow an unrelated saved entry.
            mutations.emplace_back(Kind::CreateObject, "TrinketInventory.Items", "persist.estate.json",
                target, std::string{}, key, core::dson::ValueKind::Object);
            mutations.emplace_back(Kind::SetValue, "TrinketInventory.Items", "persist.estate.json",
                target + "/id", std::string{}, std::string{}, core::dson::ValueKind::String,
                application::CampaignValue{std::string{}}, application::CampaignValue{definition.id});
            existing_ids.insert(definition.id);
            ++added;
            if (action == "add") break;
        }
        if (mutations.empty()) {
            callback(json_error(drogon::k404NotFound, "TRINKET_NOT_FOUND", "No trinkets match the requested filters."));
            return;
        }
    } else if (action == "delete" || action == "batch_delete") {
        operation_id = "campaign.trinket.destroy";
        std::map<std::string, std::pair<std::string, std::vector<std::string>>, std::less<>> metadata;
        for (const auto& definition : campaign.trinket_definitions) {
            std::vector<std::string> classes;
            for (const auto& relation : definition.relationships)
                if (relation.relationship_type == "restricted_to" && relation.type == "hero_class")
                    classes.push_back(relation.id);
            metadata.emplace(definition.id, std::pair{definition.provenance.source_id, std::move(classes)});
        }
        const auto requested_key = (*body)["rawKey"].isString() ? (*body)["rawKey"].asString() : std::string{};
        const auto requested_id = (*body)["trinketId"].isString() ? (*body)["trinketId"].asString() : std::string{};
        const auto mod_id = (*body)["modId"].isString() ? (*body)["modId"].asString() : std::string{};
        const auto hero_class = (*body)["heroClass"].isString() ? (*body)["heroClass"].asString() : std::string{};
        const bool has_requested_keys = action == "batch_delete" && (*body).isMember("rawKeys");
        std::set<std::string, std::less<>> requested_keys;
        if (has_requested_keys) {
            const auto& keys = (*body)["rawKeys"];
            if (!keys.isArray()) {
                callback(json_error(drogon::k400BadRequest, "INVALID_TRINKET_SELECTION",
                                    "The selected inventory keys must be an array."));
                return;
            }
            for (const auto& key : keys) {
                if (!key.isString() || key.asString().empty()) {
                    callback(json_error(drogon::k400BadRequest, "INVALID_TRINKET_SELECTION",
                                        "The selected inventory keys contain an invalid value."));
                    return;
                }
                requested_keys.insert(key.asString());
            }
        }
        for (const auto& entry : model.trinket_inventory) {
            if (action == "delete" && entry.raw_key != requested_key) continue;
            if (action == "batch_delete") {
                if (has_requested_keys && !requested_keys.contains(entry.raw_key)) continue;
                const auto id = entry.id.value.value_or(entry.definition.raw_id);
                const auto found = metadata.find(id);
                if (!has_requested_keys && found == metadata.end()) {
                    if (!mod_id.empty() || !hero_class.empty()) continue;
                } else if (!has_requested_keys) {
                    if (!mod_id.empty() && found->second.first != mod_id) continue;
                    if (!hero_class.empty() && std::find(found->second.second.begin(), found->second.second.end(), hero_class) == found->second.second.end()) continue;
                }
                if (!requested_id.empty() && id != requested_id) continue;
            }
            mutations.emplace_back(Kind::Erase, "TrinketInventory.Items", "persist.estate.json",
                entry.raw.display_path, std::string{}, std::string{}, core::dson::ValueKind::Object);
            ++deleted;
        }
        if (mutations.empty()) {
            callback(json_ok(campaign_value(campaign)));
            return;
        }
    } else if (action == "reorder") {
        operation_id = "campaign.trinket.reorder_inventory";
        const auto& keys = (*body)["rawKeys"];
        if (!keys.isArray() || keys.size() != model.trinket_inventory.size()) {
            callback(json_error(drogon::k400BadRequest, "INVALID_TRINKET_ORDER", "The submitted order must include every trinket exactly once."));
            return;
        }
        std::vector<std::string> desired;
        std::set<std::string, std::less<>> unique;
        for (const auto& key : keys) {
            if (!key.isString() || !unique.insert(key.asString()).second) {
                callback(json_error(drogon::k400BadRequest, "INVALID_TRINKET_ORDER", "The trinket order contains invalid or duplicate keys."));
                return;
            }
            desired.push_back(key.asString());
        }
        std::set<std::string, std::less<>> current;
        std::size_t temp = 0;
        for (const auto& entry : model.trinket_inventory) {
            current.insert(entry.raw_key);
            std::size_t numeric_key{};
            const auto [end, error] = std::from_chars(entry.raw_key.data(),
                entry.raw_key.data() + entry.raw_key.size(), numeric_key);
            if (entry.raw_key.empty() || error != std::errc{} || end != entry.raw_key.data() + entry.raw_key.size()) {
                callback(json_error(drogon::k409Conflict, "TRINKET_ORDER_UNAVAILABLE",
                                    "The save contains a nonnumeric trinket entry key that cannot be safely reordered."));
                return;
            }
            temp = std::max(temp, numeric_key + 1);
        }
        if (current != unique) {
            callback(json_error(drogon::k400BadRequest, "INVALID_TRINKET_ORDER", "The trinket order does not match the current inventory."));
            return;
        }
        std::map<std::string, std::string, std::less<>> temporary_by_original;
        for (const auto& entry : model.trinket_inventory) {
            const auto temporary_key = std::to_string(temp++);
            temporary_by_original[entry.raw_key] = temporary_key;
            mutations.emplace_back(Kind::Rename, "TrinketInventory.Items", "persist.estate.json",
                entry.raw.display_path, std::string{}, temporary_key, core::dson::ValueKind::Object);
        }
        for (std::size_t i = 0; i < desired.size(); ++i) {
            const auto temporary_key = temporary_by_original.at(desired[i]);
            mutations.emplace_back(Kind::Rename, "TrinketInventory.Items", "persist.estate.json",
                "base_root/trinkets/items/" + temporary_key, std::string{}, std::to_string(i),
                core::dson::ValueKind::Object);
        }
        if (desired.empty() || std::is_sorted(model.trinket_inventory.begin(), model.trinket_inventory.end(),
            [&](const auto& left, const auto& right) {
                return std::find(desired.begin(), desired.end(), left.raw_key) <
                       std::find(desired.begin(), desired.end(), right.raw_key);
            })) {
            callback(json_ok(campaign_value(campaign)));
            return;
        }
    } else {
        callback(json_error(drogon::k400BadRequest, "INVALID_CAMPAIGN_OPERATION", "Unknown trinket operation."));
        return;
    }

    application::ApplyCampaignDocumentMutationsOperation operation{operation_id, std::move(mutations)};
    auto applied = campaign.edits->apply(application::CampaignOperation{std::move(operation)},
                                          (*body)["revision"].asUInt64());
    if (!applied) {
        const auto status = applied.error().code == core::ErrorCode::StaleSessionRevision
            ? drogon::k409Conflict : drogon::k400BadRequest;
        callback(json_error(status, std::string{core::to_string(applied.error().code)}, applied.error().message));
        return;
    }
    auto value = campaign_value(campaign);
    value["trinketAdded"] = added;
    value["trinketDeleted"] = deleted;
    callback(json_ok(std::move(value)));
}

bool request_nonnegative_integer(const Json::Value& value) {
    return (value.isInt() || value.isUInt() || value.isInt64() || value.isUInt64()) && value.asInt64() >= 0;
}

void handle_campaign_building_rank(const drogon::HttpRequestPtr& request,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                   const std::shared_ptr<ServerContext>& context, bool maximize_all) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    const auto body = request->getJsonObject();
    if (!body || !body->isObject() || !request_nonnegative_integer((*body)["revision"]) ||
        (!maximize_all && (!(*body)["buildingId"].isString() || !(*body)["treeId"].isString() ||
                           !request_nonnegative_integer((*body)["rank"]))) ||
        (maximize_all && !(*body)["buildingId"].isNull() && !(*body)["buildingId"].isString())) {
        callback(json_error(drogon::k400BadRequest, "INVALID_CAMPAIGN_OPERATION",
                            "Building upgrade requires an operation target and revision."));
        return;
    }
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    const auto& model = context->campaign->edits->model();
    application::CampaignOperation operation;
    if (!maximize_all) {
        const std::string building_id = (*body)["buildingId"].asString();
        const std::string tree_id = (*body)["treeId"].asString();
        const auto selected = std::find_if(context->campaign->building_upgrade_trees.begin(),
            context->campaign->building_upgrade_trees.end(), [&](const auto& tree) {
                return tree.building_id == building_id && tree.id == tree_id;
            });
        if (selected == context->campaign->building_upgrade_trees.end() ||
            (*body)["rank"].asUInt64() > selected->codes.size()) {
            callback(json_error(drogon::k400BadRequest, "BUILDING_UPGRADE_NOT_FOUND",
                                "The requested building upgrade is not in the original game upgrade catalog."));
            return;
        }
        auto built = application::make_set_town_upgrade_rank_operation(model, tree_id,
            static_cast<std::int32_t>((*body)["rank"].asUInt64()),
            static_cast<std::int32_t>(selected->codes.size()));
        if (!built) {
            callback(json_error(drogon::k400BadRequest, std::string{core::to_string(built.error().code)},
                                built.error().message));
            return;
        }
        operation = std::move(built.value());
    } else {
        const auto selected_building = (*body)["buildingId"].isString()
            ? (*body)["buildingId"].asString() : std::string{};
        if (!selected_building.empty() && std::none_of(context->campaign->building_upgrade_trees.begin(),
            context->campaign->building_upgrade_trees.end(), [&](const auto& tree) {
                return tree.building_id == selected_building;
            })) {
            callback(json_error(drogon::k400BadRequest, "BUILDING_UPGRADE_NOT_FOUND",
                                "The requested building has no original upgrade tree."));
            return;
        }
        application::CompositeCampaignOperation maximum{selected_building.empty()
                ? "Max all town buildings" : "Max one town building", {},
            "campaign.town.set_upgrade_rank", {}};
        for (const auto& tree : context->campaign->building_upgrade_trees) {
            if (!selected_building.empty() && tree.building_id != selected_building) continue;
            const auto hash = static_cast<std::int32_t>(core::dson::string_hash(tree.id));
            const auto purchased = std::count_if(model.upgrade_purchase_nodes.begin(), model.upgrade_purchase_nodes.end(),
                [&](const auto& node) {
                    return node.instance_number == 0 && node.tree_id == hash &&
                           std::find(tree.codes.begin(), tree.codes.end(), node.requirement_code) != tree.codes.end() &&
                           node.is_purchased.value.value_or(false);
                });
            if (purchased == static_cast<std::ptrdiff_t>(tree.codes.size())) continue;
            auto built = application::make_set_town_upgrade_rank_operation(model, tree.id,
                static_cast<std::int32_t>(tree.codes.size()), static_cast<std::int32_t>(tree.codes.size()));
            if (!built) continue;
            const auto* composite = std::get_if<application::CompositeCampaignOperation>(&built.value());
            if (!composite) continue;
            maximum.operations.insert(maximum.operations.end(), composite->operations.begin(), composite->operations.end());
            maximum.document_mutations.insert(maximum.document_mutations.end(),
                composite->document_mutations.begin(), composite->document_mutations.end());
        }
        if (maximum.operations.empty() && maximum.document_mutations.empty()) {
            callback(json_ok(campaign_value(*context->campaign)));
            return;
        }
        operation = std::move(maximum);
    }
    auto applied = context->campaign->edits->apply(operation, (*body)["revision"].asUInt64());
    if (!applied) {
        const auto status = applied.error().code == core::ErrorCode::StaleSessionRevision
            ? drogon::k409Conflict : drogon::k400BadRequest;
        callback(json_error(status, std::string{core::to_string(applied.error().code)}, applied.error().message));
        return;
    }
    callback(json_ok(campaign_value(*context->campaign)));
}

std::vector<application::CampaignDocumentMutation> open_district_mutations(
    const ServerContext::CampaignSession& campaign) {
    using Kind = application::CampaignDocumentMutationKind;
    using Mutation = application::CampaignDocumentMutation;
    const auto town_document = campaign.profile.documents.find("persist.town.json");
    if (town_document == campaign.profile.documents.end() || !town_document->second.decoded) return {};
    const auto& document = *town_document->second.decoded;
    const auto template_building = std::find_if(campaign.edits->model().town_buildings.begin(),
        campaign.edits->model().town_buildings.end(), [](const auto& building) {
            return !building.raw.display_path.empty();
        });
    const auto boolean_template = std::find_if(document.fields.begin(), document.fields.end(), [](const auto& field) {
        return field.kind == core::dson::ValueKind::Boolean &&
               !field.path.starts_with("base_root/districts/");
    });
    if (template_building == campaign.edits->model().town_buildings.end() ||
        boolean_template == document.fields.end() || campaign.official_district_ids.empty()) return {};
    const auto append = [](std::string target, std::string source, std::string key,
                           core::dson::ValueKind kind) {
        return Mutation{Kind::AppendClone, "Town.DistrictSystem", "persist.town.json",
            std::move(target), std::move(source), std::move(key), kind};
    };
    const auto clear = [](std::string target) {
        return Mutation{Kind::ClearChildren, "Town.DistrictSystem", "persist.town.json",
            std::move(target), {}, {}, core::dson::ValueKind::Object};
    };
    const auto template_path = template_building->raw.display_path;
    const auto bool_path = boolean_template->path;
    const bool bool_value = std::get<bool>(boolean_template->value);
    std::vector<Mutation> mutations;
    mutations.push_back(append("base_root/districts", template_path, "districts", core::dson::ValueKind::Object));
    mutations.push_back(clear("base_root/districts"));
    mutations.push_back(append("base_root/districts/buildings", "base_root/districts", "buildings",
                               core::dson::ValueKind::Object));
    for (const auto& id : campaign.official_district_ids) {
        const auto district_path = "base_root/districts/buildings/" + id;
        mutations.push_back(append(district_path, template_path, id, core::dson::ValueKind::Object));
        mutations.push_back(clear(district_path));
        const auto built_path = district_path + "/built";
        mutations.push_back(append(built_path, bool_path, "built", core::dson::ValueKind::Boolean));
        mutations.push_back({Kind::SetValue, "Town.DistrictSystem", "persist.town.json", built_path,
            {}, {}, core::dson::ValueKind::Boolean, bool_value, false});
    }
    return mutations;
}

void handle_campaign_district(const drogon::HttpRequestPtr& request,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                              const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    const auto body = request->getJsonObject();
    if (!body || !body->isObject() || !(*body)["action"].isString() ||
        !request_nonnegative_integer((*body)["revision"])) {
        callback(json_error(drogon::k400BadRequest, "INVALID_CAMPAIGN_OPERATION",
                            "District operation requires action and revision."));
        return;
    }
    const auto action = (*body)["action"].asString();
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    auto& campaign = *context->campaign;
    const auto& model = campaign.edits->model();
    application::CampaignOperation operation;
    if (action == "system_open") {
        auto mutations = open_district_mutations(campaign);
        if (mutations.empty()) {
            callback(json_error(drogon::k400BadRequest, "DISTRICT_DEFINITIONS_UNAVAILABLE",
                                "District definitions or safe DSON templates are unavailable."));
            return;
        }
        operation = application::SetDistrictSystemOperation{true, campaign.official_district_ids,
                                                             std::move(mutations)};
    } else if (action == "system_lock") {
        operation = application::SetDistrictSystemOperation{false, {}, {
            {application::CampaignDocumentMutationKind::Erase, "Town.DistrictSystem", "persist.town.json",
             "base_root/districts", {}, {}, core::dson::ValueKind::Object}}};
    } else {
        if (!model.district_system_open) {
            callback(json_error(drogon::k409Conflict, "DISTRICT_SYSTEM_LOCKED",
                                "Unlock the district system before editing its buildings."));
            return;
        }
        std::vector<std::pair<std::string, bool>> desired;
        if (action == "set") {
            if (!(*body)["districtId"].isString() || !(*body)["built"].isBool()) {
                callback(json_error(drogon::k400BadRequest, "INVALID_CAMPAIGN_OPERATION",
                                    "A district ID and built state are required."));
                return;
            }
            desired.emplace_back((*body)["districtId"].asString(), (*body)["built"].asBool());
        } else if (action == "unlock_all" || action == "lock_all") {
            for (const auto& district : model.districts)
                desired.emplace_back(district.id, action == "unlock_all");
        } else {
            callback(json_error(drogon::k400BadRequest, "INVALID_CAMPAIGN_OPERATION", "Unknown district operation."));
            return;
        }
        std::vector<application::CampaignDocumentMutation> mutations;
        for (const auto& [id, built] : desired) {
            if (std::find(campaign.official_district_ids.begin(), campaign.official_district_ids.end(), id) ==
                campaign.official_district_ids.end()) continue;
            const auto found = std::find_if(model.districts.begin(), model.districts.end(), [&](const auto& item) {
                return item.id == id && item.built.value.has_value();
            });
            if (found == model.districts.end() || *found->built.value == built) continue;
            const auto path = "base_root/districts/buildings/" + id + "/built";
            mutations.push_back({application::CampaignDocumentMutationKind::SetValue, "Town.DistrictSystem",
                "persist.town.json", path, {}, {}, core::dson::ValueKind::Boolean,
                *found->built.value, built});
        }
        if (mutations.empty()) {
            callback(json_ok(campaign_value(campaign)));
            return;
        }
        operation = application::ApplyCampaignDocumentMutationsOperation{
            "campaign.town.set_district_system_open", std::move(mutations)};
    }
    auto applied = campaign.edits->apply(operation, (*body)["revision"].asUInt64());
    if (!applied) {
        const auto status = applied.error().code == core::ErrorCode::StaleSessionRevision
            ? drogon::k409Conflict : drogon::k400BadRequest;
        callback(json_error(status, std::string{core::to_string(applied.error().code)}, applied.error().message));
        return;
    }
    callback(json_ok(campaign_value(campaign)));
}

std::string save_path_component(std::string value) {
    for (auto& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (!(std::isalnum(byte) || character == '_' || character == '-')) character = '_';
    }
    return value.empty() ? "profile" : value;
}

std::filesystem::path commit_backup_directory(const application::AppConfiguration& configuration,
                                               std::string_view profile_id) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto directory = save_path_component(std::string{profile_id}) + "-" + std::to_string(now);
    return configuration.backup_root / "SaveBackups" / directory;
}

nlohmann::json save_error_log_value(const core::Error& error) {
    nlohmann::json value{{"code", core::to_string(error.code)},
                         {"module", error.module},
                         {"message", error.message},
                         {"context", error.context}};
    if (error.cause) value["cause"] = save_error_log_value(*error.cause);
    return value;
}

nlohmann::json save_change_trace(const application::ChangeSet& changes) {
    nlohmann::json value{{"field_changes", nlohmann::json::array()},
                         {"structural_changes", nlohmann::json::array()},
                         {"mutation_batches", nlohmann::json::array()}};
    for (const auto& change : changes.changes) {
        value["field_changes"].push_back({{"property", change.target.semantic_property},
                                          {"document", change.raw.document_id},
                                          {"path", change.raw.display_path}});
    }
    for (const auto& change : changes.structural_changes) {
        value["structural_changes"].push_back({{"property", change.target.semantic_property},
                                               {"document", change.raw.document_id},
                                               {"path", change.raw.display_path},
                                               {"action", change.action == application::CampaignStructuralAction::Erase
                                                   ? "erase" : "restore"},
                                               {"original_index", change.original_index},
                                               {"expected_kind", static_cast<int>(change.expected_kind)}});
    }
    for (const auto& batch : changes.document_mutation_batches) {
        nlohmann::json mutations = nlohmann::json::array();
        for (const auto& mutation : batch.mutations) {
            std::string kind;
            switch (mutation.kind) {
            case application::CampaignDocumentMutationKind::AppendClone: kind = "append_clone"; break;
            case application::CampaignDocumentMutationKind::AppendTemplate: kind = "append_builtin_template"; break;
            case application::CampaignDocumentMutationKind::InsertClone: kind = "insert_clone"; break;
            case application::CampaignDocumentMutationKind::CreateObject: kind = "create_object"; break;
            case application::CampaignDocumentMutationKind::Erase: kind = "erase"; break;
            case application::CampaignDocumentMutationKind::Rename: kind = "rename"; break;
            case application::CampaignDocumentMutationKind::ClearChildren: kind = "clear_children"; break;
            case application::CampaignDocumentMutationKind::SetValue: kind = "set_value"; break;
            }
            mutations.push_back({{"kind", kind}, {"property", mutation.semantic_property},
                                 {"document", mutation.document_id}, {"target_path", mutation.target_path},
                                 {"source_path", mutation.source_path}, {"new_key", mutation.new_key},
                                 {"insertion_index", mutation.insertion_index
                                     ? nlohmann::json{*mutation.insertion_index} : nlohmann::json{}}});
        }
        value["mutation_batches"].push_back({{"operation_id", batch.operation_id},
                                             {"transaction_id", batch.transaction_id},
                                             {"cancel", batch.cancel}, {"mutations", std::move(mutations)}});
    }
    if (changes.trinket_inventory_snapshot) {
        const auto inventory = [](const auto& entries) {
            auto result = nlohmann::json::array();
            for (const auto& item : entries)
                result.push_back({{"raw_key", item.raw_key}, {"id", item.id.value.value_or("")},
                                  {"path", item.raw.display_path}});
            return result;
        };
        value["trinket_inventory_before"] = inventory(changes.trinket_inventory_snapshot->before);
        value["trinket_inventory_after"] = inventory(changes.trinket_inventory_snapshot->after);
    }
    value["affected_documents"] = changes.affected_documents;
    return value;
}

void append_save_debug_log(const std::filesystem::path& data_root, std::string_view profile_id,
                           std::uint64_t revision, const core::Error& error,
                           const application::ChangeSet& changes) noexcept {
    try {
        const auto directory = data_root / "logs";
        std::filesystem::create_directories(directory);
        std::ofstream output(directory / "save-debug.jsonl", std::ios::binary | std::ios::app);
        if (!output) return;
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const nlohmann::json entry{{"timestamp_unix_ms", timestamp},
                                   {"profile_id", profile_id},
                                   {"revision", revision},
                                   {"error", save_error_log_value(error)},
                                   {"change_set", save_change_trace(changes)}};
        output << entry.dump() << '\n';
    } catch (...) {
        // Logging must not change the save result or interrupt the HTTP handler.
    }
}

void handle_campaign_save(const drogon::HttpRequestPtr& request,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                          const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;

    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    const auto& changes = context->campaign->edits->pending_changes();
    if (changes.empty()) {
        callback(json_error(drogon::k400BadRequest, "VALIDATION_FAILED", "There are no unsaved changes."));
        return;
    }
    const auto& configuration = context->configuration_store.current();
    if (configuration.backup_root.empty()) {
        callback(json_error(drogon::k400BadRequest, "INVALID_CONFIGURATION",
                            "A backup directory must be configured before saving."));
        return;
    }

    const auto backup_directory = commit_backup_directory(configuration,
                                                           context->campaign->profile.descriptor.id);
    auto committed = application::SafeSaveCommitter{context->file_system}.commit(
        context->campaign->profile, changes, context->campaign->profile.descriptor.root_path,
        backup_directory, application::SaveCommitMode::DirectSource);
    if (!committed) {
        append_save_debug_log(configuration.data_root, context->campaign->profile.descriptor.id,
                              context->campaign->edits->revision(), committed.error(), changes);
        const auto status = committed.error().code == core::ErrorCode::ConcurrentSaveChanged
            ? drogon::k409Conflict
            : (committed.error().code == core::ErrorCode::ValidationFailed ||
               committed.error().code == core::ErrorCode::MappingNotWritable
                ? drogon::k400BadRequest : drogon::k500InternalServerError);
        callback(json_error(status, std::string{core::to_string(committed.error().code)},
                            committed.error().message));
        return;
    }

    auto reloaded = application::SaveProfileDiscovery{context->file_system}.load(
        context->campaign->profile.descriptor.root_path);
    if (!reloaded) {
        context->campaign.reset();
        callback(json_error(drogon::k500InternalServerError, "COMMIT_READBACK_FAILED",
                            "The profile was written, but the committed profile could not be reopened."));
        return;
    }
    context->campaign->profile = std::move(reloaded.value());
    context->campaign->edits->mark_committed();

    Json::Value value(Json::objectValue);
    value["backupDirectory"] = committed.value().backup_directory.string();
    Json::Value documents(Json::arrayValue);
    for (const auto& document : committed.value().committed_documents) documents.append(document);
    value["committedDocuments"] = std::move(documents);
    value["campaign"] = campaign_value(*context->campaign);
    callback(json_ok(std::move(value)));
}

void handle_campaign_history(const drogon::HttpRequestPtr& request,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                             const std::shared_ptr<ServerContext>& context, bool undo) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    const auto body = request->getJsonObject();
    if (!body || !body->isMember("revision") ||
        !((*body)["revision"].isInt() || (*body)["revision"].isUInt() ||
          (*body)["revision"].isInt64() || (*body)["revision"].isUInt64()) ||
        (*body)["revision"].asInt64() < 0) {
        callback(json_error(drogon::k400BadRequest, "INVALID_CAMPAIGN_OPERATION", "Revision is required."));
        return;
    }
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(json_error(drogon::k409Conflict, std::string{core::to_string(error->code)}, error->message));
        return;
    }
    auto result = undo ? context->campaign->edits->undo((*body)["revision"].asUInt64())
                       : context->campaign->edits->redo((*body)["revision"].asUInt64());
    if (!result) {
        const auto status = result.error().code == core::ErrorCode::StaleSessionRevision
            ? drogon::k409Conflict : drogon::k400BadRequest;
        callback(json_error(status, std::string{core::to_string(result.error().code)}, result.error().message));
        return;
    }
    callback(json_ok(campaign_value(*context->campaign)));
}

void handle_content_asset(const drogon::HttpRequestPtr& request,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                          const std::shared_ptr<ServerContext>& context) {
    if (!has_expected_host(request, *context) || !has_expected_origin(request, *context) ||
        !has_session_cookie(request, *context)) {
        callback(forbidden_response("LOCAL_SESSION_REQUIRED", "Open the editor page before calling the local API."));
        return;
    }
    const auto path = request->getParameter("path");
    if (path.empty()) {
        callback(drogon::HttpResponse::newNotFoundResponse(request));
        return;
    }
    std::lock_guard lock(context->campaign_mutex);
    if (const auto error = ensure_campaign_locked(*context)) {
        callback(drogon::HttpResponse::newNotFoundResponse(request));
        return;
    }
    const auto resolved = context->campaign->content->resolve_asset(path);
    if (!resolved || !resolved.value() || !std::filesystem::is_regular_file(resolved.value()->physical_path)) {
        callback(drogon::HttpResponse::newNotFoundResponse(request));
        return;
    }
    std::ifstream input(resolved.value()->physical_path, std::ios::binary);
    if (!input) {
        callback(drogon::HttpResponse::newNotFoundResponse(request));
        return;
    }
    const std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setBody(bytes);
    const auto extension = resolved.value()->extension;
    if (extension == ".png") response->setContentTypeCode(drogon::CT_IMAGE_PNG);
    else if (extension == ".jpg" || extension == ".jpeg") response->setContentTypeCode(drogon::CT_IMAGE_JPG);
    else if (extension == ".webp") response->setContentTypeCode(drogon::CT_IMAGE_WEBP);
    else response->setContentTypeString("application/octet-stream");
    response->addHeader("Cache-Control", "no-store");
    callback(response);
}

void handle_configuration(const drogon::HttpRequestPtr& request,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                          const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    if (request->method() == drogon::Get) {
        callback(json_ok(configuration_value(context->configuration_store.current())));
        return;
    }
    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        callback(json_error(drogon::k400BadRequest, "INVALID_CONFIGURATION", "Configuration body must be a JSON object."));
        return;
    }
    auto configuration = context->configuration_store.current();
    const auto previous_save_roots = configuration.save_roots;
    const auto read_path = [&](const char* key, std::filesystem::path& target) {
        if ((*body).isMember(key) && (*body)[key].isString()) target = (*body)[key].asString();
    };
    const auto read_paths = [&](const char* key, std::vector<std::filesystem::path>& target) {
        if (!(*body).isMember(key) || !(*body)[key].isArray()) return;
        target.clear();
        for (const auto& item : (*body)[key]) if (item.isString()) target.emplace_back(item.asString());
    };
    read_path("gameRoot", configuration.game_root);
    read_path("backupRoot", configuration.backup_root);
    read_path("dataRoot", configuration.data_root);
    read_paths("workshopRoots", configuration.workshop_roots);
    read_paths("localModRoots", configuration.local_mod_roots);
    read_paths("saveRoots", configuration.save_roots);
    if ((*body).isMember("language") && (*body)["language"].isString()) configuration.language = (*body)["language"].asString();
    if ((*body).isMember("maxBackupCount") && (*body)["maxBackupCount"].isUInt()) configuration.max_backup_count = (*body)["maxBackupCount"].asUInt();
    if ((*body).isMember("autoEditSaveEnabled") && (*body)["autoEditSaveEnabled"].isBool()) configuration.auto_edit_save_enabled = (*body)["autoEditSaveEnabled"].asBool();
    if ((*body).isMember("autoEditSaveIntervalSeconds") && (*body)["autoEditSaveIntervalSeconds"].isUInt()) configuration.auto_edit_save_interval_seconds = (*body)["autoEditSaveIntervalSeconds"].asUInt();
    if (!configuration.save_roots.empty() && !configuration.save_roots.front().empty()) {
        const auto profile = application::SaveProfileDiscovery{context->file_system}.load(configuration.save_roots.front());
        if (!profile) {
            callback(json_error(drogon::k400BadRequest, "INVALID_SAVE_PROFILE", profile.error().message));
            return;
        }
        if (profile.value().status != application::ProfileReadStatus::Complete) {
            callback(json_error(drogon::k400BadRequest, "INVALID_SAVE_PROFILE", "The selected save profile is not structurally complete."));
            return;
        }
    }
    const auto result = context->configuration_store.save(configuration);
    if (!result) {
        callback(json_error(drogon::k400BadRequest, "INVALID_CONFIGURATION", result.error().message));
        return;
    }
    if (configuration.save_roots != previous_save_roots) context->invalidate_campaign();
    context->initialization.start(context->configuration_store.current());
    callback(json_ok(configuration_value(context->configuration_store.current())));
}

Json::Value initialization_value(const DatabaseInitializationState& state) {
    Json::Value value(Json::objectValue);
    value["status"] = state.status;
    value["phase"] = state.phase;
    value["currentWork"] = state.current_work;
    value["progressPercent"] = state.progress_percent;
    value["baseElapsedMs"] = Json::UInt64(state.base_elapsed_ms);
    value["modElapsedMs"] = Json::UInt64(state.mod_elapsed_ms);
    value["totalElapsedMs"] = Json::UInt64(state.total_elapsed_ms);
    value["installedMods"] = static_cast<Json::UInt64>(state.installed_mods);
    value["enabledMods"] = static_cast<Json::UInt64>(state.enabled_mods);
    value["reusedExisting"] = state.reused_existing;
    Json::Value diagnostics(Json::arrayValue);
    for (const auto& item : state.diagnostics) diagnostics.append(item);
    value["diagnostics"] = std::move(diagnostics);
    return value;
}

void handle_initialization(const drogon::HttpRequestPtr& request,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                          const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    if (request->method() == drogon::Post) {
        const auto& config = context->configuration_store.current();
        if (config.game_root.empty() || config.backup_root.empty() || config.data_root.empty()) {
            callback(json_error(drogon::k400BadRequest, "INVALID_CONFIGURATION", "Complete configuration is required before database initialization."));
            return;
        }
        bool force = false;
        if (const auto body = request->getJsonObject(); body && body->isMember("force") && (*body)["force"].isBool())
            force = (*body)["force"].asBool();
        if (force) context->invalidate_campaign();
        context->initialization.start(config, force);
    }
    callback(json_ok(initialization_value(context->initialization.state())));
}

void handle_recovery(const drogon::HttpRequestPtr& request,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    if (request->method() == drogon::Post) {
        const auto result = context->configuration_store.discard_recovery();
        if (!result) {
            callback(json_error(drogon::k500InternalServerError, "RECOVERY_DISCARD_FAILED", result.error().message));
            return;
        }
    }
    const auto result = context->configuration_store.recovery_status();
    if (!result) {
        callback(json_error(drogon::k500InternalServerError, "RECOVERY_STATUS_FAILED", result.error().message));
        return;
    }
    Json::Value value(Json::objectValue);
    value["available"] = result.value().available;
    value["profileId"] = result.value().profile_id;
    value["sourceProfile"] = result.value().source_profile.string();
    value["sourceFingerprint"] = Json::UInt64(result.value().source_fingerprint);
    value["revision"] = Json::UInt64(result.value().revision);
    value["savedAt"] = result.value().saved_at;
    value["path"] = result.value().path;
    callback(json_ok(std::move(value)));
}

void handle_recovery_restore(const drogon::HttpRequestPtr& request,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                             const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    const auto status = context->configuration_store.recovery_status();
    if (!status) {
        callback(json_error(drogon::k500InternalServerError, "RECOVERY_STATUS_FAILED", status.error().message));
        return;
    }
    if (!status.value().available) {
        callback(json_error(drogon::k404NotFound, "RECOVERY_NOT_FOUND", "There is no active auto-edit recovery point."));
        return;
    }
    const auto snapshot = context->configuration_store.recovery_snapshot();
    if (!snapshot) {
        callback(json_error(drogon::k500InternalServerError, "RECOVERY_READ_FAILED", snapshot.error().message));
        return;
    }
    Json::Value value(Json::objectValue);
    value["profileId"] = status.value().profile_id;
    value["sourceProfile"] = status.value().source_profile.string();
    value["sourceFingerprint"] = Json::UInt64(status.value().source_fingerprint);
    value["revision"] = Json::UInt64(status.value().revision);
    value["savedAt"] = status.value().saved_at;
    value["snapshot"] = snapshot.value();
    callback(json_ok(std::move(value)));
}

void handle_database_mods(const drogon::HttpRequestPtr& request,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    const auto init = context->initialization.state();
    if (init.status != "completed") {
        callback(json_error(drogon::k409Conflict, "DATABASE_NOT_READY", "The Mod database is not ready yet."));
        return;
    }
    const auto queried = read_enabled_mods(context->initialization.mod_database_path());
    if (!queried) {
        callback(json_error(drogon::k500InternalServerError, "MOD_DATABASE_QUERY_FAILED", queried.error().message));
        return;
    }
    Json::Value mods(Json::arrayValue);
    for (const auto& entry : queried.value()) {
        Json::Value item(Json::objectValue);
        item["order"] = static_cast<Json::UInt>(entry.order);
        item["key"] = entry.key;
        item["provider"] = entry.provider_id;
        item["externalId"] = entry.external_id;
        item["name"] = entry.fallback_name;
        item["displayName"] = entry.display_name;
        item["matchedModId"] = entry.matched_mod_id;
        item["enabled"] = entry.enabled;
        if (!entry.matched_mod_id.empty() &&
            find_mod_cover(context->initialization.mod_database_path(), entry.matched_mod_id))
            item["coverUrl"] = "/api/database/mod-cover?modId=" + entry.matched_mod_id;
        mods.append(std::move(item));
    }
    Json::Value value(Json::objectValue);
    value["mods"] = std::move(mods);
    callback(json_ok(std::move(value)));
}

void handle_mod_cover(const drogon::HttpRequestPtr& request,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                      const std::shared_ptr<ServerContext>& context) {
    if (!has_expected_host(request, *context) || !has_expected_origin(request, *context) ||
        !has_session_cookie(request, *context)) {
        callback(forbidden_response("LOCAL_SESSION_REQUIRED", "Open the editor page before calling the local API."));
        return;
    }
    const auto mod_id = request->getParameter("modId");
    const auto cover = find_mod_cover(context->initialization.mod_database_path(), mod_id);
    if (!cover) {
        callback(drogon::HttpResponse::newNotFoundResponse(request));
        return;
    }
    std::ifstream input(cover.value(), std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setBody(bytes);
    const auto ext = cover.value().extension().string();
    response->setContentTypeCode(ext == ".png" ? drogon::CT_IMAGE_PNG : drogon::CT_IMAGE_JPG);
    response->addHeader("Cache-Control", "no-store");
    callback(response);
}

void handle_game_asset(const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                       const std::shared_ptr<ServerContext>& context) {
    if (!has_expected_host(request, *context) || !has_expected_origin(request, *context) ||
        !has_session_cookie(request, *context)) {
        callback(forbidden_response("LOCAL_SESSION_REQUIRED", "Open the editor page before calling the local API."));
        return;
    }
    const auto requested = request->getParameter("path");
    const std::filesystem::path relative{requested};
    if (requested.empty() || relative.is_absolute() ||
        std::any_of(relative.begin(), relative.end(), [](const auto& component) {
            return component == "..";
        })) {
        callback(json_error(drogon::k400BadRequest, "INVALID_GAME_ASSET", "The requested game asset path is invalid."));
        return;
    }
    const auto root = context->configuration_store.current().game_root;
    const auto asset = root / relative;
    std::error_code error;
    if (!std::filesystem::is_regular_file(asset, error)) {
        callback(drogon::HttpResponse::newNotFoundResponse(request));
        return;
    }
    std::ifstream input(asset, std::ios::binary);
    if (!input) {
        callback(drogon::HttpResponse::newNotFoundResponse(request));
        return;
    }
    const std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setBody(bytes);
    auto extension = asset.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (extension == ".png") response->setContentTypeCode(drogon::CT_IMAGE_PNG);
    else if (extension == ".jpg" || extension == ".jpeg") response->setContentTypeCode(drogon::CT_IMAGE_JPG);
    else if (extension == ".webp") response->setContentTypeCode(drogon::CT_IMAGE_WEBP);
    else if (extension == ".svg") response->setContentTypeString("image/svg+xml");
    else response->setContentTypeString("application/octet-stream");
    response->addHeader("Cache-Control", "no-store");
    callback(response);
}

void handle_town_asset(const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                       const std::shared_ptr<ServerContext>& context) {
    if (!has_expected_host(request, *context) || !has_expected_origin(request, *context) ||
        !has_session_cookie(request, *context)) {
        callback(forbidden_response("LOCAL_SESSION_REQUIRED", "Open the editor page before calling the local API."));
        return;
    }
    const auto asset = find_town_asset(context->initialization.base_database_path(), request->getParameter("role"));
    if (!asset) {
        callback(drogon::HttpResponse::newNotFoundResponse(request));
        return;
    }
    std::ifstream input(asset.value(), std::ios::binary);
    if (!input) {
        callback(drogon::HttpResponse::newNotFoundResponse(request));
        return;
    }
    const std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setBody(bytes);
    auto extension = asset.value().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (extension == ".png") response->setContentTypeCode(drogon::CT_IMAGE_PNG);
    else if (extension == ".jpg" || extension == ".jpeg") response->setContentTypeCode(drogon::CT_IMAGE_JPG);
    else if (extension == ".webp") response->setContentTypeCode(drogon::CT_IMAGE_WEBP);
    else response->setContentTypeString("application/octet-stream");
    response->addHeader("Cache-Control", "no-store");
    callback(response);
}

void handle_directory_picker(const drogon::HttpRequestPtr& request,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                             const std::shared_ptr<ServerContext>& context) {
    std::function<void(const drogon::HttpResponsePtr&)> callback_ref =
        [&](const drogon::HttpResponsePtr& response) { callback(response); };
    if (!authorized_api_request(request, *context, callback_ref)) return;
    const auto body = request->getJsonObject();
    const auto kind = body && body->isMember("kind") && (*body)["kind"].isString()
        ? (*body)["kind"].asString() : std::string{};
    const bool english = context->configuration_store.current().language == "en_us";
    const auto title = english
        ? (kind == "game" ? "Choose game directory" :
           kind == "workshop" ? "Choose Workshop Mod directory" :
           kind == "localMod" ? "Choose additional local Mod directory" :
           kind == "save" ? "Choose save directory" :
           kind == "backup" ? "Choose backup directory" : "Choose directory")
        : (kind == "game" ? "选择游戏安装目录" :
           kind == "workshop" ? "选择 Workshop Mod 目录" :
           kind == "localMod" ? "选择额外本地 Mod 目录" :
           kind == "save" ? "选择存档目录" :
           kind == "backup" ? "选择存档备份目录" : "选择目录");
#ifdef _WIN32
    const auto selected = choose_directory(title);
    if (!selected) {
        callback(json_error(drogon::k409Conflict, "DIRECTORY_PICKER_CANCELLED", "Directory selection was cancelled."));
        return;
    }
    if (kind == "save") {
        const auto profile = application::SaveProfileDiscovery{context->file_system}.load(*selected);
        if (!profile) {
            callback(json_error(drogon::k400BadRequest, "INVALID_SAVE_PROFILE", profile.error().message));
            return;
        }
        if (profile.value().status != application::ProfileReadStatus::Complete) {
            callback(json_error(drogon::k400BadRequest, "INVALID_SAVE_PROFILE", "The selected save profile is not structurally complete."));
            return;
        }
    }
    Json::Value value(Json::objectValue);
    value["kind"] = kind;
    value["path"] = selected->string();
    callback(json_ok(std::move(value)));
#else
    callback(json_error(drogon::k501NotImplemented, "DIRECTORY_PICKER_UNAVAILABLE", "The native directory picker is unavailable on this platform."));
#endif
}

bool wait_for_http_server(std::uint16_t port) {
    constexpr auto timeout = std::chrono::seconds{5};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
#ifdef _WIN32
        const auto socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_handle != INVALID_SOCKET) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            (void)InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr);
            const auto connected = ::connect(
                socket_handle, reinterpret_cast<const sockaddr*>(&address),
                static_cast<int>(sizeof(address))) == 0;
            closesocket(socket_handle);
            if (connected) return true;
        }
#else
        const auto socket_handle = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_handle >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            (void)inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            const auto connected = ::connect(
                socket_handle, reinterpret_cast<const sockaddr*>(&address),
                static_cast<socklen_t>(sizeof(address))) == 0;
            ::close(socket_handle);
            if (connected) return true;
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return false;
}

std::optional<std::uint16_t> choose_ephemeral_loopback_port() {
#ifdef _WIN32
    WSADATA startup_data{};
    if (WSAStartup(MAKEWORD(2, 2), &startup_data) != 0) return std::nullopt;
    const auto socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        WSACleanup();
        return std::nullopt;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    (void)InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr);
    const auto bound = ::bind(socket_handle, reinterpret_cast<const sockaddr*>(&address),
                              static_cast<int>(sizeof(address))) == 0;
    int address_size = static_cast<int>(sizeof(address));
    const auto found = bound && ::getsockname(
        socket_handle, reinterpret_cast<sockaddr*>(&address), &address_size) == 0;
    const auto port = found ? ntohs(address.sin_port) : 0;
    closesocket(socket_handle);
    WSACleanup();
#else
    const auto socket_handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_handle < 0) return std::nullopt;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    (void)inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    const auto bound = ::bind(socket_handle, reinterpret_cast<const sockaddr*>(&address),
                              static_cast<socklen_t>(sizeof(address))) == 0;
    socklen_t address_size = static_cast<socklen_t>(sizeof(address));
    const auto found = bound && ::getsockname(
        socket_handle, reinterpret_cast<sockaddr*>(&address), &address_size) == 0;
    const auto port = found ? ntohs(address.sin_port) : 0;
    ::close(socket_handle);
#endif
    if (port == 0) return std::nullopt;
    return port;
}

bool open_browser(std::string_view url) {
#ifdef _WIN32
    const auto url_string = std::string{url};
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteA(nullptr, "open", url_string.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL));
    return result > 32;
#else
    const auto url_string = std::string{url};
    const char* executable = "xdg-open";
#ifdef __APPLE__
    executable = "open";
#endif
    std::array<char*, 3> arguments{
        const_cast<char*>(executable), const_cast<char*>(url_string.c_str()), nullptr};
    pid_t child{};
    return posix_spawnp(&child, executable, nullptr, nullptr, arguments.data(),
                        environ) == 0;
#endif
}

#ifdef _WIN32
std::wstring wide_text(std::string_view text) {
    if (text.empty()) return {};
    const auto required = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required);
    return result;
}

std::optional<std::filesystem::path> choose_directory(std::string_view title) {
    const auto title_wide = wide_text(title);
    BROWSEINFOW browse{};
    browse.lpszTitle = title_wide.c_str();
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE selected = SHBrowseForFolderW(&browse);
    if (selected == nullptr) return std::nullopt;
    wchar_t path[MAX_PATH]{};
    const auto copied = SHGetPathFromIDListW(selected, path);
    CoTaskMemFree(selected);
    if (!copied) return std::nullopt;
    return std::filesystem::path{path};
}
#endif

} // namespace

int run_drogon_http_server(
    const application::ApplicationStatusService& status_service,
    application::AppConfigurationStore& configuration_store,
    const std::filesystem::path& web_root,
    std::uint16_t requested_port,
    bool open_browser_on_start) {
    std::error_code file_error;
    const auto index_path = web_root / "index.html";
    if (!std::filesystem::is_regular_file(index_path, file_error)) {
        std::cerr << "Frontend index.html is missing from " << web_root.string() << '\n';
        return 1;
    }

    auto context = std::make_shared<ServerContext>(
        std::filesystem::absolute(web_root), make_session_token(), status_service,
        configuration_store, configuration_store.file_system());

    const auto selected_port = requested_port == 0
        ? choose_ephemeral_loopback_port()
        : std::optional<std::uint16_t>{requested_port};
    if (!selected_port) {
        std::cerr << "Could not allocate an available loopback port.\n";
        return 1;
    }

    auto& server = drogon::app();
    server.disableSession()
        .setThreadNum(1)
        .setLogLevel(trantor::Logger::kWarn)
        .setDocumentRoot(context->web_root.string())
        .setFileTypes({"html", "js", "css", "json", "svg", "png", "webp", "ico"})
        .setStaticFilesCacheTime(0)
        .addListener("127.0.0.1", *selected_port);

    server.registerHandler(
        "/", [context](const drogon::HttpRequestPtr& request,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(serve_index(request, context));
        }, {drogon::Get});
    server.registerHandler(
        "/index.html", [context](const drogon::HttpRequestPtr& request,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(serve_index(request, context));
        }, {drogon::Get});
    server.registerHandler(
        "/api/status", [context](const drogon::HttpRequestPtr& request,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_status(request, std::move(callback), context);
        }, {drogon::Get});
    server.registerHandler(
        "/api/configuration", [context](const drogon::HttpRequestPtr& request,
                                          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_configuration(request, std::move(callback), context);
        }, {drogon::Get, drogon::Put});
    server.registerHandler(
        "/api/recovery", [context](const drogon::HttpRequestPtr& request,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_recovery(request, std::move(callback), context);
        }, {drogon::Get, drogon::Post});
    server.registerHandler(
        "/api/recovery/restore", [context](const drogon::HttpRequestPtr& request,
                                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_recovery_restore(request, std::move(callback), context);
        }, {drogon::Post});
    server.registerHandler(
        "/api/profiles", [context](const drogon::HttpRequestPtr& request,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_database_mods(request, std::move(callback), context);
        }, {drogon::Get});
    server.registerHandler(
        "/api/database/mods", [context](const drogon::HttpRequestPtr& request,
                                          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_database_mods(request, std::move(callback), context);
        }, {drogon::Get});
    server.registerHandler(
        "/api/database/mod-cover", [context](const drogon::HttpRequestPtr& request,
                                               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_mod_cover(request, std::move(callback), context);
        }, {drogon::Get});
    server.registerHandler(
        "/api/game-asset", [context](const drogon::HttpRequestPtr& request,
                                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_game_asset(request, std::move(callback), context);
        }, {drogon::Get});
    server.registerHandler(
        "/api/town-asset", [context](const drogon::HttpRequestPtr& request,
                                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_town_asset(request, std::move(callback), context);
        }, {drogon::Get});
    server.registerHandler(
        "/api/content-asset", [context](const drogon::HttpRequestPtr& request,
                                           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_content_asset(request, std::move(callback), context);
        }, {drogon::Get});
    server.registerHandler(
        "/api/campaign", [context](const drogon::HttpRequestPtr& request,
                                      std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign(request, std::move(callback), context);
        }, {drogon::Get});
    server.registerHandler(
        "/api/campaign/reload", [context](const drogon::HttpRequestPtr& request,
                                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_reload(request, std::move(callback), context);
        }, {drogon::Post});
    server.registerHandler(
        "/api/campaign/resource", [context](const drogon::HttpRequestPtr& request,
                                               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_resource(request, std::move(callback), context);
        }, {drogon::Post});
    server.registerHandler(
        "/api/campaign/hero-classes", [context](const drogon::HttpRequestPtr& request,
                                                   std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_hero_classes(request, std::move(callback), context);
        }, {drogon::Get});
    server.registerHandler(
        "/api/campaign/hero", [context](const drogon::HttpRequestPtr& request,
                                            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_hero(request, std::move(callback), context);
        }, {drogon::Post});
    server.registerHandler(
        "/api/campaign/hero-trinket", [context](const drogon::HttpRequestPtr& request,
                                               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_hero_trinket(request, std::move(callback), context);
        }, {drogon::Post});
    server.registerHandler(
        "/api/campaign/quirks", [context](const drogon::HttpRequestPtr& request,
                                         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_quirk_catalog(request, std::move(callback), context);
        }, {drogon::Get});
    server.registerHandler(
        "/api/campaign/hero-quirk", [context](const drogon::HttpRequestPtr& request,
                                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_hero_quirk(request, std::move(callback), context);
        }, {drogon::Post});
    server.registerHandler(
        "/api/campaign/trinkets", [context](const drogon::HttpRequestPtr& request,
                                                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_trinket_catalog(request, std::move(callback), context);
        }, {drogon::Get});
    server.registerHandler(
        "/api/campaign/trinket", [context](const drogon::HttpRequestPtr& request,
                                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_trinket_edit(request, std::move(callback), context);
        }, {drogon::Post});
    server.registerHandler(
        "/api/campaign/building-rank", [context](const drogon::HttpRequestPtr& request,
                                                    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_building_rank(request, std::move(callback), context, false);
        }, {drogon::Post});
    server.registerHandler(
        "/api/campaign/building-max", [context](const drogon::HttpRequestPtr& request,
                                                   std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_building_rank(request, std::move(callback), context, true);
        }, {drogon::Post});
    server.registerHandler(
        "/api/campaign/district", [context](const drogon::HttpRequestPtr& request,
                                               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_district(request, std::move(callback), context);
        }, {drogon::Post});
    server.registerHandler(
        "/api/campaign/save", [context](const drogon::HttpRequestPtr& request,
                                           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_save(request, std::move(callback), context);
        }, {drogon::Post});
    server.registerHandler(
        "/api/campaign/undo", [context](const drogon::HttpRequestPtr& request,
                                           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_history(request, std::move(callback), context, true);
        }, {drogon::Post});
    server.registerHandler(
        "/api/campaign/redo", [context](const drogon::HttpRequestPtr& request,
                                           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_history(request, std::move(callback), context, false);
        }, {drogon::Post});
    server.registerHandler(
        "/api/initialization", [context](const drogon::HttpRequestPtr& request,
                                          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_initialization(request, std::move(callback), context);
        }, {drogon::Get, drogon::Post});
    server.registerHandler(
        "/api/select-directory", [context](const drogon::HttpRequestPtr& request,
                                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_directory_picker(request, std::move(callback), context);
        }, {drogon::Post});
    server.setDefaultHandler(
        [](const drogon::HttpRequestPtr& request,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            if (request->path().starts_with("/api/")) {
                callback(json_error(drogon::k404NotFound, "ROUTE_NOT_FOUND",
                                    "The requested API route does not exist."));
            } else {
                callback(drogon::HttpResponse::newNotFoundResponse(request));
            }
        });

    std::promise<std::uint16_t> ready_promise;
    auto ready_future = ready_promise.get_future();
    std::atomic_bool ready_reported{false};
    server.registerBeginningAdvice([&server, context, &ready_promise, &ready_reported] {
        const auto listeners = server.getListeners();
        const auto listener = std::find_if(listeners.begin(), listeners.end(), [](const auto& address) {
            return address.toIp() == "127.0.0.1";
        });
        if (listener == listeners.end() || listener->toPort() == 0) {
            if (!ready_reported.exchange(true))
                ready_promise.set_exception(std::make_exception_ptr(
                    std::runtime_error{"Drogon did not create a loopback listener."}));
            return;
        }
        const auto port = listener->toPort();
        context->host = "127.0.0.1:" + std::to_string(port);
        context->origin = "http://" + context->host;
        if (!ready_reported.exchange(true)) ready_promise.set_value(port);
    });

    std::thread server_thread([&server, &ready_promise, &ready_reported] {
        try {
            server.run();
        } catch (...) {
            if (!ready_reported.exchange(true))
                ready_promise.set_exception(std::current_exception());
        }
    });

    const auto& initial_configuration = configuration_store.current();
    if (!initial_configuration.game_root.empty() && !initial_configuration.backup_root.empty() &&
        !initial_configuration.data_root.empty()) {
        context->initialization.start(initial_configuration);
    }
    std::thread campaign_preload_thread;
    if (!initial_configuration.save_roots.empty() && !initial_configuration.save_roots.front().empty()) {
        campaign_preload_thread = std::thread([context] {
            while (context->initialization.state().status == "running")
                std::this_thread::sleep_for(std::chrono::milliseconds{150});
            if (context->initialization.state().status != "completed") return;
            std::lock_guard lock(context->campaign_mutex);
            (void)ensure_campaign_locked(*context);
        });
    }
    const auto join_campaign_preload = [&campaign_preload_thread] {
        if (campaign_preload_thread.joinable()) campaign_preload_thread.join();
    };

    std::uint16_t port{};
    try {
        if (ready_future.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
            std::cerr << "Timed out while starting the local HTTP service.\n";
            server.quit();
            server_thread.join();
            join_campaign_preload();
            return 1;
        }
        port = ready_future.get();
    } catch (const std::exception& error) {
        std::cerr << "Unable to start the local HTTP service: " << error.what() << '\n';
        server.quit();
        server_thread.join();
        join_campaign_preload();
        return 1;
    }

    if (!wait_for_http_server(port)) {
        std::cerr << "The local HTTP service did not become ready in time.\n";
        server.quit();
        server_thread.join();
        join_campaign_preload();
        return 1;
    }

    const auto url = context->origin + "/";
    std::cout << "DDSE local service ready at " << url << std::endl;
    if (open_browser_on_start && !open_browser(url))
        std::cerr << "Could not open the default browser. Open " << url << " manually.\n";
    std::cout << "Press Ctrl+C to stop the local service." << std::endl;
    server_thread.join();
    join_campaign_preload();
    return 0;
}

} // namespace ddse::infrastructure
