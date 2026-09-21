#include "ddse/application/campaign_edit_session.hpp"
#include "ddse/application/campaign_model_builder.hpp"
#include "ddse/application/content_environment.hpp"
#include "ddse/application/content_scanner.hpp"
#include "ddse/application/mod_environment.hpp"
#include "ddse/application/save_commit.hpp"
#include "ddse/application/save_profile.hpp"
#include "ddse/infrastructure/base_content_database.hpp"
#include "ddse/infrastructure/mod_environment_database.hpp"
#include "ddse/infrastructure/native_file_system.hpp"
#include "ddse/infrastructure/sqlite_content_environment.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
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
using namespace ddse;
using DsonDocument = core::dson::DsonDocument;
using DsonField = core::dson::DsonField;
using Json = nlohmann::json;

struct Options {
    std::filesystem::path source{"test_save_profile/profile_0"};
    std::filesystem::path game{R"(D:\SteamLibrary\steamapps\common\DarkestDungeon)"};
    std::filesystem::path workshop{R"(D:\SteamLibrary\steamapps\workshop\content\262060)"};
    std::filesystem::path local{R"(D:\SteamLibrary\steamapps\common\DarkestDungeon\modes)"};
    std::filesystem::path output{"test_save_profile/stage12_operation_tests"};
    std::string focus{"all"};
};

struct GeneratedTreeCleanup {
    std::filesystem::path staging;
    std::filesystem::path temporary;
    ~GeneratedTreeCleanup() {
        std::error_code ignored;
        if (!staging.empty()) std::filesystem::remove_all(staging, ignored);
        ignored.clear();
        if (!temporary.empty()) std::filesystem::remove_all(temporary, ignored);
    }
};

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }

Options options_from(const std::vector<std::string>& args) {
    Options result;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i + 1 >= args.size()) fail("Missing argument value after " + args[i]);
        const auto key = args[i];
        const auto& encoded = args[++i];
        auto next = std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(encoded.data()), encoded.size()));
        if (key == "--source-profile") result.source = next;
        else if (key == "--game-root") result.game = next;
        else if (key == "--workshop-root") result.workshop = next;
        else if (key == "--local-mod-root") result.local = next;
        else if (key == "--output-root") result.output = next;
        else if (key == "--focus") result.focus = encoded;
        else fail("Unknown argument: " + key);
    }
    if (result.focus != "all" && result.focus != "acceptance-followups")
        fail("--focus must be 'all' or 'acceptance-followups'");
    return result;
}

template <class T>
T must(core::Result<T, core::Error> result, std::string_view operation) {
    if (!result) fail(std::string{operation} + ": " + result.error().message);
    return std::move(result).value();
}

std::filesystem::path absolute_path(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::absolute(path, error);
    if (error) fail("Cannot resolve path " + path.string() + ": " + error.message());
    return result.lexically_normal();
}

const DsonField* find_field(const DsonDocument& document, std::string_view path) {
    const auto found = std::find_if(document.fields.begin(), document.fields.end(), [&](const auto& field) {
        return field.path == path;
    });
    return found == document.fields.end() ? nullptr : &*found;
}

const DsonField* find_display_field(const DsonDocument& document, std::string_view display_path) {
    const auto arrow = display_path.find(" => ");
    const auto local_path = arrow == std::string_view::npos ? display_path : display_path.substr(0, arrow);
    const auto* found = find_field(document, local_path);
    if (!found || arrow == std::string_view::npos || !found->embedded_document) return found;
    return find_display_field(*found->embedded_document, display_path.substr(arrow + 4));
}

const DsonField& field_at(const DsonDocument& document, std::string_view display_path) {
    const auto* field = find_display_field(document, display_path);
    if (!field) fail("Missing DSON path: " + std::string{display_path});
    return *field;
}

const DsonField& field_at(const application::RawSaveProfile& profile, std::string_view document_id,
                          std::string_view path) {
    const auto found = profile.documents.find(document_id);
    if (found == profile.documents.end() || !found->second.decoded)
        fail("Missing decoded document: " + std::string{document_id});
    return field_at(*found->second.decoded, path);
}

application::CampaignValue campaign_value(const DsonField& field) {
    if (const auto* value = std::get_if<std::int32_t>(&field.value)) return *value;
    if (const auto* value = std::get_if<float>(&field.value)) return *value;
    if (const auto* value = std::get_if<std::string>(&field.value)) return *value;
    if (const auto* value = std::get_if<bool>(&field.value)) return *value;
    if (const auto* value = std::get_if<char>(&field.value)) return *value;
    fail("A structural mutation attempted to use a non-scalar DSON value");
}

application::CampaignDocumentMutation append_clone(std::string property, std::string document,
    std::string target, std::string source, core::dson::ValueKind kind) {
    const auto slash = target.find_last_of('/');
    if (slash == std::string::npos || slash + 1 == target.size()) fail("Append target path has no DSON key");
    const auto key = target.substr(slash + 1);
    return {application::CampaignDocumentMutationKind::AppendClone, std::move(property), std::move(document),
            std::move(target), std::move(source), key, kind, std::nullopt, std::nullopt};
}
application::CampaignDocumentMutation insert_clone(std::string property, std::string document,
    std::string target, std::string source, core::dson::ValueKind kind, std::size_t index) {
    const auto slash = target.find_last_of('/');
    if (slash == std::string::npos || slash + 1 == target.size()) fail("Insert target path has no DSON key");
    const auto key = target.substr(slash + 1);
    return {application::CampaignDocumentMutationKind::InsertClone, std::move(property), std::move(document),
            std::move(target), std::move(source), key, kind, std::nullopt, std::nullopt, index};
}
application::CampaignDocumentMutation erase_entry(std::string property, std::string document,
    std::string target, core::dson::ValueKind kind) {
    return {application::CampaignDocumentMutationKind::Erase, std::move(property), std::move(document),
            std::move(target), {}, {}, kind, std::nullopt, std::nullopt};
}
application::CampaignDocumentMutation rename_entry(std::string property, std::string document,
    std::string target, std::string new_key, core::dson::ValueKind kind) {
    return {application::CampaignDocumentMutationKind::Rename, std::move(property), std::move(document),
            std::move(target), {}, std::move(new_key), kind, std::nullopt, std::nullopt};
}
application::CampaignDocumentMutation clear_children(std::string property, std::string document,
    std::string target) {
    return {application::CampaignDocumentMutationKind::ClearChildren, std::move(property), std::move(document),
            std::move(target), {}, {}, core::dson::ValueKind::Object, std::nullopt, std::nullopt};
}
application::CampaignDocumentMutation set_value(const application::RawSaveProfile& profile,
    std::string property, std::string document, std::string target, std::string source,
    application::CampaignValue after) {
    const auto& source_field = field_at(profile, document, source);
    return {application::CampaignDocumentMutationKind::SetValue, std::move(property), std::move(document),
            std::move(target), {}, {}, source_field.kind, campaign_value(source_field), std::move(after)};
}

application::CampaignOperation mutation_operation(std::string operation_id,
    std::vector<application::CampaignDocumentMutation> mutations) {
    return application::ApplyCampaignDocumentMutationsOperation{std::move(operation_id), std::move(mutations)};
}

std::string hero_data_path(std::string_view id, std::string_view inner_path) {
    return "base_root/heroes/" + std::string{id} + "/hero_file_data/raw_data => " + std::string{inner_path};
}
std::string hero_outer_path(std::string_view id) { return "base_root/heroes/" + std::string{id}; }
std::string hero_label(const domain::Hero& hero) {
    return hero.name.value.value_or("(unnamed)") + " — " + hero.class_id.value.value_or("unknown") +
           "（名单第" + std::to_string(hero.roster_position + 1) + "位，ID " + hero.persistent_id + "）";
}
const domain::Hero& hero_at(const domain::CampaignModel& model, std::size_t position) {
    const auto found = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& hero) {
        return hero.roster_position == position;
    });
    if (found == model.heroes.end()) fail("Roster position is missing: " + std::to_string(position + 1));
    return *found;
}

std::vector<application::ContentDefinition> content_list(const application::IContentEnvironment& env,
                                                          std::string_view type) {
    return must(env.list_content(type), "List " + std::string{type});
}
std::string localized_name(const application::IContentEnvironment& env,
                           const application::ContentDefinition& definition) {
    if (!definition.localization_key.empty()) {
        auto value = must(env.resolve_localization(definition.localization_key, "english"), "Resolve localization");
        if (value && !value->value.empty()) return value->value;
    }
    return definition.display_name.empty() ? definition.id : definition.display_name;
}
std::string mod_name(const application::ModEnvironmentScanResult& scan, std::string_view id) {
    const auto found = std::find_if(scan.mods.begin(), scan.mods.end(), [&](const auto& mod) { return mod.id == id; });
    return found == scan.mods.end() || found->display_name.empty() ? std::string{id} : found->display_name;
}

std::string quirk_name(const domain::HeroQuirk& quirk) {
    if (!quirk.definition.display_name.empty()) return quirk.definition.display_name;
    return quirk.id;
}

