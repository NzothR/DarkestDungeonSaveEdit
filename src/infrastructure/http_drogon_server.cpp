#include "ddse/infrastructure/http_drogon_server.hpp"

#include "ddse/application/save_profile.hpp"
#include "ddse/application/save_commit.hpp"
#include "ddse/application/campaign_model_builder.hpp"
#include "ddse/application/campaign_edit_session.hpp"
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
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <mutex>
#include <memory>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
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

    auto session = std::make_unique<ServerContext::CampaignSession>();
    session->building_upgrade_trees = load_building_upgrade_trees(*content, context.file_system);
    session->official_district_ids = load_official_district_ids(context.file_system,
        context.configuration_store.current().game_root);
    session->profile = std::move(profile.value());
    session->content = std::move(content);
    session->edits = std::make_unique<application::CampaignEditSession>(std::move(model));
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

    Json::Value heroes(Json::arrayValue);
    for (const auto& hero : model.heroes) {
        Json::Value item(Json::objectValue);
        item["id"] = hero.persistent_id;
        item["name"] = strip_game_markup(hero.name.value ? *hero.name.value : hero.persistent_id);
        item["classId"] = hero.class_id.value ? *hero.class_id.value : hero.definition.raw_id;
        item["className"] = strip_game_markup(hero.definition.display_name.empty()
            ? (hero.class_id.value ? *hero.class_id.value : hero.definition.raw_id)
            : hero.definition.display_name);
        item["state"] = hero.state == domain::EntityState::Resolved ? "resolved" : "partial";
        item["assets"] = definition_assets(hero.definition);
        if (const auto portrait = hero_roster_portrait(hero.definition)) item["portraitPath"] = *portrait;
        heroes.append(std::move(item));
    }
    value["heroes"] = std::move(heroes);

    Json::Value trinkets(Json::arrayValue);
    for (const auto& trinket : model.trinket_inventory) {
        Json::Value item(Json::objectValue);
        item["index"] = static_cast<Json::UInt64>(trinket.index);
        item["id"] = trinket.id.value ? *trinket.id.value : trinket.definition.raw_id;
        item["name"] = strip_game_markup(trinket.definition.display_name.empty()
            ? (trinket.id.value ? *trinket.id.value : trinket.definition.raw_id)
            : trinket.definition.display_name);
        item["amount"] = trinket.amount.value ? Json::Value(*trinket.amount.value) : Json::Value(Json::nullValue);
        item["assets"] = definition_assets(trinket.definition);
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
        return field.kind == core::dson::ValueKind::Boolean;
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
        "/api/campaign/resource", [context](const drogon::HttpRequestPtr& request,
                                               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_campaign_resource(request, std::move(callback), context);
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

    std::uint16_t port{};
    try {
        if (ready_future.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
            std::cerr << "Timed out while starting the local HTTP service.\n";
            server.quit();
            server_thread.join();
            return 1;
        }
        port = ready_future.get();
    } catch (const std::exception& error) {
        std::cerr << "Unable to start the local HTTP service: " << error.what() << '\n';
        server.quit();
        server_thread.join();
        return 1;
    }

    if (!wait_for_http_server(port)) {
        std::cerr << "The local HTTP service did not become ready in time.\n";
        server.quit();
        server_thread.join();
        return 1;
    }

    const auto url = context->origin + "/";
    std::cout << "DDSE local service ready at " << url << std::endl;
    if (open_browser_on_start && !open_browser(url))
        std::cerr << "Could not open the default browser. Open " << url << " manually.\n";
    std::cout << "Press Ctrl+C to stop the local service." << std::endl;
    server_thread.join();
    return 0;
}

} // namespace ddse::infrastructure
