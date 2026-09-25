#include "ddse/application/hero_template.hpp"

#include "ddse/application/hero_template_resource.hpp"
#include "ddse/core/dson/dson_document_editor.hpp"
#include "ddse/core/dson/dson_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ddse::application {
namespace {

core::Error template_error(std::string message, std::string path = {}) {
    core::Error error{core::ErrorCode::ValidationFailed, std::move(message), "HeroFactory"};
    if (!path.empty()) error.context.emplace("path", std::move(path));
    return error;
}

std::optional<std::size_t> field_index(const core::dson::DsonDocument& document,
                                       std::string_view path) {
    for (std::size_t index = 0; index < document.fields.size(); ++index)
        if (document.fields[index].path == path) return index;
    return std::nullopt;
}

template <typename T>
bool replace_value(core::dson::DsonDocument& document, std::string_view path, T value) {
    const auto index = field_index(document, path);
    if (!index) return false;
    document.fields[*index].replace_value(core::dson::Value{std::move(value)});
    return true;
}

bool replace_skill_map(core::dson::DsonDocument& document, std::string_view path,
                       const std::vector<std::string>& skill_ids) {
    const auto parent_index = field_index(document, path);
    if (!parent_index || document.fields[*parent_index].kind != core::dson::ValueKind::Object) return false;
    std::vector<std::string> old_children;
    for (const auto child : document.fields[*parent_index].children) {
        if (child >= document.fields.size()) return false;
        old_children.push_back(document.fields[child].path);
    }
    for (const auto& child_path : old_children) {
        auto erased = core::dson::DsonDocumentEditor::erase(document, child_path);
        if (!erased) return false;
    }
    for (const auto& id : skill_ids) {
        if (id.empty() || id.find('/') != std::string::npos) return false;
        auto appended = core::dson::DsonDocumentEditor::append_value(
            document, path, id, std::int32_t{0});
        if (!appended) return false;
    }
    return true;
}

} // namespace

core::Result<std::shared_ptr<core::dson::DsonDocument>, core::Error>
build_blank_level_zero_hero_template(std::string_view hero_class,
                                    std::string_view hero_name,
                                    const std::vector<std::string>& combat_skills,
                                    const std::vector<std::string>& camping_skills,
                                    float base_hit_points) {
    if (hero_class.empty() || hero_class.find('/') != std::string_view::npos || hero_name.empty() ||
        combat_skills.empty() || !(base_hit_points > 0.0F))
        return core::Result<std::shared_ptr<core::dson::DsonDocument>, core::Error>::failure(
            template_error("The class or its verified resolve-level-zero starter data is incomplete"));

    const auto* bytes = reinterpret_cast<const std::byte*>(kBlankLevelZeroHeroTemplateBytes.data());
    const auto parsed = core::dson::DsonReader{}.parse(
        std::span<const std::byte>{bytes, kBlankLevelZeroHeroTemplateBytes.size()},
        "builtin:hero-level-zero-blank-v1");
    if (!parsed)
        return core::Result<std::shared_ptr<core::dson::DsonDocument>, core::Error>::failure(parsed.error());
    auto result = std::make_shared<core::dson::DsonDocument>(parsed.value());

    const auto outer_index = field_index(*result, "base_root/heroes/1");
    if (!outer_index || result->fields[*outer_index].kind != core::dson::ValueKind::Object)
        return core::Result<std::shared_ptr<core::dson::DsonDocument>, core::Error>::failure(
            template_error("Built-in template has no hero roster object", "base_root/heroes/1"));
    std::shared_ptr<core::dson::DsonDocument> inner;
    for (const auto child : result->fields[*outer_index].children) {
        if (child >= result->fields.size()) continue;
        for (const auto nested : result->fields[child].children) {
            if (nested < result->fields.size() && result->fields[nested].embedded_document)
                inner = result->fields[nested].embedded_document;
        }
    }
    if (!inner)
        return core::Result<std::shared_ptr<core::dson::DsonDocument>, core::Error>::failure(
            template_error("Built-in template has no embedded hero DSON"));

    const auto set = [&](std::string_view path, auto value) {
        return replace_value(*inner, path, std::move(value));
    };
    const bool initialized =
        set("base_root/actor/name", std::string{hero_name}) &&
        set("base_root/actor/current_hp", base_hit_points) &&
        set("base_root/heroClass", std::string{hero_class}) &&
        set("base_root/resolveXp", std::int32_t{0}) &&
        set("base_root/m_Stress", 0.0F) &&
        set("base_root/weapon_rank", std::int32_t{0}) &&
        set("base_root/armour_rank", std::int32_t{0}) &&
        set("base_root/affliction_type_id", std::string{}) &&
        set("base_root/affliction_severity", std::int32_t{0}) &&
        set("base_root/virtue_type_id", std::string{}) &&
        // The embedded Crusader came from the game's starting expedition.
        // A hero appended to an existing hamlet must be in the town roster.
        set("base_root/roster.status", std::int32_t{0}) &&
        set("base_root/roster.before_on_start_town_visit_status", std::int32_t{0}) &&
        set("base_root/roster.missing_duration", std::int32_t{0}) &&
        set("base_root/roster.story_variation", std::int32_t{0}) &&
        set("base_root/roster.missing_from", std::int32_t{0}) &&
        set("base_root/roster.timestamp", std::int32_t{0}) &&
        set("base_root/is_death_heart_attack_completed", false) &&
        set("base_root/visited_deaths_door", false) &&
        set("base_root/deaths_door_enter_effect_round_cooldown", std::int32_t{0}) &&
        set("base_root/has_had_heart_attack", false) &&
        set("base_root/steps_taken", std::int32_t{0}) &&
        set("base_root/enemies_killed", std::int32_t{0}) &&
        set("base_root/provisions_consumed", std::int32_t{0}) &&
        set("base_root/number_of_successful_darkest_dungeon_quests", std::int32_t{0});
    if (!initialized ||
        !replace_skill_map(*inner, "base_root/skills/selected_combat_skills", combat_skills) ||
        !replace_skill_map(*inner, "base_root/skills/selected_camping_skills", camping_skills))
        return core::Result<std::shared_ptr<core::dson::DsonDocument>, core::Error>::failure(
            template_error("Built-in template could not be initialized from the effective hero definition"));

    return core::Result<std::shared_ptr<core::dson::DsonDocument>, core::Error>::success(std::move(result));
}

} // namespace ddse::application