void append_quirk_defaults(const application::RawSaveProfile& profile,
                           std::vector<application::CampaignDocumentMutation>& mutations,
                           std::string_view hero_id, std::string_view template_id,
                           std::string_view destination_id) {
    const auto source_root = hero_data_path(hero_id, "base_root/quirks/" + std::string{template_id});
    const auto target_root = hero_data_path(hero_id, "base_root/quirks/" + std::string{destination_id});
    const auto required = [&](std::string_view field_name, application::CampaignValue value) {
        const auto source = source_root + "/" + std::string{field_name};
        const auto target = target_root + "/" + std::string{field_name};
        (void)field_at(profile, "persist.roster.json", source);
        mutations.push_back(set_value(profile, "Hero.Quirks", "persist.roster.json", target, source,
                                      std::move(value)));
    };
    required("is_new", true);
    required("is_locked", false);
    for (const auto& [name, value] : std::vector<std::pair<std::string, application::CampaignValue>>{
             {"trinketId", std::int32_t{0}}, {"mission_count", std::int32_t{0}},
             {"replaces_quirk", std::int32_t{0}}, {"replaces_quirk_viewed", false},
             {"evolution_duration_remaining", std::int32_t{0}}}) {
        const auto source = source_root + "/" + name;
        if (find_display_field(*profile.documents.at("persist.roster.json").decoded, source)) {
            const auto target = target_root + "/" + name;
            mutations.push_back(set_value(profile, "Hero.Quirks", "persist.roster.json", target, source, value));
        }
    }
}

std::vector<application::CampaignDocumentMutation> make_add_hero_mutations(
    const application::RawSaveProfile& profile, const domain::Hero& source_hero,
    std::string_view new_id, std::string_view new_name) {
    const auto document = std::string{"persist.roster.json"};
    const auto source_outer = hero_outer_path(source_hero.persistent_id);
    const auto target_outer = hero_outer_path(new_id);
    std::vector<application::CampaignDocumentMutation> mutations;
    mutations.push_back(append_clone("Hero.PersistentId", document, target_outer, source_outer,
                                     core::dson::ValueKind::Object));
    const auto set_hero_value = [&](std::string_view inner_path, application::CampaignValue value) {
        const auto source = hero_data_path(source_hero.persistent_id, inner_path);
        const auto target = hero_data_path(new_id, inner_path);
        mutations.push_back(set_value(profile, "Hero.PersistentId", document, target, source, std::move(value)));
    };
    set_hero_value("base_root/actor/name", std::string{new_name});
    if (source_hero.class_id.value) set_hero_value("base_root/heroClass", *source_hero.class_id.value);
    set_hero_value("base_root/resolveXp", std::int32_t{0});
    set_hero_value("base_root/weapon_rank", std::int32_t{0});
    set_hero_value("base_root/armour_rank", std::int32_t{0});
    const auto stress_source = hero_data_path(source_hero.persistent_id, "base_root/m_Stress");
    if (const auto* stress = find_field(*profile.documents.at(document).decoded,
                                       "base_root/heroes/" + source_hero.persistent_id + "/hero_file_data/raw_data")) {
        if (stress->embedded_document && find_field(*stress->embedded_document, "base_root/m_Stress"))
            set_hero_value("base_root/m_Stress", 0.0F);
    }
    (void)stress_source;
    set_hero_value("base_root/affliction_type_id", std::string{});
    set_hero_value("base_root/affliction_severity", std::int32_t{0});
    set_hero_value("base_root/virtue_type_id", std::string{});
    const auto quirks = hero_data_path(new_id, "base_root/quirks");
    mutations.push_back(clear_children("Hero.PersistentId", document, quirks));
    const auto trinkets = hero_data_path(new_id, "base_root/trinkets/items");
    if (find_field(*profile.documents.at(document).decoded,
                   "base_root/heroes/" + source_hero.persistent_id + "/hero_file_data/raw_data") &&
        field_at(profile, document, hero_data_path(source_hero.persistent_id, "base_root/trinkets/items")).kind == core::dson::ValueKind::Object)
        mutations.push_back(clear_children("Hero.PersistentId", document, trinkets));
    return mutations;
}

std::int32_t tree_hash(std::string_view tree) {
    return static_cast<std::int32_t>(core::dson::string_hash(tree));
}

std::optional<std::int32_t> effective_upgrade_tree_maximum(
    const application::IContentEnvironment& env, const application::IFileSystem& fs,
    std::string_view virtual_path, std::string_view tree_id) {
    auto resolved = must(env.resolve_asset(virtual_path), "Resolve effective upgrade tree");
    if (!resolved) return std::nullopt;
    const auto bytes = must(fs.read_file(resolved->physical_path), "Read effective upgrade tree");
    const auto root = Json::parse(bytes, nullptr, false);
    if (!root.is_object() || !root.contains("trees") || !root["trees"].is_array())
        fail("Effective upgrade JSON has no trees array: " + std::string{virtual_path});
    for (const auto& tree : root["trees"]) {
        if (!tree.is_object() || tree.value("id", std::string{}) != tree_id) continue;
        if (!tree.contains("requirements") || !tree["requirements"].is_array())
            fail("Effective upgrade tree has no requirements array: " + std::string{tree_id});
        const auto count = static_cast<std::int32_t>(tree["requirements"].size());
        return count > 0 ? std::optional<std::int32_t>{count} : std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::int32_t> effective_equipment_tree_maximum(
    const application::IContentEnvironment& env, const application::IFileSystem& fs,
    const domain::Hero& hero, std::string_view suffix) {
    if (!hero.class_id.value) return std::nullopt;
    const auto tree = *hero.class_id.value + "." + std::string{suffix};
    const auto path = "upgrades/heroes/" + *hero.class_id.value + ".upgrades.json";
    return effective_upgrade_tree_maximum(env, fs, path, tree);
}

std::optional<std::int32_t> upgrade_instance(const domain::CampaignModel& model, const domain::Hero& hero) {
    if (!hero.class_id.value) return std::nullopt;
    const auto wanted = tree_hash(*hero.class_id.value + ".weapon");
    std::set<std::int32_t> instances;
    for (const auto& node : model.upgrade_purchase_nodes)
        if (node.tree_id == wanted) instances.insert(node.instance_number);
    if (instances.size() != 1) return std::nullopt;
    return *instances.begin();
}

std::int32_t current_rank(const domain::CampaignModel& model, std::int32_t instance, std::string_view tree,
                          char first_code, std::int32_t maximum) {
    const auto hash = tree_hash(tree);
    std::set<char> purchased;
    for (const auto& node : model.upgrade_purchase_nodes)
        if (node.instance_number == instance && node.tree_id == hash && node.is_purchased.value.value_or(false))
            purchased.insert(node.requirement_code);
    std::int32_t rank = 0;
    while (rank < maximum && purchased.contains(static_cast<char>(first_code + rank))) ++rank;
    return rank;
}

std::optional<std::int32_t> effective_combat_skill_maximum(
    const application::IContentEnvironment& env, const application::IFileSystem& fs,
    const domain::Hero& hero, const application::ContentDefinition& skill) {
    if (!hero.class_id.value) return std::nullopt;
    const auto colon = skill.id.find(':');
    const auto suffix = colon == std::string::npos ? skill.id : skill.id.substr(colon + 1);
    const auto tree_id = *hero.class_id.value + "." + suffix;
    const auto virtual_path = "upgrades/heroes/" + *hero.class_id.value + ".upgrades.json";
    auto resolved = must(env.resolve_asset(virtual_path), "Resolve effective skill upgrade tree");
    if (!resolved) return std::nullopt;
    const auto bytes = must(fs.read_file(resolved->physical_path), "Read effective skill upgrade tree");
    const auto root = Json::parse(bytes, nullptr, false);
    if (!root.is_object() || !root.contains("trees") || !root["trees"].is_array())
        fail("Effective hero upgrades JSON has no trees array: " + virtual_path);
    const Json* tree = nullptr;
    for (const auto& candidate : root["trees"])
        if (candidate.is_object() && candidate.value("id", std::string{}) == tree_id) { tree = &candidate; break; }
    if (!tree) return std::nullopt;
    if (!tree->contains("requirements") || !(*tree)["requirements"].is_array())
        fail("Effective skill tree has no requirements array: " + tree_id);
    const auto payload = Json::parse(skill.payload_json, nullptr, false);
    if (!payload.is_object() || !payload.contains("levels") || !payload["levels"].is_array())
        return std::nullopt;
    std::set<std::int32_t> levels;
    for (const auto& record : payload["levels"]) {
        if (!record.is_object() || !record.contains("level")) continue;
        const Json* value = &record["level"];
        if (value->is_array()) { if (value->empty()) continue; value = &value->front(); }
        if (value->is_number_integer()) levels.insert(value->get<std::int32_t>());
        else if (value->is_string()) {
            const auto text = value->get<std::string>();
            std::int32_t parsed{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (error == std::errc{} && end == text.data() + text.size()) levels.insert(parsed);
        }
    }
    if (levels.empty() || tree->at("requirements").size() != levels.size()) return std::nullopt;
    std::int32_t expected = 0;
    for (const auto level : levels) if (level != expected++) return std::nullopt;
    return static_cast<std::int32_t>(levels.size());
}

bool is_camping_skill_for(const application::ContentDefinition& skill, const domain::Hero& hero) {
    if (!hero.class_id.value || !skill.localization_key.starts_with("camping_skill_name_")) return false;
    if (skill.id.starts_with(*hero.class_id.value + ":")) return true;
    const auto payload = Json::parse(skill.payload_json, nullptr, false);
    if (!payload.is_object() || !payload.contains("hero_classes") || !payload["hero_classes"].is_array()) return false;
    return std::any_of(payload["hero_classes"].begin(), payload["hero_classes"].end(), [&](const Json& item) {
        return item.is_string() && item.get<std::string>() == *hero.class_id.value;
    });
}


std::optional<std::int32_t> numeric_key(std::string_view text) {
    std::int32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 0) return std::nullopt;
    return value;
}

std::vector<std::string> district_ids(const Options& options) {
    std::set<std::string, std::less<>> ids;
    const auto dlc_root = options.game / "dlc";
    if (!std::filesystem::exists(dlc_root)) fail("Official DLC directory is missing: " + dlc_root.string());
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dlc_root)) {
        if (!entry.is_regular_file() || !entry.path().filename().string().ends_with(".districts.json")) continue;
        std::ifstream input(entry.path(), std::ios::binary);
        if (!input) continue;
        Json document;
        try { input >> document; } catch (const std::exception&) { continue; }
        if (!document.is_object() || !document.contains("buildings") || !document["buildings"].is_array()) continue;
        for (const auto& building : document["buildings"])
            if (building.is_object() && building.contains("name") && building["name"].is_string())
                ids.insert(building["name"].get<std::string>());
    }
    return {ids.begin(), ids.end()};
}

