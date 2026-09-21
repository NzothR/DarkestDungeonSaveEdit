#include "ddse/application/campaign_model_builder.hpp"
#include "ddse/application/campaign_mappings.hpp"
#include "ddse/application/content_environment.hpp"
#include "ddse/application/content_scanner.hpp"
#include "ddse/application/mod_environment.hpp"
#include "ddse/application/save_profile.hpp"
#include "ddse/core/dson/dson_document_editor.hpp"
#include "ddse/core/dson/dson_reader.hpp"
#include "ddse/core/dson/dson_writer.hpp"
#include "ddse/infrastructure/base_content_database.hpp"
#include "ddse/infrastructure/mod_environment_database.hpp"
#include "ddse/infrastructure/native_file_system.hpp"
#include "ddse/infrastructure/sqlite_content_environment.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

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

struct Options {
    std::filesystem::path source_profile;
    std::filesystem::path game_root;
    std::filesystem::path workshop_root;
    std::filesystem::path local_mod_root;
    std::filesystem::path output_root;
};

struct Scenario {
    Scenario(std::string directory_value, std::string title_value, std::string details_value,
             std::map<std::string, std::vector<std::byte>, std::less<>> documents_value)
        : directory(std::move(directory_value)), title(std::move(title_value)), details(std::move(details_value)),
          changed_documents(std::move(documents_value)) {}

    std::string directory;
    std::string title;
    std::string details;
    std::map<std::string, std::vector<std::byte>, std::less<>> changed_documents;
    std::function<void(const application::RawSaveProfile&)> validate_readback;
};

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

std::filesystem::path absolute_normal(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::absolute(path, error);
    if (error) fail("Cannot make path absolute: " + path.string() + " (" + error.message() + ")");
    return result.lexically_normal();
}

std::string comparable_path(const std::filesystem::path& path) {
    auto value = absolute_normal(path).generic_string();
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
#endif
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

bool is_within_or_equal(const std::filesystem::path& candidate, const std::filesystem::path& parent) {
    const auto child = comparable_path(candidate);
    const auto base = comparable_path(parent);
    return child == base || (child.size() > base.size() && child.starts_with(base) && child[base.size()] == '/');
}

Options parse_options(const std::vector<std::string>& arguments) {
    Options result;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& key = arguments[index];
        if (key == "--source-profile" || key == "--game-root" || key == "--workshop-root" ||
            key == "--local-mod-root" || key == "--output-root") {
            if (index + 1 >= arguments.size()) fail("Missing value after " + key);
            const auto& encoded_path = arguments[++index];
            const auto path = std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(encoded_path.data()), encoded_path.size()));
            if (key == "--source-profile") result.source_profile = path;
            else if (key == "--game-root") result.game_root = path;
            else if (key == "--workshop-root") result.workshop_root = path;
            else if (key == "--local-mod-root") result.local_mod_root = path;
            else result.output_root = path;
        } else {
            fail("Unknown argument: " + key);
        }
    }
    if (result.source_profile.empty() || result.game_root.empty() || result.workshop_root.empty() ||
        result.local_mod_root.empty() || result.output_root.empty())
        fail("Usage: ddse_generate_game_test_saves --source-profile <dir> --game-root <dir> "
             "--workshop-root <dir> --local-mod-root <dir> --output-root <dir>");
    return result;
}

template <class T>
T require_value(core::Result<T, core::Error> result, std::string_view operation) {
    if (!result) fail(std::string{operation} + ": " + std::string{core::to_string(result.error().code)} + ": " + result.error().message);
    return std::move(result).value();
}

void require_success(core::Result<void, core::Error> result, std::string_view operation) {
    if (!result) fail(std::string{operation} + ": " + std::string{core::to_string(result.error().code)} + ": " + result.error().message);
}

std::string to_string(const std::vector<std::byte>& bytes) {
    std::string result(bytes.size(), '\0');
    if (!bytes.empty()) std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

DsonDocument deep_clone(const DsonDocument& source) {
    auto result = source;
    for (std::size_t index = 0; index < source.fields.size(); ++index) {
        if (source.fields[index].embedded_document)
            result.fields[index].embedded_document = std::make_shared<DsonDocument>(
                deep_clone(*source.fields[index].embedded_document));
    }
    return result;
}

DsonField& field_at(DsonDocument& document, std::string_view path) {
    const auto found = std::find_if(document.fields.begin(), document.fields.end(), [&](const DsonField& field) {
        return field.path == path;
    });
    if (found == document.fields.end()) fail("DSON field not found: " + std::string{path});
    return *found;
}

const DsonField& field_at(const DsonDocument& document, std::string_view path) {
    const auto found = std::find_if(document.fields.begin(), document.fields.end(), [&](const DsonField& field) {
        return field.path == path;
    });
    if (found == document.fields.end()) fail("DSON field not found: " + std::string{path});
    return *found;
}

void set_string(DsonDocument& document, std::string_view path, std::string value) {
    auto& field = field_at(document, path);
    if (field.kind != core::dson::ValueKind::String) fail("Expected string field at " + std::string{path});
    field.replace_value(std::move(value));
}

void set_integer(DsonDocument& document, std::string_view path, std::int32_t value) {
    auto& field = field_at(document, path);
    if (field.kind != core::dson::ValueKind::Integer) fail("Expected integer field at " + std::string{path});
    field.replace_value(value);
}

void set_boolean(DsonDocument& document, std::string_view path, bool value) {
    auto& field = field_at(document, path);
    if (field.kind != core::dson::ValueKind::Boolean) fail("Expected boolean field at " + std::string{path});
    field.replace_value(value);
}

std::vector<std::byte> encode_candidate(const DsonDocument& document, std::string_view id) {
    core::dson::DsonWriter writer;
    auto encoded = writer.encode(document);
    if (!encoded) fail("Cannot encode " + std::string{id} + ": " + encoded.error().message);
    core::dson::DsonReader reader;
    auto decoded = reader.parse(encoded.value(), id);
    if (!decoded) fail("Candidate failed DSON read-back validation for " + std::string{id} + ": " + decoded.error().message);
    return std::move(encoded).value();
}

const application::RawSaveDocument& source_document(const application::RawSaveProfile& profile,
                                                     std::string_view id) {
    const auto found = profile.documents.find(std::string{id});
    if (found == profile.documents.end() || !found->second.decoded)
        fail("Required decoded save document is unavailable: " + std::string{id});
    return found->second;
}

std::filesystem::path mod_temp_root() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("ddse-stage10-game-test-" + std::to_string(stamp));
}

std::optional<std::size_t> find_field_index(const DsonDocument& document, std::string_view path) {
    for (std::size_t index = 0; index < document.fields.size(); ++index)
        if (document.fields[index].path == path) return index;
    return std::nullopt;
}