struct Scenario {
    Scenario(std::string scenario_directory, std::string scenario_title, std::string scenario_details,
             std::filesystem::path source, application::CampaignOperation main_operation,
             std::function<void(const application::RawSaveProfile&, const domain::CampaignModel&)> verifier,
             std::vector<application::CampaignOperation> follow_ups = {})
        : directory(std::move(scenario_directory)), title(std::move(scenario_title)),
          details(std::move(scenario_details)), source_profile(std::move(source)),
          operation(std::move(main_operation)), verify(std::move(verifier)),
          follow_up_operations(std::move(follow_ups)) {}

    std::string directory;
    std::string title;
    std::string details;
    std::filesystem::path source_profile;
    application::CampaignOperation operation;
    std::function<void(const application::RawSaveProfile&, const domain::CampaignModel&)> verify;
    std::vector<application::CampaignOperation> follow_up_operations;
};

std::string first_generated_trait_id(const std::filesystem::path& game_root,
                                     std::string_view condition_type) {
    const auto path = game_root / "shared" / "trait" / "trait_library.json";
    std::ifstream input(path, std::ios::binary);
    if (!input) fail("Cannot read the original trait library: " + path.string());
    Json document;
    try { input >> document; }
    catch (const std::exception& error) { fail("Cannot parse the original trait library: " + std::string{error.what()}); }
    if (!document.is_object() || !document.contains("traits") || !document["traits"].is_array())
        fail("Original trait library has no traits array");
    for (const auto& trait : document["traits"]) {
        if (!trait.is_object() || !trait.value("is_generated", false) ||
            trait.value("overstress_type", std::string{}) != condition_type) continue;
        const auto id = trait.value("id", std::string{});
        if (!id.empty()) return id;
    }
    fail("No generated original-game trait is available for stress condition '" + std::string{condition_type} + "'");
}

Scenario make_affliction_and_stress_scenario(const domain::CampaignModel& model,
    const std::filesystem::path& game_root, const std::filesystem::path& source_path) {
    const domain::Hero* eligible = nullptr;
    for (const auto& hero : model.heroes) {
        if (hero.stress.value && hero.stress.raw && hero.affliction_id.value && hero.affliction_id.raw &&
            hero.affliction_severity.value && hero.affliction_severity.raw && hero.virtue_id.value && hero.virtue_id.raw)
            eligible = &hero;
        if (eligible) break;
    }
    if (!eligible) fail("A hero with serialized stress and affliction fields is required");
    const auto affliction_id = first_generated_trait_id(game_root, "affliction");
    application::SetHeroAfflictionStateOperation operation{
        eligible->persistent_id, application::HeroAfflictionState::Afflicted, affliction_id};
    std::ostringstream detail;
    detail << "- 英雄：" << hero_label(*eligible) << "。\n"
           << "- 状态：设置为原版折磨 ID `" << affliction_id << "`，严重度 1，美德 ID 清空。\n"
           << "- 压力值：`" << *eligible->stress.value << "` → `100`。\n"
           << "- 折磨 ID 从只读原版 `shared/trait/trait_library.json` 中选择。\n";
    const auto hero_id = eligible->persistent_id;
    return {"02_set_affliction_and_stress", "设置英雄为折磨并将压力改为 100",
        detail.str(), source_path, std::move(operation),
        [hero_id, affliction_id](const auto&, const auto& projected) {
            const auto hero = std::find_if(projected.heroes.begin(), projected.heroes.end(), [&](const auto& item) {
                return item.persistent_id == hero_id;
            });
            if (hero == projected.heroes.end() || hero->stress.value != 100.0F ||
                hero->affliction_id.value != affliction_id || hero->affliction_severity.value != 1 ||
                hero->virtue_id.value != std::string{})
                fail("Affliction and stress values did not re-project to the requested state");
        },
        {application::SetCampaignValueOperation{{"Hero.Stress", hero_id}, 100.0F}}};
}

Scenario make_hero_name_scenario(const domain::CampaignModel& model,
    const std::filesystem::path& source_path) {
    const auto& hero = hero_at(model, 0);
    if (!hero.name.value || !hero.name.raw) fail("The first roster hero has no editable mapped name");
    const std::string new_name{"英雄名称测试"};
    std::ostringstream detail;
    detail << "- 英雄：" << hero_label(hero) << "。\n- 名称：`" << *hero.name.value << "` → `" << new_name << "`。\n";
    const auto hero_id = hero.persistent_id;
    return {"03_rename_first_hero", "重命名名单第一位英雄", detail.str(), source_path,
        application::SetCampaignValueOperation{{"Hero.Name", hero_id}, new_name},
        [hero_id, new_name](const auto&, const auto& projected) {
            const auto found = std::find_if(projected.heroes.begin(), projected.heroes.end(), [&](const auto& item) {
                return item.persistent_id == hero_id;
            });
            if (found == projected.heroes.end() || found->name.value != new_name)
                fail("Hero name did not re-project to the requested value");
        }};
}

void copy_profile(const std::filesystem::path& source, const std::filesystem::path& destination) {
    if (std::filesystem::exists(destination)) fail("Refusing to overwrite existing test profile: " + destination.string());
    std::filesystem::create_directories(destination);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
        const auto relative = entry.path().lexically_relative(source);
        const auto target = destination / relative;
        if (entry.is_directory()) std::filesystem::create_directories(target);
        else if (entry.is_regular_file()) {
            std::filesystem::create_directories(target.parent_path());
            std::filesystem::copy_file(entry.path(), target);
        } else fail("Unexpected file type in save profile: " + entry.path().string());
    }
}

struct GeneratedScenario {
    std::string directory;
    std::string title;
    std::string details;
    std::vector<std::string> affected_documents;
};

GeneratedScenario write_scenario(const Scenario& scenario,
    const application::IContentEnvironment& environment,
    const std::filesystem::path& output_root) {
    infrastructure::NativeFileSystem file_system;
    application::SaveProfileDiscovery discovery{file_system};
    auto source = must(discovery.load(scenario.source_profile), "Load scenario baseline");
    const auto source_fingerprint = source.baseline_fingerprint;
    const auto model = application::CampaignModelBuilder{}.build(source, environment);
    if (model.state == domain::ModelState::Invalid) fail("Scenario source campaign model is invalid: " + scenario.directory);

    application::CampaignEditSession session{model};
    auto applied = must(session.apply(scenario.operation, session.revision()), "Apply " + scenario.directory);
    if (!applied.validation.valid()) fail("Operation validation did not pass: " + scenario.directory);
    for (const auto& follow_up : scenario.follow_up_operations) {
        auto follow_up_result = must(session.apply(follow_up, session.revision()),
                                     "Apply follow-up operation for " + scenario.directory);
        if (!follow_up_result.validation.valid())
            fail("Follow-up operation validation did not pass: " + scenario.directory);
    }
    if (session.pending_changes().empty()) fail("Operation produced no writeback changes: " + scenario.directory);

    const auto scenario_root = output_root / scenario.directory;
    const auto target_profile = scenario_root / "profile_0";
    const auto backup = scenario_root / "write_backup";
    copy_profile(scenario.source_profile, target_profile);
    auto committed = application::SafeSaveCommitter{file_system}.commit(
        source, session.pending_changes(), target_profile, backup,
        application::SaveCommitMode::AcceptanceTestCandidate);
    if (!committed) fail("Safe commit failed for " + scenario.directory + ": " + committed.error().message);

    auto result_profile = must(discovery.load(target_profile), "Reload generated scenario profile");
    const auto result_model = application::CampaignModelBuilder{}.build(result_profile, environment);
    if (result_model.state == domain::ModelState::Invalid)
        fail("Generated scenario failed semantic campaign re-projection: " + scenario.directory);
    if (scenario.verify) scenario.verify(result_profile, result_model);
    const auto source_still_matches = must(source.matches_disk_baseline(file_system), "Check source save baseline");
    if (!source_still_matches || source.baseline_fingerprint != source_fingerprint)
        fail("Safe writeback modified the immutable source profile: " + scenario.source_profile.string());
    return {scenario.directory, scenario.title, scenario.details, committed.value().committed_documents};
}

bool mod_quirk_definition(const application::ContentDefinition& definition, bool positive,
                          bool require_replaceable = true) {
    if (definition.type != "quirk" || definition.provenance.layer_type != "mod" || !definition.provenance.selected)
        return false;
    const auto payload = Json::parse(definition.payload_json, nullptr, false);
    return payload.is_object() && payload.value("is_disease", true) == false &&
           payload.value("is_positive", !positive) == positive &&
           (!require_replaceable || payload.value("can_be_replaced_by_new_quirk", false));
}

std::set<std::string, std::less<>> hero_quirk_ids(const domain::Hero& hero) {
    std::set<std::string, std::less<>> ids;
    for (const auto& quirk : hero.quirks) if (!quirk.is_disease) ids.insert(quirk.id);
    return ids;
}

Scenario make_add_hero_scenario(const application::RawSaveProfile& profile,
    const domain::CampaignModel& model, const std::vector<application::ContentDefinition>& classes,
    const application::IContentEnvironment& environment,
    const std::filesystem::path& source_path) {
    const domain::Hero* template_hero = nullptr;
    const application::ContentDefinition* class_definition = nullptr;
    for (const auto& hero : model.heroes) {
        if (!hero.class_id.value || !hero.resolve_xp.value || *hero.resolve_xp.value != 0 ||
            !hero.weapon_rank.value || *hero.weapon_rank.value != 0 ||
            !hero.armour_rank.value || *hero.armour_rank.value != 0) continue;
        const auto found = std::find_if(classes.begin(), classes.end(), [&](const auto& value) {
            return value.id == *hero.class_id.value && value.provenance.layer_type == "mod" && value.provenance.selected;
        });
        if (found != classes.end()) { template_hero = &hero; class_definition = &*found; break; }
    }
    if (!template_hero || !class_definition)
        fail("No existing level-one enabled mod hero is available as a safe template");
    std::uint64_t max_id = 0;
    for (const auto& hero : model.heroes) {
        std::uint64_t parsed{};
        const auto [end, error] = std::from_chars(hero.persistent_id.data(),
            hero.persistent_id.data() + hero.persistent_id.size(), parsed);
        if (error == std::errc{} && end == hero.persistent_id.data() + hero.persistent_id.size()) max_id = std::max(max_id, parsed);
    }
    if (max_id >= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
        fail("Hero IDs reached the safe test range");
    const auto new_id = std::to_string(max_id + 1);
    std::string new_name = "DDSE_STAGE12_HERO";
    std::size_t suffix = 1;
    while (std::any_of(model.heroes.begin(), model.heroes.end(), [&](const auto& hero) {
        return hero.name.value && *hero.name.value == new_name;
    })) new_name = "DDSE_STAGE12_HERO_" + std::to_string(suffix++);
    auto mutations = make_add_hero_mutations(profile, *template_hero, new_id, new_name);
    std::ostringstream detail;
    detail << "- 新英雄：**" << localized_name(environment, *class_definition) << "** (`"
           << *template_hero->class_id.value << "`)；来源 mod **" << class_definition->provenance.source_id << "**。\n"
           << "- 名单位置：第 " << model.heroes.size() + 1 << " 位；名称 `" << new_name << "`；新增 key `" << new_id << "`。\n"
           << "- 等级 1、武器/防具初始等级、战斗与生存技能沿用等级 1 模板；怪癖与已装备饰品清空。\n";
    return {"01_add_mod_hero", "新增一个 1 级 mod 英雄", detail.str(), source_path,
        mutation_operation("campaign.hero.add", std::move(mutations)),
        [new_id, new_name, class_id = *template_hero->class_id.value,
         source = class_definition->provenance.source_id](const auto&, const auto& projected) {
            if (projected.heroes.empty()) fail("Added hero is missing after re-projection");
            const auto& added = projected.heroes.back();
            if (added.persistent_id != new_id || added.name.value.value_or("") != new_name ||
                added.class_id.value.value_or("") != class_id || added.resolve_xp.value.value_or(-1) != 0 ||
                added.weapon_rank.value.value_or(-1) != 0 || added.armour_rank.value.value_or(-1) != 0 ||
                !added.quirks.empty() || !added.trinkets.empty() || added.definition.source_id != source)
                fail("New hero structural/readback validation failed");
        }};
}

Scenario make_replace_quirks_scenario(const application::RawSaveProfile& profile,
    const domain::CampaignModel& model, const std::vector<application::ContentDefinition>& definitions,
    const application::IContentEnvironment& environment, const application::ModEnvironmentScanResult& scan,
    const std::filesystem::path& source_path) {
    for (const auto& hero : model.heroes) {
        const auto old_positive = std::find_if(hero.quirks.begin(), hero.quirks.end(), [](const auto& q) {
            return !q.is_disease && q.polarity == domain::QuirkPolarity::Positive && !q.is_locked.value.value_or(false);
        });
        const auto old_negative = std::find_if(hero.quirks.begin(), hero.quirks.end(), [](const auto& q) {
            return !q.is_disease && q.polarity == domain::QuirkPolarity::Negative && !q.is_locked.value.value_or(false);
        });
        if (old_positive == hero.quirks.end() || old_negative == hero.quirks.end()) continue;
        const auto existing = hero_quirk_ids(hero);
        const auto new_positive = std::find_if(definitions.begin(), definitions.end(), [&](const auto& d) {
            return mod_quirk_definition(d, true) && !existing.contains(d.id) && d.id != old_negative->id;
        });
        auto new_negative = std::find_if(definitions.begin(), definitions.end(), [&](const auto& d) {
            return d.id == "ablutomania" && mod_quirk_definition(d, false, false) &&
                   !existing.contains(d.id) && d.id != old_positive->id;
        });
        if (new_negative == definitions.end()) {
            new_negative = std::find_if(definitions.begin(), definitions.end(), [&](const auto& d) {
                return mod_quirk_definition(d, false, false) && !existing.contains(d.id) && d.id != old_positive->id;
            });
        }
        if (new_positive == definitions.end() || new_negative == definitions.end()) continue;
        std::vector<application::CampaignDocumentMutation> mutations;
        const auto positive_path = old_positive->raw.display_path;
        mutations.push_back(rename_entry("Hero.Quirks", "persist.roster.json", positive_path,
                                          new_positive->id, core::dson::ValueKind::Object));
        append_quirk_defaults(profile, mutations, hero.persistent_id, old_positive->id, new_positive->id);

        const auto negative_path = old_negative->raw.display_path;
        const auto negative_index = static_cast<std::size_t>(std::distance(hero.quirks.begin(), old_negative));
        mutations.push_back(erase_entry("Hero.Quirks", "persist.roster.json", negative_path,
                                        core::dson::ValueKind::Object));
        mutations.push_back(insert_clone("Hero.Quirks", "persist.roster.json",
            hero_data_path(hero.persistent_id, "base_root/quirks/" + new_negative->id), negative_path,
            core::dson::ValueKind::Object, negative_index));
        append_quirk_defaults(profile, mutations, hero.persistent_id, old_negative->id, new_negative->id);
        std::ostringstream detail;
        detail << "- 英雄：**" << hero_label(hero) << "**。\n"
               << "- 正面：`" << quirk_name(*old_positive) << "` (`" << old_positive->id << "`) → **"
               << localized_name(environment, *new_positive) << "** (`" << new_positive->id << "`)，mod **"
               << mod_name(scan, new_positive->provenance.source_id) << "**。\n"
               << "- 负面：删除 `" << quirk_name(*old_negative) << "` (`" << old_negative->id << "`) 后，在同一名单位置新增 **"
               << localized_name(environment, *new_negative) << "** (`" << new_negative->id << "`)，mod **"
               << mod_name(scan, new_negative->provenance.source_id) << "**；新记录 `replaces_quirk=0`，不应显示替换标记。\n";
        const auto hero_id = hero.persistent_id;
        const auto positive_id = new_positive->id, negative_id = new_negative->id;
        const auto removed_negative_id = old_negative->id;
        return {"02_replace_mod_quirks", "替换一个正面和一个负面 mod 怪癖", detail.str(), source_path,
            mutation_operation("campaign.hero.add_or_replace_quirk", std::move(mutations)),
            [hero_id, positive_id, negative_id, removed_negative_id, negative_index](const auto&, const auto& projected) {
                const auto found = std::find_if(projected.heroes.begin(), projected.heroes.end(), [&](const auto& h) { return h.persistent_id == hero_id; });
                if (found == projected.heroes.end()) fail("Quirk replacement hero is missing");
                const auto has = [&](const std::string& id, domain::QuirkPolarity polarity) {
                    return std::any_of(found->quirks.begin(), found->quirks.end(), [&](const auto& q) {
                        return q.id == id && q.polarity == polarity && q.definition.layer_type == "mod";
                    });
                };
                if (!has(positive_id, domain::QuirkPolarity::Positive) || !has(negative_id, domain::QuirkPolarity::Negative))
                    fail("Replacement quirks did not project with expected polarity and mod source");
                if (std::any_of(found->quirks.begin(), found->quirks.end(), [&](const auto& quirk) {
                        return quirk.id == removed_negative_id;
                    }) || negative_index >= found->quirks.size() || found->quirks[negative_index].id != negative_id)
                    fail("Negative quirk replacement did not delete the old record and insert the new one at its original ordered position");
                const auto negative = std::find_if(found->quirks.begin(), found->quirks.end(), [&](const auto& quirk) {
                    return quirk.id == negative_id;
                });
                if (negative == found->quirks.end() || negative->replaces_quirk.value != 0 ||
                    negative->trinket_id.value != 0)
                    fail("New negative quirk retained replacement or trinket metadata instead of neutral defaults");
            }};
    }
    fail("No early hero has replaceable positive/negative quirks and unused mod replacements");
}

Scenario make_add_quirks_scenario(const application::RawSaveProfile& profile,
    const domain::CampaignModel& model, const std::vector<application::ContentDefinition>& definitions,
    const application::IContentEnvironment& environment, const application::ModEnvironmentScanResult& scan,
    const std::filesystem::path& source_path) {
    for (const auto& hero : model.heroes) {
        std::size_t positives = 0, negatives = 0;
        for (const auto& quirk : hero.quirks) {
            if (quirk.is_disease) continue;
            positives += quirk.polarity == domain::QuirkPolarity::Positive;
            negatives += quirk.polarity == domain::QuirkPolarity::Negative;
        }
        if (positives >= 10 || negatives >= 10 || hero.quirks.empty()) continue;
        const auto existing = hero_quirk_ids(hero);
        const auto template_quirk = std::find_if(hero.quirks.begin(), hero.quirks.end(), [](const auto& q) {
            return !q.is_disease && q.state != domain::EntityState::Invalid;
        });
        const auto positive = std::find_if(definitions.begin(), definitions.end(), [&](const auto& d) {
            return mod_quirk_definition(d, true, false) && !existing.contains(d.id);
        });
        const auto negative = std::find_if(definitions.begin(), definitions.end(), [&](const auto& d) {
            return mod_quirk_definition(d, false, false) && !existing.contains(d.id) && d.id != (positive == definitions.end() ? "" : positive->id);
        });
        if (template_quirk == hero.quirks.end() || positive == definitions.end() || negative == definitions.end()) continue;
        std::vector<application::CampaignDocumentMutation> mutations;
        for (const auto* added : {&*positive, &*negative}) {
            const auto target = hero_data_path(hero.persistent_id, "base_root/quirks/" + added->id);
            mutations.push_back(append_clone("Hero.Quirks", "persist.roster.json", target,
                template_quirk->raw.display_path, core::dson::ValueKind::Object));
            append_quirk_defaults(profile, mutations, hero.persistent_id, template_quirk->id, added->id);
        }
        std::ostringstream detail;
        detail << "- 英雄：**" << hero_label(hero) << "**（名单第 " << hero.roster_position + 1 << " 位）。\n"
               << "- 新增正面：**" << localized_name(environment, *positive) << "** (`" << positive->id << "`)，mod **"
               << mod_name(scan, positive->provenance.source_id) << "**。\n"
               << "- 新增负面：**" << localized_name(environment, *negative) << "** (`" << negative->id << "`)，mod **"
               << mod_name(scan, negative->provenance.source_id) << "**。\n"
               << "- 原正/负面怪癖数量：" << positives << " / " << negatives << "；修改后各增加 1。\n";
        const auto hero_id = hero.persistent_id, positive_id = positive->id, negative_id = negative->id;
        return {"03_add_mod_quirks", "增加一个正面和一个负面 mod 怪癖", detail.str(), source_path,
            mutation_operation("campaign.hero.add_or_replace_quirk", std::move(mutations)),
            [hero_id, positive_id, negative_id](const auto&, const auto& projected) {
                const auto found = std::find_if(projected.heroes.begin(), projected.heroes.end(), [&](const auto& h) { return h.persistent_id == hero_id; });
                if (found == projected.heroes.end()) fail("Quirk addition hero is missing");
                for (const auto& id : {positive_id, negative_id})
                    if (std::none_of(found->quirks.begin(), found->quirks.end(), [&](const auto& q) {
                            return q.id == id && q.definition.layer_type == "mod";
                        })) fail("Added mod quirk is missing after semantic re-projection: " + id);
            }};
    }
    fail("No hero below the 10 positive/negative quirk caps has a usable mod template");
}

Scenario make_add_inventory_trinket_scenario(const application::RawSaveProfile& profile,
    const domain::CampaignModel& model, const std::vector<application::ContentDefinition>& definitions,
    const application::IContentEnvironment& environment, const application::ModEnvironmentScanResult& scan,
    const std::filesystem::path& source_path) {
    std::set<std::string, std::less<>> inventory_ids;
    const domain::TrinketInventoryEntry* template_item = nullptr;
    std::int32_t maximum_index = -1;
    for (const auto& item : model.trinket_inventory) {
        if (item.id.value) inventory_ids.insert(*item.id.value);
        const auto index = numeric_key(item.raw_key);
        if (index && *index > maximum_index) { maximum_index = *index; template_item = &item; }
    }
    if (!template_item) fail("No inventory trinket exists to use as a DSON item template");
    const auto selected = std::find_if(definitions.begin(), definitions.end(), [&](const auto& item) {
        return item.type == "trinket" && item.provenance.layer_type == "mod" && item.provenance.selected &&
               !inventory_ids.contains(item.id);
    });
    if (selected == definitions.end()) fail("No unused enabled mod trinket is available");
    if (maximum_index == std::numeric_limits<std::int32_t>::max()) fail("Inventory index is outside the safe range");
    const auto new_index = std::to_string(maximum_index + 1);
    const auto target = "base_root/trinkets/items/" + new_index;
    const auto source = template_item->raw.display_path;
    std::vector<application::CampaignDocumentMutation> mutations;
    mutations.push_back(append_clone("TrinketInventory.Items", "persist.estate.json", target,
                                     source, core::dson::ValueKind::Object));
    mutations.push_back(set_value(profile, "TrinketInventory.Items", "persist.estate.json", target + "/id",
                                  source + "/id", selected->id));
    mutations.push_back(set_value(profile, "TrinketInventory.Items", "persist.estate.json", target + "/amount",
                                  source + "/amount", std::int32_t{1}));
    std::ostringstream detail;
    detail << "- 新饰品：**" << localized_name(environment, *selected) << "** (`" << selected->id << "`) × 1。\n"
           << "- 来源 mod：**" << mod_name(scan, selected->provenance.source_id) << "** (`"
           << selected->provenance.source_id << "`)。\n"
           << "- 追加位置：物品栏第 " << maximum_index + 2 << " 个，DSON key `" << new_index << "`。\n";
    const auto id = selected->id;
    return {"04_add_inventory_trinket", "在饰品库存末尾增加一个 mod 饰品", detail.str(), source_path,
        mutation_operation("campaign.trinket.add_inventory", std::move(mutations)),
        [id, new_index](const auto&, const auto& projected) {
            const auto found = std::find_if(projected.trinket_inventory.begin(), projected.trinket_inventory.end(),
                [&](const auto& item) { return item.raw_key == new_index; });
            if (found == projected.trinket_inventory.end() || found->id.value.value_or("") != id ||
                found->amount.value.value_or(-1) != 1 || found->definition.layer_type != "mod")
                fail("Inventory trinket did not re-project with requested ID, amount, and mod source");
        }};
}

Scenario make_equip_trinkets_scenario(const application::RawSaveProfile& profile,
    const domain::CampaignModel& model, const std::vector<application::ContentDefinition>& definitions,
    const application::ContentDefinition& generic_definition,
    const application::IContentEnvironment& environment, const application::ModEnvironmentScanResult& scan,
    const std::filesystem::path& source_path) {
    const auto* generic = &generic_definition;
    const domain::Hero* target_hero = nullptr;
    const application::ContentDefinition* specific = nullptr;
    for (const auto& hero : model.heroes) {
        if (!hero.trinkets.empty() || !hero.class_id.value) continue;
        const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto& item) {
            if (item.type != "trinket" || item.provenance.layer_type != "mod" || !item.provenance.selected || item.id == generic->id)
                return false;
            return std::any_of(item.relationships.begin(), item.relationships.end(), [&](const auto& relation) {
                return relation.relationship_type == "restricted_to" && relation.id == *hero.class_id.value;
            });
        });
        if (found != definitions.end()) { target_hero = &hero; specific = &*found; break; }
    }
    if (!target_hero || !specific) fail("No empty-roster hero has an enabled class-specific mod trinket");
    const domain::HeroTrinket* template_item = nullptr;
    for (const auto& hero : model.heroes) if (!hero.trinkets.empty()) { template_item = &hero.trinkets.front(); break; }
    if (!template_item) fail("No equipped trinket item exists to provide a record template");
    const auto source_path_item = template_item->raw.display_path;
    const auto& source_field = field_at(profile, "persist.roster.json", source_path_item);
    std::string source_id_path = source_path_item;
    if (source_field.kind == core::dson::ValueKind::Object) source_id_path += "/id";
    const auto first_target = hero_data_path(target_hero->persistent_id, "base_root/trinkets/items/0");
    const auto second_target = hero_data_path(target_hero->persistent_id, "base_root/trinkets/items/1");
    const auto item_id_target = [&](const std::string& object_path) {
        return source_field.kind == core::dson::ValueKind::Object ? object_path + "/id" : object_path;
    };
    std::vector<application::CampaignDocumentMutation> mutations;
    for (const auto& [id, target] : {std::pair{generic->id, first_target}, std::pair{specific->id, second_target}}) {
        mutations.push_back(append_clone("Hero.Trinkets", "persist.roster.json", target,
                                         source_path_item, source_field.kind));
        mutations.push_back(set_value(profile, "Hero.Trinkets", "persist.roster.json", item_id_target(target),
                                      source_id_path, id));
    }
    std::ostringstream detail;
    detail << "- 英雄：**" << hero_label(*target_hero) << "**，原饰品栏为空。\n"
           << "- 原版通用饰品：**" << localized_name(environment, *generic) << "** (`" << generic->id << "`)。\n"
           << "- 职业限定 mod 饰品：**" << localized_name(environment, *specific) << "** (`" << specific->id << "`)；来源 **"
           << mod_name(scan, specific->provenance.source_id) << "** (`" << specific->provenance.source_id
           << "`)，职业限制 `" << *target_hero->class_id.value << "`。\n";
    const auto hero_id = target_hero->persistent_id, generic_id = generic->id, specific_id = specific->id;
    return {"05_equip_hero_trinkets", "为英雄装备原版通用与 mod 职业专属饰品", detail.str(), source_path,
        mutation_operation("campaign.hero.equip_trinket", std::move(mutations)),
        [hero_id, generic_id, specific_id](const auto&, const auto& projected) {
            const auto found = std::find_if(projected.heroes.begin(), projected.heroes.end(), [&](const auto& h) { return h.persistent_id == hero_id; });
            if (found == projected.heroes.end() || found->trinkets.size() != 2) fail("Equipped trinkets did not re-project as two hero items");
            const auto count = [&](const std::string& id) {
                return std::count_if(found->trinkets.begin(), found->trinkets.end(), [&](const auto& item) { return item.id == id; });
            };
            if (count(generic_id) != 1 || count(specific_id) != 1) fail("Hero trinket IDs differ from the requested pair");
        }};
}