std::string localized_name(const application::IContentEnvironment& environment,
                           const application::ContentDefinition& definition) {
    if (!definition.localization_key.empty()) {
        const auto localized = environment.resolve_localization(definition.localization_key, "english");
        if (localized && localized.value() && !localized.value()->value.empty()) return localized.value()->value;
    }
    if (!definition.localized_name.empty()) return definition.localized_name;
    if (!definition.display_name.empty()) return definition.display_name;
    return definition.id;
}

std::string mod_display_name(const application::ModEnvironmentScanResult& scan, std::string_view id) {
    const auto found = std::find_if(scan.mods.begin(), scan.mods.end(), [&](const auto& mod) { return mod.id == id; });
    return found == scan.mods.end() || found->display_name.empty() ? std::string{id} : found->display_name;
}

std::int32_t integer_value(const domain::LocatedValue<std::int32_t>& value, std::int32_t fallback) {
    return value.value.value_or(fallback);
}

std::string hero_outer_path(std::string_view hero_id) {
    return "base_root/heroes/" + std::string{hero_id};
}

std::string hero_embedded_path(std::string_view hero_id) {
    return hero_outer_path(hero_id) + "/hero_file_data/raw_data";
}

DsonDocument& hero_inner_document(DsonDocument& roster, std::string_view hero_id) {
    auto& raw_data = field_at(roster, hero_embedded_path(hero_id));
    if (!raw_data.embedded_document) fail("Hero embedded data could not be decoded: " + std::string{hero_id});
    return *raw_data.embedded_document;
}

std::string class_display(const domain::Hero& hero) {
    return hero.definition.display_name.empty() ? hero.class_id.value.value_or("unknown") : hero.definition.display_name;
}

std::string hero_display(const domain::Hero& hero) {
    return hero.name.value.value_or("(unnamed)");
}

std::string quirk_inner_path(std::string_view quirk_id) {
    return "base_root/quirks/" + std::string{quirk_id};
}

void initialize_quirk(DsonDocument& hero_data, std::string_view quirk_id) {
    const auto root = quirk_inner_path(quirk_id);
    set_boolean(hero_data, root + "/is_new", true);
    set_boolean(hero_data, root + "/is_locked", false);
    const auto set_int_if_present = [&](std::string_view leaf, std::int32_t value) {
        if (find_field_index(hero_data, root + "/" + std::string{leaf}))
            set_integer(hero_data, root + "/" + std::string{leaf}, value);
    };
    const auto set_bool_if_present = [&](std::string_view leaf, bool value) {
        if (find_field_index(hero_data, root + "/" + std::string{leaf}))
            set_boolean(hero_data, root + "/" + std::string{leaf}, value);
    };
    set_int_if_present("trinketId", -1);
    set_int_if_present("mission_count", 0);
    set_int_if_present("replaces_quirk", -1);
    set_bool_if_present("replaces_quirk_viewed", false);
    set_int_if_present("evolution_duration_remaining", 0);
}

std::set<std::string, std::less<>> hero_quirk_ids(const domain::Hero& hero) {
    std::set<std::string, std::less<>> result;
    for (const auto& quirk : hero.quirks)
        if (!quirk.is_disease) result.insert(quirk.id);
    return result;
}

bool is_mod_quirk(const application::ContentDefinition& definition, bool positive) {
    if (definition.provenance.layer_type != "mod" || !definition.provenance.selected) return false;
    const auto payload = nlohmann::json::parse(definition.payload_json, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) return false;
    return payload.value("is_disease", true) == false && payload.value("is_positive", !positive) == positive &&
           payload.value("can_be_replaced_by_new_quirk", false) == true;
}

const application::ContentDefinition* find_mod_quirk(
    const std::vector<application::ContentDefinition>& definitions, bool positive,
    const std::set<std::string, std::less<>>& excluded) {
    for (const auto& definition : definitions)
        if (is_mod_quirk(definition, positive) && !excluded.contains(definition.id)) return &definition;
    return nullptr;
}

std::size_t numeric_child_index(std::string_view value) {
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        return std::numeric_limits<std::size_t>::max();
    return result;
}

std::vector<application::ContentDefinition> content_list(const application::IContentEnvironment& environment,
                                                         std::string_view type) {
    return require_value(environment.list_content(type), "List " + std::string{type} + " definitions");
}

const application::ContentDefinition* find_definition_by_id(
    const std::vector<application::ContentDefinition>& definitions, std::string_view id) {
    const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto& item) {
        return item.id == id && item.provenance.layer_type == "mod" && item.provenance.selected;
    });
    return found == definitions.end() ? nullptr : &*found;
}

struct HeroAdditionChoice {
    const domain::Hero* source_hero{};
    const application::ContentDefinition* class_definition{};
    std::string new_hero_id;
    std::string new_name{"DDSE_STAGE10_HERO"};
};