Scenario make_open_district_system_scenario(const application::RawSaveProfile& profile,
    const std::filesystem::path& source_path, std::vector<std::string> ids) {
    if (ids.empty()) fail("No official DLC district definitions were found");
    const auto& town = *profile.documents.at("persist.town.json").decoded;
    if (find_field(town, "base_root/districts"))
        fail("Source profile already has an open district system; use a closed baseline to generate test 18");
    const auto* town_buildings = find_field(town, "base_root/buildings");
    if (!town_buildings || town_buildings->children.empty()) fail("Town has no building object for a district template");
    const auto& town_template = town.fields.at(town_buildings->children.front());
    const auto bool_template = std::find_if(town.fields.begin(), town.fields.end(), [](const auto& field) {
        return field.kind == core::dson::ValueKind::Boolean;
    });
    if (bool_template == town.fields.end()) fail("Town document has no boolean field for built=false state");
    std::vector<application::CampaignDocumentMutation> mutations;
    mutations.push_back(append_clone("Town.DistrictSystem", "persist.town.json", "base_root/districts",
        town_template.path, core::dson::ValueKind::Object));
    mutations.push_back(clear_children("Town.DistrictSystem", "persist.town.json", "base_root/districts"));
    mutations.push_back(append_clone("Town.DistrictSystem", "persist.town.json", "base_root/districts/buildings",
        "base_root/districts", core::dson::ValueKind::Object));
    for (const auto& id : ids) {
        const auto district_path = "base_root/districts/buildings/" + id;
        mutations.push_back(append_clone("Town.DistrictSystem", "persist.town.json", district_path,
            town_template.path, core::dson::ValueKind::Object));
        mutations.push_back(clear_children("Town.DistrictSystem", "persist.town.json", district_path));
        const auto built_path = district_path + "/built";
        mutations.push_back(append_clone("Town.DistrictSystem", "persist.town.json", built_path,
            bool_template->path, core::dson::ValueKind::Boolean));
        mutations.push_back(set_value(profile, "Town.DistrictSystem", "persist.town.json", built_path,
            bool_template->path, false));
    }
    const auto first = ids.front();
    return {"11_open_district_system", "开放小镇建筑系统", "- 按原版与 DLC 的有效 District 定义建立 " +
        std::to_string(ids.size()) + " 个状态项，初始 `built=false`。\n- 只开放小镇建筑系统，不建造任何地区建筑。\n",
        source_path, mutation_operation("campaign.town.set_district_system_open", std::move(mutations)),
        [ids = std::move(ids), first](const auto&, const auto& projected) {
            if (projected.districts.size() != ids.size() ||
                std::any_of(projected.districts.begin(), projected.districts.end(), [](const auto& item) {
                    return !item.built.value || *item.built.value;
                })) fail("District system did not project every definition as built=false");
            if (std::none_of(projected.districts.begin(), projected.districts.end(), [&](const auto& item) { return item.id == first; }))
                fail("Representative district is missing after system open");
        }};
}

Scenario make_close_district_system_scenario(const application::RawSaveProfile& profile,
    const std::filesystem::path& source_path) {
    if (!find_field(*profile.documents.at("persist.town.json").decoded, "base_root/districts"))
        fail("District-system lock requires a baseline where the system is open");
    auto mutation = erase_entry("Town.DistrictSystem", "persist.town.json", "base_root/districts",
                                core::dson::ValueKind::Object);
    return {"14_lock_district_system", "锁定小镇建筑系统", "- 删除 `base_root/districts` 系统状态，保持普通小镇建筑和升级购买历史。\n",
        source_path, mutation_operation("campaign.town.set_district_system_open", {std::move(mutation)}),
        [](const auto& profile_after, const auto& projected) {
            if (!projected.districts.empty() || find_field(*profile_after.documents.at("persist.town.json").decoded, "base_root/districts"))
                fail("District-system lock left district state behind");
        }};
}

Scenario make_set_district_built_scenario(const domain::CampaignModel& model, std::string district_id, bool built,
    std::filesystem::path source_path, std::string output_directory) {
    const auto existing = std::find_if(model.districts.begin(), model.districts.end(), [&](const auto& item) {
        return item.id == district_id && item.built.value;
    });
    if (existing == model.districts.end()) fail("District state is not present in this scenario baseline");
    const bool current = *existing->built.value;
    if (current == built) fail("District already has the requested built state");
    return {std::move(output_directory), built ? "解锁一项小镇建筑" : "锁定一项小镇建筑",
        "- 小镇建筑：`" + district_id + "`，`built` 从 " + (current ? "true" : "false") + " 改为 " +
        (built ? "true" : "false") + "。\n- 本存档以系统已开放的前置档为基线。\n",
        std::move(source_path), application::SetDistrictBuiltOperation{std::move(district_id), built},
        [district_id = existing->id, built](const auto&, const auto& projected) {
            const auto target = std::find_if(projected.districts.begin(), projected.districts.end(), [&](const auto& item) {
                return item.id == district_id;
            });
            if (target == projected.districts.end() || !target->built.value || *target->built.value != built)
                fail("District built state failed semantic re-projection");
        }};
}

Scenario make_camping_unequip_scenario(const domain::CampaignModel& model,
    std::filesystem::path source_path) {
    for (const auto& hero : model.heroes) {
        const auto skill = std::find_if(hero.camping_skills.begin(), hero.camping_skills.end(), [](const auto& item) {
            return item.camping;
        });
        if (skill == hero.camping_skills.end()) continue;
        const auto hero_id = hero.persistent_id, skill_id = skill->id;
        const auto detail = "- 英雄：**" + hero_label(hero) + "**。\n- 取消装备生存技能 `" + skill_id +
            "`；训练营购买状态不变。\n";
        return {"09_unequip_camping_skill", "取消装备一项生存技能", detail, std::move(source_path),
            application::UnequipHeroCampingSkillOperation{hero_id, skill_id},
            [hero_id, skill_id](const auto&, const auto& projected) {
                const auto found = std::find_if(projected.heroes.begin(), projected.heroes.end(), [&](const auto& h) { return h.persistent_id == hero_id; });
                if (found == projected.heroes.end() || std::any_of(found->camping_skills.begin(), found->camping_skills.end(),
                    [&](const auto& value) { return value.id == skill_id; }))
                    fail("Camping skill remains selected after unequip");
            }};
    }
    fail("No hero has an equipped camping skill for the unequip test");
}

Scenario make_equipment_scenario(const domain::CampaignModel& model, const domain::Hero& hero, std::int32_t weapon_rank,
    std::int32_t armour_rank, std::int32_t effective_weapon_max, std::int32_t effective_armour_max,
    std::filesystem::path source_path, std::string directory) {
    auto operation = must(application::make_set_hero_equipment_ranks_operation(
        model, hero.persistent_id, weapon_rank, armour_rank, effective_weapon_max, effective_armour_max),
        "Plan equipment-rank operation");
    const auto details = "- 英雄：**" + hero_label(hero) + "**。\n- 武器 raw rank：" +
        std::to_string(hero.weapon_rank.value.value_or(-1)) + " → " + std::to_string(weapon_rank) +
        "（界面等级 " + std::to_string(hero.weapon_rank.value.value_or(-1) + 1) + " → " +
        std::to_string(weapon_rank + 1) + "）。\n- 防具 raw rank：" +
        std::to_string(hero.armour_rank.value.value_or(-1)) + " → " + std::to_string(armour_rank) +
        "（界面等级 " + std::to_string(hero.armour_rank.value.value_or(-1) + 1) + " → " +
        std::to_string(armour_rank + 1) + "）。\n- 英雄字段与武器/防具购买节点由一个复合 Operation 原子写入。\n";
    const auto hero_id = hero.persistent_id, class_id = hero.class_id.value.value_or("");
    return {std::move(directory), "同时设置武器和防具等级", details, std::move(source_path), std::move(operation),
        [hero_id, class_id, weapon_rank, armour_rank, effective_weapon_max, effective_armour_max](const auto&, const auto& projected) {
            const auto found = std::find_if(projected.heroes.begin(), projected.heroes.end(), [&](const auto& h) { return h.persistent_id == hero_id; });
            if (found == projected.heroes.end() || found->weapon_rank.value.value_or(-1) != weapon_rank ||
                found->armour_rank.value.value_or(-1) != armour_rank)
                fail("Equipment ranks did not re-project to the requested values");
            const auto instance = upgrade_instance(projected, *found);
            if (!instance || current_rank(projected, *instance, class_id + ".weapon", '0', effective_weapon_max) != weapon_rank ||
                current_rank(projected, *instance, class_id + ".armour", '0', effective_armour_max) != armour_rank)
                fail("Equipment purchase rows do not match the hero rank fields");
        }};
}

Scenario make_combat_skill_scenario(const domain::CampaignModel& model,
    const application::IContentEnvironment& environment,
    const domain::Hero& hero, const application::ContentDefinition& skill, std::int32_t max_rank,
    std::int32_t target_rank, std::filesystem::path source_path, std::string directory) {
    auto operation = must(application::make_set_hero_combat_skill_rank_operation(
        model, hero.persistent_id, skill.id, target_rank, max_rank), "Plan combat-skill rank operation");
    const auto instance = upgrade_instance(model, hero);
    if (!instance) fail("Combat skill hero purchase instance is not unique");
    const auto suffix_at = skill.id.find(':');
    const auto tree = hero.class_id.value.value_or("") + "." +
        (suffix_at == std::string::npos ? skill.id : skill.id.substr(suffix_at + 1));
    const auto current = current_rank(model, *instance, tree, '0', max_rank);
    const auto detail = "- 英雄：**" + hero_label(hero) + "**。\n- 战斗技能：**" +
        localized_name(environment, skill) + "** (`" + skill.id + "`)；有效 Mod 技能树等级上限 " +
        std::to_string(max_rank) + "。\n- 购买节点等级：" + std::to_string(current) + " → " +
        std::to_string(target_rank) + "；不改英雄等级或经验。\n";
    const auto hero_id = hero.persistent_id;
    return {std::move(directory), "设置战斗技能等级", detail, std::move(source_path), std::move(operation),
        [hero_id, tree, instance = *instance, target_rank, max_rank](const auto&, const auto& projected) {
            if (current_rank(projected, instance, tree, '0', max_rank) != target_rank)
                fail("Combat skill purchase history did not re-project to the requested rank");
            if (std::none_of(projected.heroes.begin(), projected.heroes.end(), [&](const auto& h) { return h.persistent_id == hero_id; }))
                fail("Combat-skill target hero disappeared after writeback");
        }};
}