HeroAdditionChoice choose_hero_to_add(const domain::CampaignModel& model,
                                      const std::vector<application::ContentDefinition>& classes) {
    const domain::Hero* selected = nullptr;
    const application::ContentDefinition* selected_class = nullptr;
    for (const auto& hero : model.heroes) {
        if (!hero.class_id.value || hero.definition.layer_type != "mod" ||
            !hero.resolve_xp.value || *hero.resolve_xp.value != 0 ||
            integer_value(hero.weapon_rank, -1) != 0 || integer_value(hero.armour_rank, -1) != 0)
            continue;
        const auto* definition = find_definition_by_id(classes, *hero.class_id.value);
        if (!definition) continue;
        if (selected == nullptr || hero.roster_position < selected->roster_position) {
            selected = &hero;
            selected_class = definition;
        }
    }
    if (selected == nullptr || selected_class == nullptr)
        fail("The save has no level-one mod hero to clone safely. The generator will not invent an unverified hero template.");

    std::uint64_t max_id = 0;
    for (const auto& hero : model.heroes) {
        std::uint64_t numeric = 0;
        const auto parsed = std::from_chars(hero.persistent_id.data(),
            hero.persistent_id.data() + hero.persistent_id.size(), numeric);
        if (parsed.ec == std::errc{} && parsed.ptr == hero.persistent_id.data() + hero.persistent_id.size())
            max_id = std::max(max_id, numeric);
    }
    if (max_id >= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
        fail("Roster hero identifiers have reached the safe numeric test range");
    auto new_id = std::to_string(max_id + 1);
    while (std::any_of(model.heroes.begin(), model.heroes.end(), [&](const auto& hero) {
        return hero.persistent_id == new_id;
    })) new_id = std::to_string(++max_id + 1);

    auto new_name = std::string{"DDSE_STAGE10_HERO"};
    std::size_t suffix = 1;
    while (std::any_of(model.heroes.begin(), model.heroes.end(), [&](const auto& hero) {
        return hero.name.value && *hero.name.value == new_name;
    })) new_name = "DDSE_STAGE10_HERO_" + std::to_string(suffix++);
    return {selected, selected_class, std::move(new_id), std::move(new_name)};
}

std::string skill_ids(const std::vector<domain::HeroSkillSelection>& skills) {
    std::string result;
    for (const auto& skill : skills) {
        if (!result.empty()) result += ", ";
        result += skill.id;
    }
    return result.empty() ? "(none stored)" : result;
}

std::vector<std::string> skill_id_vector(const std::vector<domain::HeroSkillSelection>& skills) {
    std::vector<std::string> result;
    result.reserve(skills.size());
    for (const auto& skill : skills) result.push_back(skill.id);
    return result;
}

Scenario make_resource_scenario(const application::RawSaveProfile& profile, std::int32_t value) {
    const auto& raw = source_document(profile, "persist.estate.json");
    auto estate = deep_clone(*raw.decoded);
    const auto observations = application::scan_campaign_resources(estate);
    if (!observations.wallet_found || observations.issues.size() > 0 || observations.entries.empty())
        fail("The source save does not have a fully readable resource wallet");
    std::ostringstream detail;
    detail << "Every wallet resource is set to **" << value << "**:\n\n";
    for (const auto& item : observations.entries) {
        if (!item.id || !item.amount) fail("Wallet entry lacks an id or amount at " + item.raw_object_path);
        const auto path = item.raw_object_path + "/amount";
        detail << "- `" << *item.id << "`: " << *item.amount << " → " << value << "\n";
        set_integer(estate, path, value);
    }
    const auto candidate = encode_candidate(estate, raw.id);
    core::dson::DsonReader reader;
    auto parsed = require_value(reader.parse(candidate, raw.id), "Read back resource candidate");
    const auto verified = application::read_campaign_resources(parsed);
    if (!verified || std::any_of(verified.value().begin(), verified.value().end(), [&](const auto& item) {
            return item.amount != value;
        }))
        fail("Resource candidate did not read back with the requested amount for every wallet entry");
    Scenario scenario{"01_resources_all_777", "Set all campaign resources to " + std::to_string(value),
                       detail.str(), {{raw.id, candidate}}};
    scenario.validate_readback = [value](const application::RawSaveProfile& output) {
        const auto resources = application::read_campaign_resources(*source_document(output, "persist.estate.json").decoded);
        if (!resources || resources.value().empty() || std::any_of(resources.value().begin(), resources.value().end(),
                [&](const auto& item) { return item.amount != value; }))
            fail("On-disk resource test profile does not expose the requested value for every wallet resource");
    };
    return scenario;
}

Scenario make_add_hero_scenario(const application::RawSaveProfile& profile,
                                const domain::CampaignModel& model,
                                const application::IContentEnvironment& environment,
                                const application::ModEnvironmentScanResult& scan,
                                const application::ContentDefinition& class_definition,
                                const domain::Hero& source_hero,
                                std::string new_id, std::string new_name) {
    const auto& raw = source_document(profile, "persist.roster.json");
    auto roster = deep_clone(*raw.decoded);
    const auto source_path = hero_outer_path(source_hero.persistent_id);
    const auto appended = core::dson::DsonDocumentEditor::append_clone(
        roster, "base_root/heroes", roster, source_path, new_id);
    if (!appended) fail("Could not clone a mod hero into the roster: " + appended.error().message);

    auto& inner = hero_inner_document(roster, new_id);
    set_string(inner, "base_root/actor/name", new_name);
    set_string(inner, "base_root/heroClass", *source_hero.class_id.value);
    set_integer(inner, "base_root/resolveXp", 0);
    set_integer(inner, "base_root/weapon_rank", 0);
    set_integer(inner, "base_root/armour_rank", 0);
    if (find_field_index(inner, "base_root/m_Stress")) {
        auto& stress = field_at(inner, "base_root/m_Stress");
        if (stress.kind == core::dson::ValueKind::Float) stress.replace_value(0.0F);
    }
    set_string(inner, "base_root/affliction_type_id", "");
    set_integer(inner, "base_root/affliction_severity", 0);
    set_string(inner, "base_root/virtue_type_id", "");

    while (true) {
        const auto& quirks = field_at(inner, "base_root/quirks");
        if (quirks.children.empty()) break;
        const auto child_path = inner.fields.at(quirks.children.front()).path;
        auto erased = core::dson::DsonDocumentEditor::erase(inner, child_path);
        if (!erased) fail("Could not clear cloned hero quirks: " + erased.error().message);
    }
    if (find_field_index(inner, "base_root/trinkets/items")) {
        while (true) {
            const auto& items = field_at(inner, "base_root/trinkets/items");
            if (items.children.empty()) break;
            const auto child_path = inner.fields.at(items.children.front()).path;
            auto erased = core::dson::DsonDocumentEditor::erase(inner, child_path);
            if (!erased) fail("Could not clear cloned hero equipment: " + erased.error().message);
        }
    }

    const auto candidate = encode_candidate(roster, raw.id);
    core::dson::DsonReader reader;
    auto parsed = require_value(reader.parse(candidate, raw.id), "Read back new hero candidate");
    const auto added_inner_path = hero_embedded_path(new_id);
    const auto& added_raw_data = field_at(parsed, added_inner_path);
    if (!added_raw_data.embedded_document) fail("New hero has no readable embedded DSON after write");
    const auto& added_inner = *added_raw_data.embedded_document;
    if (std::get<std::string>(field_at(added_inner, "base_root/actor/name").value) != new_name ||
        std::get<std::string>(field_at(added_inner, "base_root/heroClass").value) != *source_hero.class_id.value ||
        std::get<std::int32_t>(field_at(added_inner, "base_root/resolveXp").value) != 0 ||
        std::get<std::int32_t>(field_at(added_inner, "base_root/weapon_rank").value) != 0 ||
        std::get<std::int32_t>(field_at(added_inner, "base_root/armour_rank").value) != 0 ||
        !field_at(added_inner, "base_root/quirks").children.empty())
        fail("New hero candidate does not match the requested initial values");

    std::ostringstream detail;
    detail << "- New roster key: `" << new_id << "` (last position, " << model.heroes.size() + 1 << " of "
           << model.heroes.size() + 1 << ")\n"
           << "- Hero name: **" << new_name << "**\n"
           << "- Class: **" << localized_name(environment, class_definition)
           << "** (`" << *source_hero.class_id.value << "`)\n"
           << "- Source mod: **" << mod_display_name(scan, class_definition.provenance.source_id) << "** (`"
           << class_definition.provenance.source_id << "`)\n"
           << "- Initial combat skills: " << skill_ids(source_hero.combat_skills) << "\n"
           << "- Initial camping skills: " << skill_ids(source_hero.camping_skills) << "\n"
           << "- Level, weapon rank, armour rank: **1 / 0 / 0**; quirks/equipped trinkets: **none**\n";
    Scenario scenario{"02_add_mod_hero", "Add a level-one mod hero", detail.str(), {{raw.id, candidate}}};
    scenario.validate_readback = [&environment, new_id, new_name,
                                  expected_class = *source_hero.class_id.value,
                                  expected_class_source = class_definition.provenance.source_id,
                                  expected_combat = skill_id_vector(source_hero.combat_skills),
                                  expected_camping = skill_id_vector(source_hero.camping_skills)](
                                      const application::RawSaveProfile& output) {
        const auto projected = application::CampaignModelBuilder{}.build(output, environment);
        if (projected.heroes.empty() || projected.heroes.back().persistent_id != new_id ||
            projected.heroes.back().name.value.value_or("") != new_name ||
            !projected.heroes.back().class_id.value || *projected.heroes.back().class_id.value != expected_class ||
            projected.heroes.back().definition.layer_type != "mod" ||
            projected.heroes.back().definition.source_id != expected_class_source ||
            projected.heroes.back().resolve_xp.value.value_or(-1) != 0 ||
            projected.heroes.back().weapon_rank.value.value_or(-1) != 0 ||
            projected.heroes.back().armour_rank.value.value_or(-1) != 0 ||
            !projected.heroes.back().quirks.empty() || !projected.heroes.back().trinkets.empty() ||
            skill_id_vector(projected.heroes.back().combat_skills) != expected_combat ||
            skill_id_vector(projected.heroes.back().camping_skills) != expected_camping)
            fail("On-disk mod hero did not retain its class/initial skills and requested level-one state");
    };
    return scenario;
}

struct QuirkReplacementChoice {
    const domain::Hero* hero{};
    const domain::HeroQuirk* old_positive{};
    const domain::HeroQuirk* old_negative{};
    const application::ContentDefinition* new_positive{};
    const application::ContentDefinition* new_negative{};
};

QuirkReplacementChoice choose_quirk_replacements(
    const domain::CampaignModel& model,
    const std::vector<application::ContentDefinition>& definitions) {
    for (const auto& hero : model.heroes) {
        const auto old_positive = std::find_if(hero.quirks.begin(), hero.quirks.end(), [](const auto& quirk) {
            return !quirk.is_disease && quirk.polarity == domain::QuirkPolarity::Positive &&
                   !quirk.is_locked.value.value_or(false);
        });
        const auto old_negative = std::find_if(hero.quirks.begin(), hero.quirks.end(), [](const auto& quirk) {
            return !quirk.is_disease && quirk.polarity == domain::QuirkPolarity::Negative &&
                   !quirk.is_locked.value.value_or(false);
        });
        if (old_positive == hero.quirks.end() || old_negative == hero.quirks.end()) continue;
        const auto existing = hero_quirk_ids(hero);
        const auto* new_positive = find_mod_quirk(definitions, true, existing);
        const auto* new_negative = find_mod_quirk(definitions, false, existing);
        if (!new_positive || !new_negative || new_positive->id == new_negative->id) continue;
        return {&hero, &*old_positive, &*old_negative, new_positive, new_negative};
    }
    fail("No early roster hero has replaceable positive and negative quirks plus unused mod quirk definitions");
}

Scenario make_replace_quirks_scenario(const application::RawSaveProfile& profile,
                                      const application::ModEnvironmentScanResult& scan,
                                      const application::IContentEnvironment& environment,
                                      const QuirkReplacementChoice& choice) {
    const auto& raw = source_document(profile, "persist.roster.json");
    auto roster = deep_clone(*raw.decoded);
    auto& inner = hero_inner_document(roster, choice.hero->persistent_id);
    const auto old_positive_id = choice.old_positive->id;
    const auto old_negative_id = choice.old_negative->id;
    auto renamed_positive = core::dson::DsonDocumentEditor::rename(
        inner, quirk_inner_path(old_positive_id), choice.new_positive->id);
    if (!renamed_positive) fail("Could not replace positive quirk: " + renamed_positive.error().message);
    auto renamed_negative = core::dson::DsonDocumentEditor::rename(
        inner, quirk_inner_path(old_negative_id), choice.new_negative->id);
    if (!renamed_negative) fail("Could not replace negative quirk: " + renamed_negative.error().message);
    initialize_quirk(inner, choice.new_positive->id);
    initialize_quirk(inner, choice.new_negative->id);

    const auto candidate = encode_candidate(roster, raw.id);
    core::dson::DsonReader reader;
    auto parsed = require_value(reader.parse(candidate, raw.id), "Read back replacement quirks candidate");
    const auto& parsed_inner = *field_at(parsed, hero_embedded_path(choice.hero->persistent_id)).embedded_document;
    (void)field_at(parsed_inner, quirk_inner_path(choice.new_positive->id));
    (void)field_at(parsed_inner, quirk_inner_path(choice.new_negative->id));
    if (find_field_index(parsed_inner, quirk_inner_path(old_positive_id)) ||
        find_field_index(parsed_inner, quirk_inner_path(old_negative_id)))
        fail("Replacement candidate still contains one of the old quirk identifiers");

    std::ostringstream detail;
    detail << "- Hero: **" << hero_display(*choice.hero) << "** — " << class_display(*choice.hero)
           << " (roster position " << choice.hero->roster_position + 1 << ", key `" << choice.hero->persistent_id << "`)\n"
           << "- Positive: **" << (choice.old_positive->definition.display_name.empty()
                    ? old_positive_id : choice.old_positive->definition.display_name)
           << "** (`" << old_positive_id << "`) → **" << localized_name(environment, *choice.new_positive)
           << "** (`" << choice.new_positive->id << "`), mod **"
           << mod_display_name(scan, choice.new_positive->provenance.source_id) << "** (`"
           << choice.new_positive->provenance.source_id << "`)\n"
           << "- Negative: **" << (choice.old_negative->definition.display_name.empty()
                    ? old_negative_id : choice.old_negative->definition.display_name)
           << "** (`" << old_negative_id << "`) → **" << localized_name(environment, *choice.new_negative)
           << "** (`" << choice.new_negative->id << "`), mod **"
           << mod_display_name(scan, choice.new_negative->provenance.source_id) << "** (`"
           << choice.new_negative->provenance.source_id << "`)\n";
    Scenario scenario{"03_replace_mod_quirks_first_eligible_hero", "Replace one positive and one negative mod quirk",
                      detail.str(), {{raw.id, candidate}}};
    scenario.validate_readback = [&environment,
                                  hero_id = choice.hero->persistent_id,
                                  old_positive_id, old_negative_id,
                                  new_positive_id = choice.new_positive->id,
                                  new_negative_id = choice.new_negative->id,
                                  expected_quirks = choice.hero->quirks.size()](
                                      const application::RawSaveProfile& output) {
        const auto projected = application::CampaignModelBuilder{}.build(output, environment);
        const auto hero = std::find_if(projected.heroes.begin(), projected.heroes.end(), [&](const auto& value) {
            return value.persistent_id == hero_id;
        });
        if (hero == projected.heroes.end() || hero->quirks.size() != expected_quirks)
            fail("On-disk quirk replacement profile changed the hero's quirk count unexpectedly");
        const auto has_quirk = [&](std::string_view id, domain::QuirkPolarity polarity, bool must_exist) {
            const auto found = std::find_if(hero->quirks.begin(), hero->quirks.end(), [&](const auto& quirk) {
                return !quirk.is_disease && quirk.id == id;
            });
            if ((found != hero->quirks.end()) != must_exist) return false;
            return !must_exist || (found->polarity == polarity && found->definition.layer_type == "mod");
        };
        if (!has_quirk(old_positive_id, domain::QuirkPolarity::Positive, false) ||
            !has_quirk(old_negative_id, domain::QuirkPolarity::Negative, false) ||
            !has_quirk(new_positive_id, domain::QuirkPolarity::Positive, true) ||
            !has_quirk(new_negative_id, domain::QuirkPolarity::Negative, true))
            fail("On-disk quirk replacement profile failed semantic polarity or mod-provenance validation");
    };
    return scenario;
}

struct QuirkAdditionChoice {
    const domain::Hero* hero{};
    const domain::HeroQuirk* template_quirk{};
    const application::ContentDefinition* positive{};
    const application::ContentDefinition* negative{};
    std::size_t current_positive{};
    std::size_t current_negative{};
};

QuirkAdditionChoice choose_quirks_to_add(
    const domain::CampaignModel& model,
    const std::vector<application::ContentDefinition>& definitions) {
    for (const auto& hero : model.heroes) {
        std::size_t positives = 0;
        std::size_t negatives = 0;
        const domain::HeroQuirk* template_quirk = nullptr;
        for (const auto& quirk : hero.quirks) {
            if (quirk.is_disease) continue;
            if (!template_quirk && quirk.state != domain::EntityState::Invalid) template_quirk = &quirk;
            if (quirk.polarity == domain::QuirkPolarity::Positive) ++positives;
            if (quirk.polarity == domain::QuirkPolarity::Negative) ++negatives;
        }
        if (positives >= 10 || negatives >= 10 || template_quirk == nullptr) continue;
        const auto existing = hero_quirk_ids(hero);
        const auto* positive = find_mod_quirk(definitions, true, existing);
        const auto* negative = find_mod_quirk(definitions, false, existing);
        if (!positive || !negative || positive->id == negative->id) continue;
        return {&hero, template_quirk, positive, negative, positives, negatives};
    }
    fail("No early roster hero is below the 10 positive / 10 negative quirk limits with a usable quirk template");
}

Scenario make_add_quirks_scenario(const application::RawSaveProfile& profile,
                                  const application::ModEnvironmentScanResult& scan,
                                  const application::IContentEnvironment& environment,
                                  const QuirkAdditionChoice& choice) {
    const auto& raw = source_document(profile, "persist.roster.json");
    auto roster = deep_clone(*raw.decoded);
    auto& inner = hero_inner_document(roster, choice.hero->persistent_id);
    const auto parent_path = std::string{"base_root/quirks"};
    const auto template_path = quirk_inner_path(choice.template_quirk->id);
    auto positive = core::dson::DsonDocumentEditor::append_clone(
        inner, parent_path, inner, template_path, choice.positive->id);
    if (!positive) fail("Could not add positive mod quirk: " + positive.error().message);
    initialize_quirk(inner, choice.positive->id);
    auto negative = core::dson::DsonDocumentEditor::append_clone(
        inner, parent_path, inner, template_path, choice.negative->id);
    if (!negative) fail("Could not add negative mod quirk: " + negative.error().message);
    initialize_quirk(inner, choice.negative->id);

    const auto candidate = encode_candidate(roster, raw.id);
    core::dson::DsonReader reader;
    auto parsed = require_value(reader.parse(candidate, raw.id), "Read back added quirks candidate");
    const auto& parsed_inner = *field_at(parsed, hero_embedded_path(choice.hero->persistent_id)).embedded_document;
    (void)field_at(parsed_inner, quirk_inner_path(choice.positive->id));
    (void)field_at(parsed_inner, quirk_inner_path(choice.negative->id));
    if (field_at(parsed_inner, parent_path).children.size() != choice.hero->quirks.size() + 2)
        fail("Added quirk candidate has an unexpected quirk count");

    std::ostringstream detail;
    detail << "- Hero: **" << hero_display(*choice.hero) << "** — " << class_display(*choice.hero)
           << " (roster position " << choice.hero->roster_position + 1 << ", key `" << choice.hero->persistent_id << "`)\n"
           << "- Before: " << choice.current_positive << " positive / " << choice.current_negative << " negative\n"
           << "- Added positive: **" << localized_name(environment, *choice.positive) << "** (`" << choice.positive->id
           << "`), mod **" << mod_display_name(scan, choice.positive->provenance.source_id) << "** (`"
           << choice.positive->provenance.source_id << "`)\n"
           << "- Added negative: **" << localized_name(environment, *choice.negative) << "** (`" << choice.negative->id
           << "`), mod **" << mod_display_name(scan, choice.negative->provenance.source_id) << "** (`"
           << choice.negative->provenance.source_id << "`)\n";
    Scenario scenario{"04_add_mod_quirks_near_front", "Add one positive and one negative mod quirk",
                      detail.str(), {{raw.id, candidate}}};
    scenario.validate_readback = [&environment,
                                  hero_id = choice.hero->persistent_id,
                                  positive_id = choice.positive->id,
                                  negative_id = choice.negative->id,
                                  expected_quirks = choice.hero->quirks.size() + 2](
                                      const application::RawSaveProfile& output) {
        const auto projected = application::CampaignModelBuilder{}.build(output, environment);
        const auto hero = std::find_if(projected.heroes.begin(), projected.heroes.end(), [&](const auto& value) {
            return value.persistent_id == hero_id;
        });
        if (hero == projected.heroes.end() || hero->quirks.size() != expected_quirks)
            fail("On-disk quirk addition profile failed its final quirk count check");
        const auto has_quirk = [&](std::string_view id, domain::QuirkPolarity polarity) {
            const auto found = std::find_if(hero->quirks.begin(), hero->quirks.end(), [&](const auto& quirk) {
                return !quirk.is_disease && quirk.id == id;
            });
            return found != hero->quirks.end() && found->polarity == polarity &&
                   found->definition.layer_type == "mod";
        };
        if (!has_quirk(positive_id, domain::QuirkPolarity::Positive) ||
            !has_quirk(negative_id, domain::QuirkPolarity::Negative))
            fail("On-disk quirk addition profile failed semantic polarity or mod-provenance validation");
    };
    return scenario;
}

Scenario make_add_trinket_scenario(const application::RawSaveProfile& profile,
                                   const domain::CampaignModel& model,
                                   const application::ModEnvironmentScanResult& scan,
                                   const application::IContentEnvironment& environment,
                                   const std::vector<application::ContentDefinition>& definitions) {
    const auto& raw = source_document(profile, "persist.estate.json");
    auto estate = deep_clone(*raw.decoded);
    std::set<std::string, std::less<>> inventory_ids;
    std::size_t max_index = 0;
    std::string template_index;
    for (const auto& entry : model.trinket_inventory) {
        if (entry.id.value) inventory_ids.insert(*entry.id.value);
        const auto numeric = numeric_child_index(entry.raw_key);
        if (numeric != std::numeric_limits<std::size_t>::max() &&
            (template_index.empty() || numeric >= max_index)) {
            max_index = numeric;
            template_index = entry.raw_key;
        }
    }
    if (template_index.empty()) fail("Cannot add a trinket because the save has no inventory template item");

    const auto selected = std::find_if(definitions.begin(), definitions.end(), [&](const auto& definition) {
        return definition.type == "trinket" && definition.provenance.layer_type == "mod" &&
               definition.provenance.selected && !inventory_ids.contains(definition.id);
    });
    if (selected == definitions.end()) fail("No enabled mod trinket is available outside the current inventory");
    if (max_index >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        fail("Trinket inventory index has reached the safe test range");
    const auto new_index = std::to_string(max_index + 1);
    const auto source_item_path = "base_root/trinkets/items/" + template_index;
    auto appended = core::dson::DsonDocumentEditor::append_clone(
        estate, "base_root/trinkets/items", estate, source_item_path, new_index);
    if (!appended) fail("Could not append mod trinket to inventory: " + appended.error().message);
    const auto item_path = "base_root/trinkets/items/" + new_index;
    set_string(estate, item_path + "/id", selected->id);
    set_integer(estate, item_path + "/amount", 1);

    const auto candidate = encode_candidate(estate, raw.id);
    core::dson::DsonReader reader;
    auto parsed = require_value(reader.parse(candidate, raw.id), "Read back added trinket candidate");
    if (std::get<std::string>(field_at(parsed, item_path + "/id").value) != selected->id ||
        std::get<std::int32_t>(field_at(parsed, item_path + "/amount").value) != 1)
        fail("Added trinket candidate failed its inventory id/count check");

    std::ostringstream detail;
    detail << "- Inventory: appended at position " << max_index + 2 << " (raw key `" << new_index << "`)\n"
           << "- Trinket: **" << localized_name(environment, *selected) << "** (`" << selected->id << "`) × 1\n"
           << "- Source mod: **" << mod_display_name(scan, selected->provenance.source_id) << "** (`"
           << selected->provenance.source_id << "`)\n";
    Scenario scenario{"05_add_mod_trinket", "Add a mod trinket to the estate inventory", detail.str(), {{raw.id, candidate}}};
    scenario.validate_readback = [&environment,
                                  trinket_id = selected->id, new_index](
                                     const application::RawSaveProfile& output) {
        const auto projected = application::CampaignModelBuilder{}.build(output, environment);
        const auto inventory_item = std::find_if(projected.trinket_inventory.begin(), projected.trinket_inventory.end(),
            [&](const auto& item) { return item.raw_key == new_index; });
        if (inventory_item == projected.trinket_inventory.end() || inventory_item->id.value.value_or("") != trinket_id ||
            inventory_item->amount.value.value_or(-1) != 1 || inventory_item->definition.layer_type != "mod")
            fail("On-disk trinket inventory test profile failed its id/count check");
    };
    return scenario;
}

std::string read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) fail("Cannot read file: " + path.string());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void copy_profile_tree(const std::filesystem::path& source, const std::filesystem::path& target) {
    if (!std::filesystem::is_directory(source)) fail("Source profile is not a directory: " + source.string());
    if (std::filesystem::exists(target)) fail("Refusing to overwrite generated profile: " + target.string());
    std::filesystem::create_directories(target);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
        const auto relative = entry.path().lexically_relative(source);
        const auto destination = target / relative;
        if (entry.is_symlink()) fail("Save profile contains a symlink; refusing ambiguous copy: " + entry.path().string());
        if (entry.is_directory()) {
            std::filesystem::create_directories(destination);
        } else if (entry.is_regular_file()) {
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::copy_file(entry.path(), destination, std::filesystem::copy_options::none);
        } else {
            fail("Save profile contains an unsupported filesystem entry: " + entry.path().string());
        }
    }
}