Scenario make_camping_unlock_scenario(const domain::CampaignModel& model,
    const application::IContentEnvironment& environment, const domain::Hero& hero,
    const application::ContentDefinition& skill, std::string suffix,
    std::filesystem::path source_path) {
    auto operation = must(application::make_set_hero_camping_skill_learned_operation(
        model, hero.persistent_id, suffix, true), "Plan camping training operation");
    const auto instance = upgrade_instance(model, hero);
    if (!instance) fail("Camping hero purchase instance is not unique");
    const auto tree = hero.class_id.value.value_or("") + "." + suffix;
    return {"08_unlock_camping_skill", "解锁一个生存训练营技能",
        "- 英雄：**" + hero_label(hero) + "**。\n- 生存技能：**" + localized_name(environment, skill) +
        "** (`" + suffix + "`)；训练营学习状态改为已学会，不自动装备。\n",
        std::move(source_path), std::move(operation),
        [instance = *instance, tree](const auto&, const auto& projected) {
            if (current_rank(projected, instance, tree, '0', 1) != 1)
                fail("Camping training node did not re-project as learned");
        }};
}

Scenario make_town_upgrade_scenario(const domain::CampaignModel& model,
    std::int32_t target_rank, std::int32_t max_rank, std::filesystem::path source_path,
    std::string directory) {
    const std::string tree = "stage_coach.rostersize";
    auto operation = must(application::make_set_town_upgrade_rank_operation(model, tree, target_rank, max_rank),
                          "Plan town upgrade operation");
    const auto current = current_rank(model, 0, tree, 'a', max_rank);
    return {std::move(directory), "设置马车名单容量升级进度",
        "- 建筑：马车（Stagecoach），升级树 `" + tree + "`。\n- 已购买节点：" +
        std::to_string(current) + " → " + std::to_string(target_rank) + "；后续节点回锁。\n",
        std::move(source_path), std::move(operation),
        [tree, target_rank, max_rank](const auto&, const auto& projected) {
            if (current_rank(projected, 0, tree, 'a', max_rank) != target_rank)
                fail("Town upgrade purchase nodes did not re-project to the requested rank");
        }};
}

std::pair<application::RawSaveProfile, domain::CampaignModel> load_profile_model(
    const std::filesystem::path& path, const application::IFileSystem& file_system,
    const application::IContentEnvironment& environment) {
    application::SaveProfileDiscovery discovery{file_system};
    auto profile = must(discovery.load(path), "Load dependent test profile");
    auto model = application::CampaignModelBuilder{}.build(profile, environment);
    if (model.state == domain::ModelState::Invalid) fail("Dependent test profile has an invalid semantic model");
    return {std::move(profile), std::move(model)};
}

void write_readme(const std::filesystem::path& path, std::string_view source_path,
                  std::uint64_t source_fingerprint, const std::vector<GeneratedScenario>& scenarios,
                  std::size_t mod_count) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) fail("Cannot create Stage 12 acceptance checklist: " + path.string());
    output << "# Stage 12 Operation 写回测试档\n\n"
           << "本批每项测试都通过 `CampaignEditSession::apply(Operation)`、`SaveAdapter` 和 `SafeSaveCommitter` 生成。"
           << "每个 `profile_0` 都是独立档案副本；`write_backup` 是安全提交器在写入前创建并校验的完整副本。\n\n"
           << "- 源存档：`" << source_path << "`\n"
           << "- 源存档指纹：`0x" << std::hex << source_fingerprint << std::dec << "`\n"
           << "- 启用 mod 数：" << mod_count << "（顺序按源存档解析）\n"
           << "- 测试存档数：" << scenarios.size() << "；另有 `00_control/profile_0` 对照组。\n\n"
           << "## 验收步骤\n\n"
           << "1. 先用 `00_control/profile_0` 确认环境和对照档可正常载入。\n"
           << "2. 每次只将一个目录里的 `profile_0` 放入游戏存档位置；完整退出游戏后再换下一项。\n"
           << "3. 按下面每项写明的英雄、技能、怪癖、饰品或升级检查界面结果；保存并重载一次，确认变更持久。\n"
           << "4. 有依赖关系的用例按目录名先测前置项。完成后记录通过/失败及游戏内提示。\n\n"
           << "## 测试项\n\n";
    for (const auto& scenario : scenarios) {
        output << "### `" << scenario.directory << "/profile_0` — " << scenario.title << "\n\n"
               << scenario.details << "\n"
               << "**验收：**档案可载入；目标值与说明一致；无 roster / inventory / town 数据异常；保存并重载后仍成立。\n\n"
               << "**写入文档：**";
        for (const auto& document : scenario.affected_documents) output << " `" << document << "`";
        output << "\n\n";
    }
    output << "## 写回和验收边界\n\n"
           << "- 测试生成器使用 `AcceptanceTestCandidate` 模式；只接受与源存档逐字节一致的独立副本，并在写入前创建完整备份、写后重新解析验证。\n"
           << "- 仍处于 `VERIFIED_SAMPLE` 的映射只用于生成待验收测试副本；普通 `SafeSaveCommitter` 默认模式继续拒绝这些映射。\n"
           << "- 每项具体依赖关系写在该测试条目的说明中。\n\n"
           << "## 自动验证范围\n\n"
           << "生成时已由 SaveAdapter 检查映射范围、before-value、DSON 结构与回读语义；SafeSaveCommitter 检查源档基线、"
           << "建立完整备份、写入副本并再次解析验证。自动检查不代替你在游戏中的最终确认。\n";
    output.close();
    if (!output) fail("Failed to flush the Stage 12 acceptance checklist");
}

void create_control_copy(const std::filesystem::path& source,
                         const std::filesystem::path& staging_root) {
    copy_profile(source, staging_root / "00_control" / "profile_0");
}

std::uint64_t fingerprint(const application::IFileSystem& file_system,
                          const std::filesystem::path& profile) {
    return must(application::SaveProfileDiscovery::fingerprint_profile(file_system, profile),
                "Fingerprint profile");
}

void run(const Options& incoming) {
    Options options = incoming;
    options.source = absolute_path(options.source);
    options.game = absolute_path(options.game);
    options.workshop = absolute_path(options.workshop);
    options.local = absolute_path(options.local);
    options.output = absolute_path(options.output);
    if (!std::filesystem::exists(options.source)) fail("Source profile does not exist: " + options.source.string());
    if (!std::filesystem::exists(options.game)) fail("Game root does not exist: " + options.game.string());
    if (std::filesystem::exists(options.output)) fail("Refusing to overwrite existing output tree: " + options.output.string());

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto staging_root = options.output.parent_path() /
        (options.output.filename().string() + ".staging-" + std::to_string(stamp));
    const auto temporary = std::filesystem::temp_directory_path() / ("ddse-stage12-" + std::to_string(stamp));
    std::filesystem::create_directories(staging_root);
    std::filesystem::create_directories(temporary);
    const GeneratedTreeCleanup cleanup{staging_root, temporary};

    infrastructure::NativeFileSystem file_system;
    application::SaveProfileDiscovery discovery{file_system};
    auto source_profile = must(discovery.load(options.source), "Load source save profile");
    const auto source_fingerprint = source_profile.baseline_fingerprint;

    application::BaseContentScanner base_scanner{file_system};
    auto base_scan = must(base_scanner.scan(application::BaseContentScanConfig::defaults(options.game)),
                          "Scan read-only base/DLC content");
    const auto base_db = temporary / "base.db";
    const auto base_summary = must(infrastructure::BaseContentDatabaseBuilder{}.rebuild(base_db, base_scan),
                                   "Build temporary base content database");
    application::ModEnvironmentScanConfig mod_config;
    mod_config.workshop_root = options.workshop;
    mod_config.local_mod_roots = {options.local};
    mod_config.save_profile_root = options.source;
    mod_config.prefer_manager_export = false;
    mod_config.base_content_database = base_db;
    application::ModEnvironmentScanner mod_scanner{file_system};
    auto mod_scan = must(mod_scanner.scan(mod_config), "Scan save-selected mod environment");
    if (mod_scan.effective_order_source != "save_profile" || mod_scan.effective_order.size() != mod_scan.save_order.size())
        fail("Could not resolve every enabled mod from the source save order");
    const auto mod_db = temporary / "mods.db";
    const auto mod_summary = must(infrastructure::ModEnvironmentDatabaseBuilder{}.rebuild(mod_db, base_db, mod_scan),
                                  "Build temporary mod content database");
    application::ContentEnvironmentSelection selection;
    selection.language = "english";
    selection.fallback_language = "english";
    infrastructure::SqliteContentEnvironment environment({base_db, mod_db, selection});
    const auto model = application::CampaignModelBuilder{}.build(source_profile, environment);
    if (model.state == domain::ModelState::Invalid || model.heroes.empty()) fail("Source campaign model is invalid or empty");
    const auto skills = content_list(environment, "skill");
    const auto quirks = content_list(environment, "quirk");
    const auto trinkets = content_list(environment, "trinket");
    const auto classes = content_list(environment, "hero_class");
    std::optional<application::ContentDefinition> generic_vanilla_trinket;
    for (const auto& item : base_scan.definitions) {
        if (item.kind != "trinket") continue;
        const auto payload = Json::parse(item.payload_json, nullptr, false);
        if (!payload.is_object() || !payload.contains("hero_class_requirements") ||
            !payload["hero_class_requirements"].is_array() || !payload["hero_class_requirements"].empty()) continue;
        application::ContentDefinition definition;
        definition.type = item.kind;
        definition.id = item.id;
        definition.display_name = item.display_name;
        definition.localization_key = item.localization_key;
        definition.payload_json = item.payload_json;
        definition.provenance = {item.source_id, "base", item.virtual_path, true};
        generic_vanilla_trinket = std::move(definition);
        break;
    }
    if (!generic_vanilla_trinket && options.focus == "all")
        fail("No generic vanilla/DLC trinket is available for the equip test");
    std::cout << "Source roster=" << model.heroes.size() << ", selected mods=" << mod_scan.effective_order.size()
              << ", mod definitions=" << mod_summary.definitions << ", diagnostics=" << mod_scan.diagnostics.size() << "\n";

    create_control_copy(options.source, staging_root);
    std::vector<GeneratedScenario> completed;
    const auto add_case = [&](const Scenario& scenario) {
        completed.push_back(write_scenario(scenario, environment, staging_root));
        std::cout << "Generated " << scenario.directory << "\n";
    };
    if (options.focus == "all") {
    add_case(make_add_hero_scenario(source_profile, model, classes, environment, options.source));
    add_case(make_replace_quirks_scenario(source_profile, model, quirks, environment, mod_scan, options.source));
    add_case(make_add_quirks_scenario(source_profile, model, quirks, environment, mod_scan, options.source));
    add_case(make_add_inventory_trinket_scenario(source_profile, model, trinkets, environment, mod_scan, options.source));
    add_case(make_equip_trinkets_scenario(source_profile, model, trinkets, *generic_vanilla_trinket,
                                           environment, mod_scan, options.source));

    const auto first = hero_at(model, 0);
    const auto second = hero_at(model, std::min<std::size_t>(1, model.heroes.size() - 1));
    const auto second_weapon_max = effective_equipment_tree_maximum(environment, file_system, second, "weapon");
    const auto second_armour_max = effective_equipment_tree_maximum(environment, file_system, second, "armour");
    const auto first_weapon_max = effective_equipment_tree_maximum(environment, file_system, first, "weapon");
    const auto first_armour_max = effective_equipment_tree_maximum(environment, file_system, first, "armour");
    if (!second_weapon_max || !second_armour_max || !first_weapon_max || !first_armour_max ||
        *second_weapon_max < 3 || *second_armour_max < 4 || *first_weapon_max < 1 || *first_armour_max < 1)
        fail("Effective hero equipment trees do not support the planned test ranks (second=" +
             (second_weapon_max ? std::to_string(*second_weapon_max) : "missing") + "/" +
             (second_armour_max ? std::to_string(*second_armour_max) : "missing") + ", first=" +
             (first_weapon_max ? std::to_string(*first_weapon_max) : "missing") + "/" +
             (first_armour_max ? std::to_string(*first_armour_max) : "missing") + ")");
    add_case(make_equipment_scenario(model, second, 3, 4, *second_weapon_max, *second_armour_max,
                                     options.source, "06_upgrade_hero_equipment"));
    add_case(make_equipment_scenario(model, first, 1, 0, *first_weapon_max, *first_armour_max,
                                     options.source, "07_downgrade_hero_equipment"));

    std::optional<Scenario> skill_upgrade;
    std::optional<Scenario> skill_downgrade;
    for (const auto& hero : model.heroes) {
        if (!hero.class_id.value || !hero.resolve_xp.value) continue;
        const auto instance = upgrade_instance(model, hero);
        if (!instance) continue;
        for (const auto& skill : skills) {
            if (!skill.id.starts_with(*hero.class_id.value + ":")) continue;
            const auto maximum = effective_combat_skill_maximum(environment, file_system, hero, skill);
            if (!maximum || *maximum < 2) continue;
            const auto colon = skill.id.find(':');
            const auto tree = *hero.class_id.value + "." + skill.id.substr(colon + 1);
            const auto rank = current_rank(model, *instance, tree, '0', *maximum);
            if (!skill_upgrade && *hero.resolve_xp.value <= 8 && rank < *maximum) {
                skill_upgrade = make_combat_skill_scenario(model, environment, hero, skill, *maximum, *maximum,
                    options.source, "10_upgrade_combat_skill_to_max");
            }
            if (!skill_downgrade && *hero.resolve_xp.value >= 36 && rank == *maximum && *maximum >= 3) {
                skill_downgrade = make_combat_skill_scenario(model, environment, hero, skill, *maximum, 2,
                    options.source, "10a_downgrade_combat_skill_to_2");
            }
        }
    }
    if (!skill_upgrade || !skill_downgrade)
        fail("Could not find both a low-level upgradable and high-level maxed combat skill using the effective mod trees");
    add_case(*skill_upgrade);
    add_case(*skill_downgrade);

    std::optional<Scenario> camping_unlock;
    for (const auto& hero : model.heroes) {
        if (!hero.class_id.value || !hero.resolve_xp.value || *hero.resolve_xp.value > 8) continue;
        const auto instance = upgrade_instance(model, hero);
        if (!instance) continue;
        for (const auto& skill : skills) {
            if (!is_camping_skill_for(skill, hero)) continue;
            const auto colon = skill.id.find(':');
            const auto suffix = skill.id.substr(colon == std::string::npos ? 0 : colon + 1);
            const auto tree = *hero.class_id.value + "." + suffix;
            if (current_rank(model, *instance, tree, '0', 1) == 0) {
                camping_unlock = make_camping_unlock_scenario(model, environment, hero, skill, suffix, options.source);
                break;
            }
        }
        if (camping_unlock) break;
    }
    if (!camping_unlock) fail("No low-level hero has an unlearned camping skill node");
    add_case(*camping_unlock);
    add_case(make_camping_unequip_scenario(model, options.source));

    const std::string town_tree = "stage_coach.rostersize";
    const auto town_max_value = effective_upgrade_tree_maximum(environment, file_system,
        "upgrades/building/stage_coach.upgrades.json", town_tree);
    if (!town_max_value) fail("Could not resolve the effective Stagecoach roster-size upgrade tree");
    const auto town_max = *town_max_value;
    const auto town_current = current_rank(model, 0, town_tree, 'a', town_max);
    if (town_max < 3 || town_current <= 1) fail("Stagecoach roster size does not have the expected upgrade nodes for tests 14/15");
    add_case(make_town_upgrade_scenario(model, 1, town_max, options.source, "11_town_upgrade_reduce"));
    const auto reduced_path = staging_root / "11_town_upgrade_reduce" / "profile_0";
    auto [reduced_profile, reduced_model] = load_profile_model(reduced_path, file_system, environment);
    add_case(make_town_upgrade_scenario(reduced_model, 3, town_max, reduced_path, "12_town_upgrade_restore"));

    const auto ids = district_ids(options);
    add_case(make_open_district_system_scenario(source_profile, options.source, ids));
    const auto open_path = staging_root / "11_open_district_system" / "profile_0";
    auto [open_profile, open_model] = load_profile_model(open_path, file_system, environment);
    const auto district_id = ids.front();
    auto unlock = make_set_district_built_scenario(open_model, district_id, true, open_path,
                                                   "13_unlock_district_building");
    add_case(unlock);
    const auto unlock_path = staging_root / "13_unlock_district_building" / "profile_0";
    auto [unlocked_profile, unlocked_model] = load_profile_model(unlock_path, file_system, environment);
    add_case(make_set_district_built_scenario(unlocked_model, district_id, false, unlock_path,
                                               "14_lock_district_building"));
    add_case(make_close_district_system_scenario(open_profile, open_path));
    } else {
        add_case(make_replace_quirks_scenario(source_profile, model, quirks, environment, mod_scan, options.source));
        add_case(make_affliction_and_stress_scenario(model, options.game, options.source));
        add_case(make_hero_name_scenario(model, options.source));
    }

    const auto source_unchanged = must(source_profile.matches_disk_baseline(file_system), "Verify immutable source profile");
    if (!source_unchanged || fingerprint(file_system, options.source) != source_fingerprint)
        fail("Original source profile changed during Stage 12 test generation");
    write_readme(staging_root / "README.md", options.source.string(), source_fingerprint, completed,
                 mod_scan.effective_order.size());
    std::filesystem::rename(staging_root, options.output);
    const auto base_definition_count = base_summary.hero_classes + base_summary.skills + base_summary.trinkets +
        base_summary.quirks + base_summary.diseases + base_summary.resources + base_summary.buildings;
    std::cout << "Generated " << completed.size() << " operation-backed profiles plus control at "
              << options.output.string() << "\nBase definitions=" << base_definition_count
              << ", enabled mods=" << mod_scan.effective_order.size() << ", diagnostics=" << mod_scan.diagnostics.size() << "\n";
    std::error_code ignored;
    std::filesystem::remove_all(temporary, ignored);
}

int run(const std::vector<std::string>& args) {
    try {
        run(options_from(args));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Stage 12 Operation test-save generation failed: " << error.what() << '\n';
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int main() {
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) { LocalFree(argv); return 2; }
        std::string converted(static_cast<std::size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, converted.data(), bytes, nullptr, nullptr) <= 0) {
            LocalFree(argv);
            return 2;
        }
        converted.resize(static_cast<std::size_t>(bytes - 1));
        args.push_back(std::move(converted));
    }
    LocalFree(argv);
    return run(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return run(args);
}
#endif