struct TreeListing {
    std::set<std::string, std::less<>> files;
    std::set<std::string, std::less<>> directories;
};

TreeListing list_tree(const std::filesystem::path& root) {
    TreeListing result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto relative = entry.path().lexically_relative(root).generic_string();
        if (entry.is_symlink()) fail("Generated profile contains an unexpected symlink: " + entry.path().string());
        if (entry.is_directory()) result.directories.insert(relative);
        else if (entry.is_regular_file()) result.files.insert(relative);
        else fail("Generated profile contains an unsupported filesystem entry: " + entry.path().string());
    }
    return result;
}

void verify_profile_copy(const std::filesystem::path& source, const std::filesystem::path& target,
                         const std::set<std::string, std::less<>>& changed_files) {
    const auto source_tree = list_tree(source);
    const auto target_tree = list_tree(target);
    if (source_tree.files != target_tree.files || source_tree.directories != target_tree.directories)
        fail("Generated profile changed the source directory layout");
    for (const auto& relative : source_tree.files) {
        if (changed_files.contains(relative)) continue;
        if (read_file_bytes(source / relative) != read_file_bytes(target / relative))
            fail("Generated profile changed an unrelated file: " + relative);
    }
}

void write_scenario_profile(application::IFileSystem& file_system,
                            application::SaveProfileDiscovery& discovery,
                            const application::RawSaveProfile& source_profile,
                            const std::filesystem::path& output_root,
                            const Scenario& scenario) {
    const auto destination = output_root / scenario.directory / source_profile.descriptor.id;
    copy_profile_tree(source_profile.descriptor.root_path, destination);
    std::set<std::string, std::less<>> changed_names;
    for (const auto& [id, bytes] : scenario.changed_documents) {
        const auto& source = source_document(source_profile, id);
        const auto name = source.path.filename().generic_string();
        changed_names.insert(name);
        require_success(file_system.write_file_atomic(destination / source.path.filename(), to_string(bytes)),
                       "Atomically write candidate " + name);
    }
    verify_profile_copy(source_profile.descriptor.root_path, destination, changed_names);
    const auto readback = require_value(discovery.load(destination), "Reopen generated profile " + scenario.directory);
    if (readback.status != application::ProfileReadStatus::Complete)
        fail("Generated profile is not complete: " + scenario.directory);
    const auto source_game = source_document(source_profile, "persist.game.json").bytes;
    if (source_document(readback, "persist.game.json").bytes != source_game)
        fail("Generated profile changed its saved mod order: " + scenario.directory);
    if (scenario.validate_readback) scenario.validate_readback(readback);
}

std::string format_fingerprint(std::uint64_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

std::string make_readme(const Options& options,
                        const application::RawSaveProfile& source_profile,
                        const application::ModEnvironmentScanResult& mod_scan,
                        const std::vector<Scenario>& scenarios,
                        std::uint64_t backup_fingerprint) {
    std::ostringstream output;
    output << "# Stage 10 实际修改测试存档\n\n"
           << "这些 profile 均从源存档的只读快照生成。原始 profile 没有被改写。每次游戏检查只使用一个测试 profile。\n\n"
           << "- 源 profile：`" << absolute_normal(options.source_profile).string() << "`\n"
           << "- 源指纹：`" << format_fingerprint(source_profile.baseline_fingerprint) << "`\n"
           << "- 单独备份 profile 指纹：`" << format_fingerprint(backup_fingerprint) << "`\n"
           << "- 对照组：`00_control/" << source_profile.descriptor.id << "`（与源 profile 文件树一致）\n"
           << "- Mod 顺序来源：`persist.game.json / applied_ugcs_1_0`，启用 "
           << mod_scan.effective_order.size() << " 个；没有读取 `Default.json`。\n"
           << "- 基础游戏内容只读扫描：`" << absolute_normal(options.game_root).string() << "`\n"
           << "- 已安装 mod：" << mod_scan.mods.size() << "；内容解析诊断：" << mod_scan.diagnostics.size() << "。\n\n"
           << "## 游戏内确认清单\n\n"
           << "1. 先检查 `00_control` 能正常进入城镇，作为原始状态对照。\n"
           << "2. 每次退出游戏后，只换入一个下方 profile；不要同时混用多个测试目录。\n"
           << "3. 确认 profile 能载入、英雄名单/仓库可打开、没有损坏存档提示或异常缺失内容。\n"
           << "4. 检查每项指定变更是否出现，离开对应界面后重新打开确认仍存在。\n"
           << "5. 如要确认重启持久性，先把正在测试的 profile 复制到另一个临时目录，再在游戏内保存并重启检查。\n"
           << "6. 任何无法载入或出现副作用的 case，退出游戏后恢复你自己的 profile 备份；不要覆盖 `00_control`。\n\n"
           << "## 测试 profile\n\n";
    for (const auto& scenario : scenarios) {
        output << "### `" << scenario.directory << "/" << source_profile.descriptor.id << "` — "
               << scenario.title << "\n\n" << scenario.details << "\n"
               << "**请确认：** profile 能载入；以上指定项目显示正确；其他相关字段没有意外改变；保存后重新载入仍正确。\n\n";
    }
    output << "## Mod 顺序快照\n\n"
           << "以下启用顺序直接来自源 profile。所有测试都保留原始 `persist.game.json` 字节。\n\n";
    for (const auto& entry : mod_scan.save_order) {
        const auto id = entry.matched_mod_id.empty() ? entry.identity : entry.matched_mod_id;
        output << entry.position + 1 << ". `" << entry.identity << "` — "
               << mod_display_name(mod_scan, id) << "\n";
    }
    output << "\n## 运行生成器\n\n"
           << "从仓库根目录运行：\n\n"
           << "```powershell\n.\\scripts\\generate_game_test_saves.ps1\n```\n\n"
           << "生成器默认拒绝覆盖已存在的输出目录。需要重新生成时，先自行移走或重命名 `"
           << absolute_normal(options.output_root).string() << "`。\n";
    return output.str();
}

class TemporaryDatabaseWorkspace {
public:
    TemporaryDatabaseWorkspace() : root_(mod_temp_root()) {
        if (std::filesystem::exists(root_)) fail("Temporary database workspace unexpectedly exists: " + root_.string());
        std::filesystem::create_directories(root_);
        created_ = true;
    }
    ~TemporaryDatabaseWorkspace() {
        if (!created_) return;
        const auto temp = std::filesystem::temp_directory_path().lexically_normal();
        const auto root = root_.lexically_normal();
        const auto name = root.filename().string();
        if (root.parent_path() == temp && name.starts_with("ddse-stage10-game-test-")) {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
private:
    std::filesystem::path root_;
    bool created_{};
};

void check_paths(const Options& options) {
    for (const auto* path : {&options.source_profile, &options.game_root,
                             &options.workshop_root, &options.local_mod_root})
        if (!std::filesystem::is_directory(*path)) fail("Required read-only input directory does not exist: " + path->string());
    if (std::filesystem::exists(options.output_root))
        fail("Output already exists; refusing to overwrite it: " + options.output_root.string());
    const auto output = absolute_normal(options.output_root);
    for (const auto* input : {&options.source_profile, &options.game_root,
                              &options.workshop_root, &options.local_mod_root}) {
        if (is_within_or_equal(output, *input) || is_within_or_equal(*input, output))
            fail("Output and read-only input paths must be disjoint: " + output.string() + " / " + input->string());
    }
}

void run_generator(const Options& options) {
    check_paths(options);
    infrastructure::NativeFileSystem file_system;
    application::SaveProfileDiscovery discovery{file_system};
    auto profile = require_value(discovery.load(options.source_profile), "Load source profile");
    if (profile.status != application::ProfileReadStatus::Complete)
        fail("Source profile is partial/read-only; refusing to generate game test profiles");
    const auto source_fingerprint = require_value(
        application::SaveProfileDiscovery::fingerprint_profile(file_system, options.source_profile),
        "Fingerprint source profile");
    if (source_fingerprint != profile.baseline_fingerprint) fail("Source profile changed during generator startup");

    const auto backup_profile = options.source_profile.parent_path() / "backup" / options.source_profile.filename();
    std::uint64_t backup_fingerprint = 0;
    if (std::filesystem::is_directory(backup_profile)) {
        auto backup_hash = application::SaveProfileDiscovery::fingerprint_profile(file_system, backup_profile);
        if (backup_hash) backup_fingerprint = backup_hash.value();
    }

    TemporaryDatabaseWorkspace databases;
    const auto base_database = databases.root() / "base_content.db";
    const auto mod_database = databases.root() / "mod_environment.db";

    std::cout << "Scanning read-only base game and DLC content...\n" << std::flush;
    application::BaseContentScanner base_scanner{file_system};
    auto base_scan = require_value(base_scanner.scan(
        application::BaseContentScanConfig::defaults(options.game_root)), "Scan base game content");
    auto base_summary = require_value(infrastructure::BaseContentDatabaseBuilder{}.rebuild(base_database, base_scan),
                                      "Build temporary base content database");
    std::cout << "Scanning enabled mods in source-save order...\n" << std::flush;
    application::ModEnvironmentScanConfig mod_config;
    mod_config.workshop_root = options.workshop_root;
    mod_config.local_mod_roots = {options.local_mod_root};
    mod_config.save_profile_root = options.source_profile;
    mod_config.prefer_manager_export = false;
    mod_config.base_content_database = base_database;
    application::ModEnvironmentScanner mod_scanner{file_system};
    auto mod_scan = require_value(mod_scanner.scan(mod_config), "Scan mod environment from source save");
    if (mod_scan.effective_order_source != "save_profile" || !mod_scan.comparison.save_available ||
        mod_scan.comparison.manager_export_available)
        fail("Mod environment did not use only the source save's enabled mod order");
    if (mod_scan.effective_order.size() != mod_scan.save_order.size())
        fail("Some enabled mods from the source save are not installed/matched; refusing to guess: enabled in save=" +
             std::to_string(mod_scan.save_order.size()) + ", resolved=" + std::to_string(mod_scan.effective_order.size()));
    auto mod_summary = require_value(infrastructure::ModEnvironmentDatabaseBuilder{}.rebuild(
        mod_database, base_database, mod_scan), "Build temporary save-ordered mod environment");
    std::cout << "Enabled mods from save: " << mod_scan.effective_order.size()
              << " (installed " << mod_scan.mods.size() << ", parser diagnostics " << mod_scan.diagnostics.size() << ")\n";

    application::ContentEnvironmentSelection selection;
    selection.language = "english";
    selection.fallback_language = "english";
    infrastructure::SqliteContentEnvironment environment({base_database, mod_database, selection});
    const auto model = application::CampaignModelBuilder{}.build(profile, environment);
    if (model.state == domain::ModelState::Invalid || model.heroes.empty())
        fail("Source save did not project a usable campaign roster");

    const auto classes = content_list(environment, "hero_class");
    const auto quirks = content_list(environment, "quirk");
    const auto trinkets = content_list(environment, "trinket");
    const auto add_hero = choose_hero_to_add(model, classes);
    const auto replace_quirks = choose_quirk_replacements(model, quirks);
    const auto add_quirks = choose_quirks_to_add(model, quirks);

    constexpr std::int32_t resource_value = 777;
    std::vector<Scenario> scenarios;
    scenarios.push_back(make_resource_scenario(profile, resource_value));
    scenarios.push_back(make_add_hero_scenario(profile, model, environment, mod_scan, *add_hero.class_definition,
        *add_hero.source_hero, add_hero.new_hero_id, add_hero.new_name));
    scenarios.push_back(make_replace_quirks_scenario(profile, mod_scan, environment, replace_quirks));
    scenarios.push_back(make_add_quirks_scenario(profile, mod_scan, environment, add_quirks));
    scenarios.push_back(make_add_trinket_scenario(profile, model, mod_scan, environment, trinkets));

    auto output = absolute_normal(options.output_root);
    std::filesystem::create_directories(output.parent_path());
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto staging = output;
    staging += ".building-" + std::to_string(stamp);
    if (std::filesystem::exists(staging)) fail("Generation staging path already exists: " + staging.string());
    std::filesystem::create_directories(staging);
    copy_profile_tree(options.source_profile, staging / "00_control" / profile.descriptor.id);
    const auto control_hash = require_value(application::SaveProfileDiscovery::fingerprint_profile(
        file_system, staging / "00_control" / profile.descriptor.id), "Verify control profile copy");
    if (control_hash != profile.baseline_fingerprint) fail("Control profile differs from source profile bytes");

    for (const auto& scenario : scenarios) {
        std::cout << "Generating " << scenario.directory << "...\n" << std::flush;
        write_scenario_profile(file_system, discovery, profile, staging, scenario);
    }
    auto final_source_check = profile.matches_disk_baseline(file_system);
    if (!final_source_check || !final_source_check.value()) fail("Source profile changed during generation; outputs were not published");
    const auto backup_matches = backup_fingerprint == 0 ? "unavailable" :
        (backup_fingerprint == profile.baseline_fingerprint ? "identical" : "different");
    std::string readme = make_readme(options, profile, mod_scan, scenarios, backup_fingerprint);
    const auto base_definition_count = base_summary.hero_classes + base_summary.skills + base_summary.trinkets +
                                      base_summary.quirks + base_summary.diseases + base_summary.resources +
                                      base_summary.buildings;
    readme += "\n- 单独备份对照结果：`" + std::string{backup_matches} + "`。\n"
              "- 临时基础内容库统计：" + std::to_string(base_definition_count) + " definitions / " +
              std::to_string(base_summary.assets) + " assets。\n"
              "- 临时有效 mod 内容库统计：" + std::to_string(mod_summary.definitions) + " definitions / " +
              std::to_string(mod_summary.assets) + " assets。\n";
    require_success(file_system.write_file_atomic(staging / "README.md", readme), "Write test save checklist");
    if (std::filesystem::exists(output)) fail("Output appeared while generation was running; refusing to replace it");
    std::filesystem::rename(staging, output);
    std::cout << "\nGenerated five independent game test profiles and one control profile at:\n"
              << output.string() << "\nChecklist: " << (output / "README.md").string() << "\n";
}

int run(const std::vector<std::string>& arguments) {
    try {
        run_generator(parse_options(arguments));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Game test save generation failed: " << error.what() << '\n';
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int main() {
    int wide_argc = 0;
    auto wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (!wide_argv) {
        std::cerr << "Unable to read Unicode command-line arguments\n";
        return 2;
    }
    std::vector<std::string> arguments;
    arguments.reserve(wide_argc > 1 ? static_cast<std::size_t>(wide_argc - 1) : 0);
    for (int index = 1; index < wide_argc; ++index) {
        const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[index], -1,
                                                  nullptr, 0, nullptr, nullptr);
        if (required <= 0) {
            LocalFree(wide_argv);
            std::cerr << "Unable to convert a command-line argument to UTF-8\n";
            return 2;
        }
        std::string converted(static_cast<std::size_t>(required), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[index], -1,
                                converted.data(), required, nullptr, nullptr) <= 0) {
            LocalFree(wide_argv);
            std::cerr << "Unable to convert a command-line argument to UTF-8\n";
            return 2;
        }
        converted.resize(static_cast<std::size_t>(required - 1));
        arguments.push_back(std::move(converted));
    }
    LocalFree(wide_argv);
    return run(arguments);
}
#else
int main(int argc, char* argv[]) {
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
    return run(arguments);
}
#endif
