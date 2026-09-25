#include "ddse/application/campaign_edit_session.hpp"

#include "ddse/application/campaign_mappings.hpp"
#include "ddse/core/dson/dson_document.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ddse::application {
namespace {

using domain::CampaignModel;
using domain::EntityState;
using domain::RawLocator;

struct ResolvedTarget {
    CampaignValue value;
    RawLocator raw;
    EntityState entity_state{EntityState::Resolved};
    std::function<void(const CampaignValue&)> assign;
};

const CampaignMappingDescriptor* find_mapping(std::string_view property) {
    return find_campaign_mapping(property);
}

bool campaign_value_matches_kind(core::dson::ValueKind kind, const CampaignValue& value) {
    return (kind == core::dson::ValueKind::Integer && std::holds_alternative<std::int32_t>(value)) ||
           (kind == core::dson::ValueKind::Float && std::holds_alternative<float>(value)) ||
           (kind == core::dson::ValueKind::String && std::holds_alternative<std::string>(value)) ||
           (kind == core::dson::ValueKind::Boolean && std::holds_alternative<bool>(value)) ||
           (kind == core::dson::ValueKind::Character && std::holds_alternative<char>(value));
}

bool is_editable_property(std::string_view property) {
    const auto* mapping = find_mapping(property);
    return mapping != nullptr && mapping->editable_in_session;
}

std::vector<SetCampaignValueOperation> flatten(const CampaignOperation& operation) {
    if (const auto* single = std::get_if<SetCampaignValueOperation>(&operation)) return {*single};
    if (const auto* composite = std::get_if<CompositeCampaignOperation>(&operation)) return composite->operations;
    if (const auto* state = std::get_if<SetHeroAfflictionStateOperation>(&operation)) {
        const bool afflicted = state->state == HeroAfflictionState::Afflicted;
        return {
            {{"Hero.AfflictionId", state->hero_id}, afflicted ? state->affliction_id : std::string{}},
            {{"Hero.AfflictionSeverity", state->hero_id}, afflicted ? std::int32_t{1} : std::int32_t{0}},
            {{"Hero.VirtueId", state->hero_id}, std::string{}},
        };
    }
    if (const auto* quirk = std::get_if<SetHeroQuirkLockedOperation>(&operation))
        return {{{"Hero.Quirk.Locked", quirk->hero_id, std::nullopt, quirk->quirk_id}, quirk->locked}};
    if (const auto* district = std::get_if<SetDistrictBuiltOperation>(&operation))
        return {{{"Town.District.Built", {}, std::nullopt, district->district_id}, district->built}};
    return {};
}

std::string operation_label(const CampaignOperation& operation) {
    if (const auto* single = std::get_if<SetCampaignValueOperation>(&operation))
        return single->target.semantic_property;
    if (const auto* composite = std::get_if<CompositeCampaignOperation>(&operation))
        return composite->label.empty() ? "Composite campaign operation" : composite->label;
    if (std::holds_alternative<SetHeroQuirkLockedOperation>(operation)) return "Set hero quirk lock";
    if (std::holds_alternative<SetHeroAfflictionStateOperation>(operation)) return "Set hero affliction state";
    if (std::holds_alternative<SetDistrictBuiltOperation>(operation)) return "Set district built state";
    if (const auto* district = std::get_if<SetDistrictSystemOperation>(&operation))
        return district->open ? "Unlock district system" : "Lock district system";
    if (std::holds_alternative<RemoveHeroQuirkOperation>(operation)) return "Remove hero quirk";
    if (std::holds_alternative<UnequipHeroCampingSkillOperation>(operation)) return "Unequip camping skill";
    if (std::holds_alternative<ApplyCampaignDocumentMutationsOperation>(operation))
        return std::get<ApplyCampaignDocumentMutationsOperation>(operation).operation_id;
    return "Destroy trinket";
}

std::vector<std::string_view> split_mapping_path(std::string_view path) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto split = path.find(" => ", start);
        const auto end = split == std::string_view::npos ? path.size() : split;
        parts.push_back(path.substr(start, end - start));
        if (split == std::string_view::npos) break;
        start = split + 4;
    }
    return parts;
}

std::vector<std::string_view> split_segments(std::string_view path) {
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        segments.push_back(path.substr(start, end == std::string_view::npos ? path.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return segments;
}

bool template_part_prefix_matches(std::string_view pattern, std::string_view value) {
    const auto patterns = split_segments(pattern);
    const auto values = split_segments(value);
    if (patterns.size() > values.size()) return false;
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        const auto item = patterns[i];
        if (item.size() >= 2 && item.front() == '{' && item.back() == '}') {
            if (values[i].empty()) return false;
        } else if (item != values[i]) return false;
    }
    return true;
}

bool path_is_within_mapping(const CampaignMappingDescriptor& mapping, std::string_view path) {
    const auto patterns = split_mapping_path(mapping.raw_path_template);
    const auto values = split_mapping_path(path);
    if (patterns.empty() || values.size() < patterns.size()) return false;
    for (std::size_t i = 0; i < patterns.size(); ++i)
        if (!template_part_prefix_matches(patterns[i], values[i])) return false;
    return true;
}

bool camping_scalar_clone_source_allowed(const CampaignDocumentMutation& mutation) {
    if (mutation.semantic_property != "Hero.SelectedCampingSkills" ||
        mutation.kind != CampaignDocumentMutationKind::AppendClone ||
        mutation.expected_kind != core::dson::ValueKind::Integer ||
        mutation.document_id != "persist.roster.json") return false;
    const auto* combat = find_mapping("Hero.SelectedCombatSkills");
    return combat != nullptr && path_is_within_mapping(*combat, mutation.source_path);
}

bool safe_dson_key(std::string_view key) {
    return !key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '.' || value == ':';
    });
}

template <typename T, typename Slot>
std::optional<ResolvedTarget> resolve_slot(Slot& slot, EntityState entity_state) {
    if (!slot.value || !slot.raw) return std::nullopt;
    ResolvedTarget result{CampaignValue{*slot.value}, *slot.raw, entity_state, {}};
    if constexpr (!std::is_const_v<std::remove_reference_t<Slot>>) {
        auto* target = &slot;
        result.assign = [target](const CampaignValue& value) {
            if (const auto* typed = std::get_if<T>(&value)) target->value = *typed;
        };
    }
    return result;
}

template <typename Model>
std::optional<ResolvedTarget> resolve_target(Model& model, const CampaignOperationTarget& target) {
    if (target.semantic_property == "Upgrade.PurchaseNode") {
        if (!target.occurrence_index || target.entity_id.empty() || target.member_id.empty()) return std::nullopt;
        std::int32_t instance{};
        const auto [instance_end, instance_error] = std::from_chars(
            target.entity_id.data(), target.entity_id.data() + target.entity_id.size(), instance);
        const auto separator = target.member_id.find('|');
        if (instance_error != std::errc{} || instance_end != target.entity_id.data() + target.entity_id.size() ||
            separator == std::string::npos) return std::nullopt;
        std::int32_t tree{};
        const auto tree_text = std::string_view{target.member_id}.substr(0, separator);
        const auto [tree_end, tree_error] = std::from_chars(tree_text.data(), tree_text.data() + tree_text.size(), tree);
        const auto code = std::string_view{target.member_id}.substr(separator + 1);
        if (tree_error != std::errc{} || tree_end != tree_text.data() + tree_text.size() || code.size() != 1)
            return std::nullopt;
        const auto found = std::find_if(model.upgrade_purchase_nodes.begin(), model.upgrade_purchase_nodes.end(),
            [&](const auto& node) {
                return node.index == *target.occurrence_index && node.instance_number == instance &&
                       node.tree_id == tree && node.requirement_code == code.front();
            });
        if (found == model.upgrade_purchase_nodes.end()) return std::nullopt;
        return resolve_slot<bool>(found->is_purchased, EntityState::Resolved);
    }

    if (target.semantic_property == "Estate.Resource.Amount") {
        if (!target.occurrence_index) return std::nullopt;
        const auto match_count = std::count_if(model.resources.begin(), model.resources.end(), [&](const auto& item) {
            return item.index == *target.occurrence_index;
        });
        if (match_count != 1) return std::nullopt;
        const auto found = std::find_if(model.resources.begin(), model.resources.end(), [&](const auto& item) {
            return item.index == *target.occurrence_index;
        });
        if (found == model.resources.end()) return std::nullopt;
        return resolve_slot<std::int32_t>(found->amount, found->state);
    }

    if (target.semantic_property == "Town.District.Built") {
        if (target.member_id.empty()) return std::nullopt;
        const auto count = std::count_if(model.districts.begin(), model.districts.end(), [&](const auto& item) {
            return item.id == target.member_id;
        });
        if (count != 1) return std::nullopt;
        const auto found = std::find_if(model.districts.begin(), model.districts.end(), [&](const auto& item) {
            return item.id == target.member_id;
        });
        return resolve_slot<bool>(found->built, EntityState::Resolved);
    }

    if (target.entity_id.empty()) return std::nullopt;
    const auto match_count = std::count_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
        return item.persistent_id == target.entity_id;
    });
    if (match_count != 1) return std::nullopt;
    const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
        return item.persistent_id == target.entity_id;
    });
    if (hero == model.heroes.end()) return std::nullopt;

    if (target.semantic_property == "Hero.Quirk.Locked") {
        if (target.member_id.empty()) return std::nullopt;
        const auto count = std::count_if(hero->quirks.begin(), hero->quirks.end(), [&](const auto& item) {
            return item.id == target.member_id;
        });
        if (count != 1) return std::nullopt;
        const auto quirk = std::find_if(hero->quirks.begin(), hero->quirks.end(), [&](const auto& item) {
            return item.id == target.member_id;
        });
        return resolve_slot<bool>(quirk->is_locked, quirk->state);
    }

    if (target.semantic_property == "Hero.Name") return resolve_slot<std::string>(hero->name, hero->state);
    if (target.semantic_property == "Hero.ResolveXp") return resolve_slot<std::int32_t>(hero->resolve_xp, hero->state);
    if (target.semantic_property == "Hero.Stress") return resolve_slot<float>(hero->stress, hero->state);
    if (target.semantic_property == "Hero.AfflictionId") return resolve_slot<std::string>(hero->affliction_id, hero->state);
    if (target.semantic_property == "Hero.AfflictionSeverity") return resolve_slot<std::int32_t>(hero->affliction_severity, hero->state);
    if (target.semantic_property == "Hero.VirtueId") return resolve_slot<std::string>(hero->virtue_id, hero->state);
    if (target.semantic_property == "Hero.CurrentHp") return resolve_slot<float>(hero->current_hp, hero->state);
    if (target.semantic_property == "Hero.WeaponRank") return resolve_slot<std::int32_t>(hero->weapon_rank, hero->state);
    if (target.semantic_property == "Hero.ArmourRank") return resolve_slot<std::int32_t>(hero->armour_rank, hero->state);
    return std::nullopt;
}

void add_issue(ValidationReport& report, ValidationSeverity severity, std::string code,
               std::string message, const CampaignOperationTarget& target, bool blocking) {
    report.issues.push_back({severity, std::move(code), std::move(message),
                             target.semantic_property, target.entity_id, blocking});
}

void validate_one(const CampaignModel& model, const SetCampaignValueOperation& operation,
                  ValidationReport& report, bool allow_affliction_state_fields = false) {
    const auto& target = operation.target;
    const auto* mapping = find_mapping(target.semantic_property);
    if (mapping == nullptr) {
        add_issue(report, ValidationSeverity::Error, "mapping.missing",
                  "No campaign mapping exists for this semantic property", target, true);
        return;
    }
    const bool stress_condition_field = target.semantic_property == "Hero.AfflictionId" ||
        target.semantic_property == "Hero.AfflictionSeverity" || target.semantic_property == "Hero.VirtueId";
    if (!is_editable_property(target.semantic_property) && !(allow_affliction_state_fields && stress_condition_field)) {
        add_issue(report, ValidationSeverity::Error, "mapping.not_editable_in_session",
                  "The mapped property does not have an in-memory operation yet", target, true);
        return;
    }

    const auto resolved = resolve_target(model, target);
    if (!resolved) {
        add_issue(report, ValidationSeverity::Error, "target.missing",
                  "The target entity or mapped value is not present in the campaign model", target, true);
        return;
    }
    if (resolved->raw.document_id != mapping->document_id || resolved->raw.display_path.empty()) {
        add_issue(report, ValidationSeverity::Error, "mapping.locator_mismatch",
                  "The semantic value has no locator matching its registered document", target, true);
        return;
    }
    if (resolved->entity_state == EntityState::Invalid || resolved->entity_state == EntityState::Unresolved) {
        add_issue(report, ValidationSeverity::Error, "target.unavailable",
                  "The target entity is invalid or unresolved", target, true);
        return;
    }

    const bool type_matches =
        (mapping->expected_type == core::dson::ValueKind::Integer && std::holds_alternative<std::int32_t>(operation.value)) ||
        (mapping->expected_type == core::dson::ValueKind::Float && std::holds_alternative<float>(operation.value)) ||
        (mapping->expected_type == core::dson::ValueKind::String && std::holds_alternative<std::string>(operation.value)) ||
        (mapping->expected_type == core::dson::ValueKind::Boolean && std::holds_alternative<bool>(operation.value)) ||
        (mapping->expected_type == core::dson::ValueKind::Character && std::holds_alternative<char>(operation.value));
    if (!type_matches || operation.value.index() != resolved->value.index()) {
        add_issue(report, ValidationSeverity::Error, "value.type_mismatch",
                  "The requested value type does not match the mapped campaign field", target, true);
        return;
    }

    if (const auto* integer = std::get_if<std::int32_t>(&operation.value); integer && *integer < 0) {
        add_issue(report, ValidationSeverity::Error, "value.negative_integer",
                  "Resource amounts, XP, and equipment ranks must be non-negative", target, true);
        return;
    }
    if (const auto* number = std::get_if<float>(&operation.value); number && (!std::isfinite(*number) || *number < 0.0F)) {
        add_issue(report, ValidationSeverity::Error, "value.invalid_number",
                  "The value must be a finite non-negative number", target, true);
        return;
    }
    if (target.semantic_property == "Hero.Name" &&
        std::holds_alternative<std::string>(operation.value) && std::get<std::string>(operation.value).empty()) {
        add_issue(report, ValidationSeverity::Error, "value.empty_name",
                  "A hero name cannot be empty", target, true);
        return;
    }
    if (target.semantic_property == "Hero.Quirk.Locked") {
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
            return item.persistent_id == target.entity_id;
        });
        if (hero == model.heroes.end()) {
            add_issue(report, ValidationSeverity::Error, "quirk.lock_ineligible",
                      "Only an existing positive, non-disease, lockable quirk can have its lock state changed", target, true);
            return;
        }
        const auto quirk = std::find_if(hero->quirks.begin(), hero->quirks.end(), [&](const auto& item) {
            return item.id == target.member_id;
        });
        if (quirk == hero->quirks.end() || quirk->is_disease ||
            quirk->polarity != domain::QuirkPolarity::Positive) {
            add_issue(report, ValidationSeverity::Error, "quirk.lock_ineligible",
                      "Only an existing positive, non-disease, lockable quirk can have its lock state changed", target, true);
            return;
        }
        bool definition_allows_lock = false;
        try {
            const auto definition = nlohmann::json::parse(quirk->definition.definition_payload_json);
            definition_allows_lock = definition.is_object() && definition.value("can_modify_in_activity", false);
        } catch (const nlohmann::json::exception&) {
        }
        if (!definition_allows_lock) {
            add_issue(report, ValidationSeverity::Error, "quirk.lock_not_supported",
                      "The effective quirk definition does not permit locking", target, true);
            return;
        }
    }
    if (resolved->entity_state == EntityState::Partial) {
        add_issue(report, ValidationSeverity::Warning, "target.partial_entity",
                  "The field is available, but other parts of this entity are unresolved", target, false);
    }
}

core::Error report_error(const ValidationReport& report) {
    const auto issue = std::find_if(report.issues.begin(), report.issues.end(), [](const auto& item) {
        return item.blocking;
    });
    const auto code = issue != report.issues.end() && issue->code == "mapping.missing"
        ? core::ErrorCode::MappingNotWritable : core::ErrorCode::ValidationFailed;
    std::map<std::string, std::string> context;
    if (issue != report.issues.end()) {
        context.emplace("property", issue->semantic_property);
        if (!issue->entity_id.empty()) context.emplace("entity_id", issue->entity_id);
        context.emplace("validation_code", issue->code);
    }
    return {code, issue == report.issues.end() ? "Campaign operation validation failed" : issue->message,
            "CampaignEditSession", std::move(context)};
}

ChangeSet changes_for(const std::vector<CampaignFieldChange>& changes) {
    ChangeSet result;
    result.changes = changes;
    std::set<std::string, std::less<>> documents;
    for (const auto& change : result.changes) documents.insert(change.raw.document_id);
    result.affected_documents.assign(documents.begin(), documents.end());
    return result;
}

ChangeSet changes_for(const std::vector<CampaignFieldChange>& changes,
                      const std::vector<CampaignStructuralChange>& structural_changes) {
    auto result = changes_for(changes);
    result.structural_changes = structural_changes;
    std::set<std::string, std::less<>> documents(result.affected_documents.begin(), result.affected_documents.end());
    for (const auto& change : structural_changes) documents.insert(change.raw.document_id);
    result.affected_documents.assign(documents.begin(), documents.end());
    return result;
}

std::optional<CampaignStructuralChange> remove_structural_entry(
    CampaignModel& model, const CampaignOperation& operation) {
    if (const auto* remove = std::get_if<RemoveHeroQuirkOperation>(&operation)) {
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
            return item.persistent_id == remove->hero_id;
        });
        if (hero == model.heroes.end()) return std::nullopt;
        const auto found = std::find_if(hero->quirks.begin(), hero->quirks.end(), [&](const auto& item) {
            return item.id == remove->quirk_id && !item.is_disease;
        });
        if (found == hero->quirks.end()) return std::nullopt;
        const auto index = static_cast<std::size_t>(std::distance(hero->quirks.begin(), found));
        CampaignStructuralChange change{{"Hero.Quirk.Entry", remove->hero_id, std::nullopt, remove->quirk_id},
                                        found->raw, core::dson::ValueKind::Object, index, *found};
        hero->quirks.erase(found);
        return change;
    }
    if (const auto* unequip = std::get_if<UnequipHeroCampingSkillOperation>(&operation)) {
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
            return item.persistent_id == unequip->hero_id;
        });
        if (hero == model.heroes.end()) return std::nullopt;
        const auto found = std::find_if(hero->camping_skills.begin(), hero->camping_skills.end(), [&](const auto& item) {
            return item.camping && item.id == unequip->skill_id;
        });
        if (found == hero->camping_skills.end()) return std::nullopt;
        const auto index = static_cast<std::size_t>(std::distance(hero->camping_skills.begin(), found));
        CampaignStructuralChange change{{"Hero.SelectedCampingSkills", unequip->hero_id, std::nullopt,
                                         unequip->skill_id},
                                        found->raw, core::dson::ValueKind::Integer, index, *found};
        hero->camping_skills.erase(found);
        return change;
    }
    if (const auto* destroy = std::get_if<DestroyTrinketOperation>(&operation)) {
        if (destroy->hero_id.empty()) {
            const auto found = std::find_if(model.trinket_inventory.begin(), model.trinket_inventory.end(), [&](const auto& item) {
                return item.raw_key == destroy->item_key || item.raw.display_path == destroy->item_key;
            });
            if (found == model.trinket_inventory.end()) return std::nullopt;
            const auto index = static_cast<std::size_t>(std::distance(model.trinket_inventory.begin(), found));
            CampaignStructuralChange change{{"TrinketInventory.Entry", {}, std::nullopt, found->raw_key},
                                            found->raw, core::dson::ValueKind::Object, index, *found};
            model.trinket_inventory.erase(found);
            return change;
        }
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
            return item.persistent_id == destroy->hero_id;
        });
        if (hero == model.heroes.end()) return std::nullopt;
        const auto found = std::find_if(hero->trinkets.begin(), hero->trinkets.end(), [&](const auto& item) {
            return item.raw.display_path == destroy->item_key;
        });
        if (found == hero->trinkets.end()) return std::nullopt;
        const auto index = static_cast<std::size_t>(std::distance(hero->trinkets.begin(), found));
        CampaignStructuralChange change{{"Hero.Trinket.Entry", destroy->hero_id, std::nullopt,
                                          found->raw.steps.empty() ? std::string{} : found->raw.steps.back().field_name},
                                        found->raw, core::dson::ValueKind::Object, index, *found};
        hero->trinkets.erase(found);
        return change;
    }
    return std::nullopt;
}

bool replay_structural_entry(CampaignModel& model, const CampaignStructuralChange& change, bool forward) {
    const auto insert_at = [](auto& values, std::size_t index, const auto& value) {
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(std::min(index, values.size())), value);
    };
    if (change.target.semantic_property == "Hero.Quirk.Entry") {
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
            return item.persistent_id == change.target.entity_id;
        });
        if (hero == model.heroes.end()) return false;
        if (forward) {
            const auto found = std::find_if(hero->quirks.begin(), hero->quirks.end(), [&](const auto& item) {
                return item.raw.display_path == change.raw.display_path;
            });
            if (found == hero->quirks.end()) return false;
            hero->quirks.erase(found);
        } else if (const auto* entry = std::get_if<domain::HeroQuirk>(&change.removed_entry)) {
            insert_at(hero->quirks, change.original_index, *entry);
        } else return false;
        return true;
    }
    if (change.target.semantic_property == "Hero.SelectedCampingSkills") {
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
            return item.persistent_id == change.target.entity_id;
        });
        if (hero == model.heroes.end()) return false;
        if (forward) {
            const auto found = std::find_if(hero->camping_skills.begin(), hero->camping_skills.end(), [&](const auto& item) {
                return item.raw.display_path == change.raw.display_path;
            });
            if (found == hero->camping_skills.end()) return false;
            hero->camping_skills.erase(found);
        } else if (const auto* entry = std::get_if<domain::HeroSkillSelection>(&change.removed_entry)) {
            insert_at(hero->camping_skills, change.original_index, *entry);
        } else return false;
        return true;
    }
    if (change.target.semantic_property == "Hero.Trinket.Entry") {
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
            return item.persistent_id == change.target.entity_id;
        });
        if (hero == model.heroes.end()) return false;
        if (forward) {
            const auto found = std::find_if(hero->trinkets.begin(), hero->trinkets.end(), [&](const auto& item) {
                return item.raw.display_path == change.raw.display_path;
            });
            if (found == hero->trinkets.end()) return false;
            hero->trinkets.erase(found);
        } else if (const auto* entry = std::get_if<domain::HeroTrinket>(&change.removed_entry)) {
            insert_at(hero->trinkets, change.original_index, *entry);
        } else return false;
        return true;
    }
    if (change.target.semantic_property == "TrinketInventory.Entry") {
        if (forward) {
            const auto found = std::find_if(model.trinket_inventory.begin(), model.trinket_inventory.end(), [&](const auto& item) {
                return item.raw.display_path == change.raw.display_path;
            });
            if (found == model.trinket_inventory.end()) return false;
            model.trinket_inventory.erase(found);
        } else if (const auto* entry = std::get_if<domain::TrinketInventoryEntry>(&change.removed_entry)) {
            insert_at(model.trinket_inventory, change.original_index, *entry);
        } else return false;
        return true;
    }
    return false;
}

bool project_district_state_mutations(CampaignModel& model,
                                     const std::vector<CampaignDocumentMutation>& mutations) {
    constexpr std::string_view prefix{"base_root/districts/buildings/"};
    for (const auto& mutation : mutations) {
        if (mutation.semantic_property != "Town.DistrictSystem" ||
            mutation.document_id != "persist.town.json" ||
            mutation.kind != CampaignDocumentMutationKind::SetValue || !mutation.after ||
            !mutation.target_path.starts_with(prefix) || !mutation.target_path.ends_with("/built")) continue;
        const auto id = std::string_view{mutation.target_path}.substr(
            prefix.size(), mutation.target_path.size() - prefix.size() - 6);
        if (id.empty() || id.find('/') != std::string_view::npos) return false;
        const auto* desired = std::get_if<bool>(&*mutation.after);
        if (!desired) return false;
        const auto found = std::find_if(model.districts.begin(), model.districts.end(), [&](const auto& item) {
            return item.id == id;
        });
        if (found == model.districts.end()) {
            domain::DistrictState district;
            district.id = std::string{id};
            district.read_only = false;
            district.raw = {"persist.town.json", {}, "base_root/districts/buildings/" + std::string{id}};
            district.built = {*desired, domain::RawLocator{"persist.town.json", {}, mutation.target_path}};
            district.definition.raw_id = std::string{id};
            model.districts.push_back(std::move(district));
        } else {
            found->read_only = false;
            found->built.value = *desired;
            found->built.raw = domain::RawLocator{"persist.town.json", {}, mutation.target_path};
        }
    }
    model.district_system_open = model.district_system_open || !model.districts.empty();
    return true;
}

bool apply_purchase_row_mutations(CampaignModel& model,
                                 const std::vector<CampaignDocumentMutation>& mutations,
                                 bool forward, std::string* failure_reason = nullptr) {
    const auto fail_projection = [&](std::string reason) {
        if (failure_reason) *failure_reason = std::move(reason);
        return false;
    };
    for (const auto& append : mutations) {
        if (append.kind != CampaignDocumentMutationKind::AppendClone ||
            append.semantic_property != "Upgrade.PurchaseNode.Entry" ||
            append.document_id != "persist.upgrades.json") continue;
        const auto slash = append.target_path.find_last_of('/');
        if (slash == std::string::npos) return fail_projection("append path has no row key: " + append.target_path);
        const auto row_path = append.target_path;
        std::size_t index{};
        const auto key = std::string_view{append.target_path}.substr(slash + 1);
        const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), index);
        if (error != std::errc{} || end != key.data() + key.size())
            return fail_projection("append row key is not numeric: " + append.target_path);
        const auto existing = std::find_if(model.upgrade_purchase_nodes.begin(), model.upgrade_purchase_nodes.end(),
            [&](const auto& node) { return node.index == index; });
        if (!forward) {
            if (existing != model.upgrade_purchase_nodes.end()) model.upgrade_purchase_nodes.erase(existing);
            continue;
        }
        if (existing != model.upgrade_purchase_nodes.end())
            return fail_projection("append row index already exists: " + append.target_path);

        std::optional<std::int32_t> instance;
        std::optional<std::int32_t> tree;
        std::optional<char> code;
        std::optional<bool> purchased;
        RawLocator row_raw{"persist.upgrades.json", {}, row_path};
        RawLocator purchased_raw{"persist.upgrades.json", {}, row_path + "/is_purchased"};
        for (const auto& mutation : mutations) {
            if (mutation.kind != CampaignDocumentMutationKind::SetValue ||
                mutation.semantic_property != "Upgrade.PurchaseNode.Entry" ||
                mutation.document_id != "persist.upgrades.json" || !mutation.after ||
                !mutation.target_path.starts_with(row_path + "/")) continue;
            const auto field = std::string_view{mutation.target_path}.substr(row_path.size() + 1);
            if (field == "instance_number") {
                if (const auto* value = std::get_if<std::int32_t>(&*mutation.after)) instance = *value;
            } else if (field == "tree_id") {
                if (const auto* value = std::get_if<std::int32_t>(&*mutation.after)) tree = *value;
            } else if (field == "requirement_code") {
                if (const auto* value = std::get_if<char>(&*mutation.after)) code = *value;
            } else if (field == "is_purchased") {
                if (const auto* value = std::get_if<bool>(&*mutation.after)) {
                    purchased = *value;
                    purchased_raw.display_path = mutation.target_path;
                }
            }
        }
        if (!instance || !tree || !code || !purchased)
            return fail_projection("append row lacks typed fields at " + row_path + " (instance=" +
                std::to_string(instance.has_value()) + ", tree=" + std::to_string(tree.has_value()) +
                ", code=" + std::to_string(code.has_value()) + ", purchased=" +
                std::to_string(purchased.has_value()) + ")");
        model.upgrade_purchase_nodes.push_back({index, *instance, *tree, *code,
                                                std::move(row_raw), {*purchased, std::move(purchased_raw)}});
    }
    return true;
}

bool apply_camping_skill_mutations(CampaignModel& model,
                                   const std::vector<CampaignDocumentMutation>& mutations) {
    for (const auto& mutation : mutations) {
        if (mutation.semantic_property != "Hero.SelectedCampingSkills") continue;
        const auto owner = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& hero) {
            return mutation.target_path.starts_with(hero.raw.display_path +
                "/hero_file_data/raw_data => base_root/skills/selected_camping_skills/");
        });
        if (owner == model.heroes.end()) return false;
        if (mutation.kind == CampaignDocumentMutationKind::AppendClone) {
            std::optional<domain::HeroSkillSelection> source;
            for (const auto& hero : model.heroes) {
                for (const auto& skill : hero.camping_skills)
                    if (skill.raw.display_path == mutation.source_path) source = skill;
                for (const auto& skill : hero.combat_skills)
                    if (skill.raw.display_path == mutation.source_path) source = skill;
            }
            if (!source || std::any_of(owner->camping_skills.begin(), owner->camping_skills.end(),
                [&](const auto& skill) { return skill.id == mutation.new_key; })) return false;
            source->id = mutation.new_key;
            source->camping = true;
            source->read_only = false;
            source->raw = {"persist.roster.json", {}, mutation.target_path};
            source->raw_value.raw = source->raw;
            source->definition = {};
            owner->camping_skills.push_back(std::move(*source));
        } else if (mutation.kind == CampaignDocumentMutationKind::Erase) {
            const auto found = std::find_if(owner->camping_skills.begin(), owner->camping_skills.end(),
                [&](const auto& skill) { return skill.raw.display_path == mutation.target_path; });
            if (found == owner->camping_skills.end()) return false;
            owner->camping_skills.erase(found);
        } else return false;
    }
    return true;
}

bool apply_trinket_inventory_mutations(CampaignModel& model,
                                      const std::vector<CampaignDocumentMutation>& mutations,
                                      bool forward) {
    const auto parse_index = [](std::string_view path) -> std::optional<std::size_t> {
        const auto slash = path.find_last_of('/');
        if (slash == std::string_view::npos) return std::nullopt;
        const auto value = path.substr(slash + 1);
        std::size_t index{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), index);
        if (value.empty() || error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
        return index;
    };
    if (!forward) return true; // History restores the captured model snapshot.
    for (const auto& mutation : mutations) {
        if (mutation.semantic_property != "TrinketInventory.Items" ||
            mutation.document_id != "persist.estate.json") continue;
        if (mutation.kind == CampaignDocumentMutationKind::Erase) {
            const auto found = std::find_if(model.trinket_inventory.begin(), model.trinket_inventory.end(),
                [&](const auto& item) { return item.raw.display_path == mutation.target_path; });
            if (found == model.trinket_inventory.end()) return false;
            model.trinket_inventory.erase(found);
        } else if (mutation.kind == CampaignDocumentMutationKind::Rename) {
            const auto found = std::find_if(model.trinket_inventory.begin(), model.trinket_inventory.end(),
                [&](const auto& item) { return item.raw.display_path == mutation.target_path; });
            if (found == model.trinket_inventory.end()) return false;
            const auto slash = mutation.target_path.find_last_of('/');
            const auto renamed_path = mutation.target_path.substr(0, slash + 1) + mutation.new_key;
            const auto index = parse_index(renamed_path);
            if (!index) return false;
            found->raw.display_path = renamed_path;
            found->raw_key = mutation.new_key;
            found->index = *index;
        } else if (mutation.kind == CampaignDocumentMutationKind::AppendClone ||
                   mutation.kind == CampaignDocumentMutationKind::CreateObject) {
            const auto index = parse_index(mutation.target_path);
            if (!index) return false;
            const auto source = std::find_if(model.trinket_inventory.begin(), model.trinket_inventory.end(),
                [&](const auto& item) { return item.raw.display_path == mutation.source_path; });
            domain::TrinketInventoryEntry entry;
            if (mutation.kind == CampaignDocumentMutationKind::AppendClone && source != model.trinket_inventory.end())
                entry = *source;
            entry.index = *index;
            entry.raw_key = mutation.new_key;
            entry.raw = RawLocator{"persist.estate.json", {}, mutation.target_path};
            entry.id.raw = RawLocator{"persist.estate.json", {}, mutation.target_path + "/id"};
            entry.amount.raw = RawLocator{"persist.estate.json", {}, mutation.target_path + "/amount"};
            if (mutation.kind == CampaignDocumentMutationKind::CreateObject) {
                entry.item_type.value = "trinket";
                entry.amount.value = 1;
            }
            for (const auto& field : mutations) {
                if (field.semantic_property != "TrinketInventory.Items" || !field.after ||
                    !field.target_path.starts_with(mutation.target_path + "/")) continue;
                const auto name = std::string_view{field.target_path}.substr(mutation.target_path.size() + 1);
                if (name == "id") {
                    if (const auto* value = std::get_if<std::string>(&*field.after)) {
                        entry.id.value = *value;
                        entry.definition.raw_id = *value;
                        entry.definition.state = EntityState::Unresolved;
                        entry.definition.display_name = *value;
                    }
                } else if (name == "amount") {
                    if (const auto* value = std::get_if<std::int32_t>(&*field.after)) entry.amount.value = *value;
                }
            }
            if (!entry.id.value || !entry.amount.value) return false;
            model.trinket_inventory.push_back(std::move(entry));
        }
    }
    std::stable_sort(model.trinket_inventory.begin(), model.trinket_inventory.end(),
        [](const auto& left, const auto& right) { return left.index < right.index; });
    return true;
}

bool apply_hero_roster_mutations(CampaignModel& model,
                                 const std::vector<CampaignDocumentMutation>& mutations) {
    const auto rewrite = [](std::string& path, std::string_view old_id, std::string_view new_id) {
        const auto marker = "/heroes/" + std::string{old_id};
        const auto position = path.find(marker);
        if (position != std::string::npos) path.replace(position + 8, old_id.size(), new_id);
    };
    for (const auto& mutation : mutations) {
        if (mutation.semantic_property != "Hero.PersistentId" || mutation.document_id != "persist.roster.json") continue;
        if (mutation.kind == CampaignDocumentMutationKind::Erase) {
            const auto found = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& hero) {
                return hero.raw.display_path == mutation.target_path;
            });
            if (found == model.heroes.end()) return false;
            model.heroes.erase(found);
            for (std::size_t index = 0; index < model.heroes.size(); ++index) model.heroes[index].roster_position = index;
        } else if (mutation.kind == CampaignDocumentMutationKind::Rename) {
            const auto found = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& hero) {
                return hero.raw.display_path == mutation.target_path;
            });
            if (found == model.heroes.end()) return false;
            const auto old_id = found->persistent_id;
            found->persistent_id = mutation.new_key;
            rewrite(found->raw.display_path, old_id, mutation.new_key);
            if (found->name.raw) rewrite(found->name.raw->display_path, old_id, mutation.new_key);
            if (found->class_id.raw) rewrite(found->class_id.raw->display_path, old_id, mutation.new_key);
        } else if (mutation.kind == CampaignDocumentMutationKind::AppendTemplate) {
            domain::Hero added;
            added.persistent_id = mutation.new_key;
            added.roster_position = model.heroes.size();
            added.state = domain::EntityState::Resolved;
            added.read_only = false;
            added.raw = {"persist.roster.json", {}, mutation.target_path};
            const auto embedded = mutation.target_path + "/hero_file_data/raw_data => base_root/";
            added.name.value = std::string{};
            added.name.raw = domain::RawLocator{"persist.roster.json", {}, embedded + "actor/name"};
            added.class_id.value = mutation.template_hero_class;
            added.class_id.raw = domain::RawLocator{"persist.roster.json", {}, embedded + "heroClass"};
            added.definition.raw_id = mutation.template_hero_class;
            added.definition.display_name = mutation.template_class_name;
            added.definition.source_id = mutation.template_source_id;
            added.definition.state = domain::EntityState::Resolved;
            if (!mutation.template_portrait_path.empty())
                added.definition.assets.push_back({"portrait_roster", mutation.template_portrait_path,
                    mutation.template_source_id, true});
            added.resolve_xp.value = 0;
            added.resolve_xp.raw = domain::RawLocator{"persist.roster.json", {}, embedded + "resolveXp"};
            added.level.value = 0;
            added.stress.value = 0.0F;
            added.stress.raw = domain::RawLocator{"persist.roster.json", {}, embedded + "m_Stress"};
            added.current_hp.value = mutation.template_base_hit_points;
            added.current_hp.raw = domain::RawLocator{"persist.roster.json", {}, embedded + "actor/current_hp"};
            added.weapon_rank.value = 0;
            added.weapon_rank.raw = domain::RawLocator{"persist.roster.json", {}, embedded + "weapon_rank"};
            added.armour_rank.value = 0;
            added.armour_rank.raw = domain::RawLocator{"persist.roster.json", {}, embedded + "armour_rank"};
            for (const auto& id : mutation.template_combat_skills) {
                domain::HeroSkillSelection skill;
                skill.id = id;
                skill.read_only = false;
                skill.raw = {"persist.roster.json", {}, embedded + "skills/selected_combat_skills/" + id};
                skill.raw_value.value = 0;
                skill.raw_value.raw = skill.raw;
                added.combat_skills.push_back(std::move(skill));
            }
            for (const auto& id : mutation.template_camping_skills) {
                domain::HeroSkillSelection skill;
                skill.id = id;
                skill.camping = true;
                skill.read_only = false;
                skill.raw = {"persist.roster.json", {}, embedded + "skills/selected_camping_skills/" + id};
                skill.raw_value.value = 0;
                skill.raw_value.raw = skill.raw;
                added.camping_skills.push_back(std::move(skill));
            }
            model.heroes.push_back(std::move(added));
        } else if (mutation.kind == CampaignDocumentMutationKind::AppendClone) {
            const auto source = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& hero) {
                return hero.raw.display_path == mutation.source_path;
            });
            if (source == model.heroes.end()) return false;
            auto added = *source;
            const auto old_id = added.persistent_id;
            added.persistent_id = mutation.new_key;
            added.roster_position = model.heroes.size();
            rewrite(added.raw.display_path, old_id, mutation.new_key);
            added.raw.display_path = mutation.target_path;
            if (added.name.raw) rewrite(added.name.raw->display_path, old_id, mutation.new_key);
            if (added.class_id.raw) rewrite(added.class_id.raw->display_path, old_id, mutation.new_key);
            for (const auto& field : mutations) {
                if (field.semantic_property != "Hero.PersistentId" || !field.after ||
                    !field.target_path.starts_with(mutation.target_path + "/hero_file_data/raw_data => base_root/")) continue;
                if (field.target_path.ends_with("/actor/name")) {
                    if (const auto* value = std::get_if<std::string>(&*field.after)) added.name.value = *value;
                } else if (field.target_path.ends_with("/resolveXp")) {
                    if (const auto* value = std::get_if<std::int32_t>(&*field.after)) added.resolve_xp.value = *value;
                } else if (field.target_path.ends_with("/weapon_rank")) {
                    if (const auto* value = std::get_if<std::int32_t>(&*field.after)) added.weapon_rank.value = *value;
                } else if (field.target_path.ends_with("/armour_rank")) {
                    if (const auto* value = std::get_if<std::int32_t>(&*field.after)) added.armour_rank.value = *value;
                } else if (field.target_path.ends_with("/m_Stress")) {
                    if (const auto* value = std::get_if<float>(&*field.after)) added.stress.value = *value;
                }
            }
            for (const auto& field : mutations) if (field.semantic_property == "Hero.PersistentId" &&
                field.kind == CampaignDocumentMutationKind::ClearChildren &&
                field.target_path.starts_with(mutation.target_path + "/hero_file_data/raw_data => base_root/")) {
                if (field.target_path.ends_with("/quirks")) added.quirks.clear();
                if (field.target_path.ends_with("/trinkets/items")) added.trinkets.clear();
            }
            model.heroes.push_back(std::move(added));
        }
    }
    return true;
}

bool same_target(const CampaignOperationTarget& lhs, const CampaignOperationTarget& rhs) {
    return lhs.semantic_property == rhs.semantic_property && lhs.entity_id == rhs.entity_id &&
           lhs.occurrence_index == rhs.occurrence_index && lhs.member_id == rhs.member_id;
}

void merge_pending_changes(ChangeSet& pending, const ChangeSet& delta) {
    for (const auto& change : delta.changes) {
        const auto found = std::find_if(pending.changes.begin(), pending.changes.end(), [&](const auto& existing) {
            return same_target(existing.target, change.target);
        });
        if (found == pending.changes.end()) {
            if (change.before != change.after) pending.changes.push_back(change);
        } else {
            found->after = change.after;
            if (found->before == found->after) pending.changes.erase(found);
        }
    }
    for (const auto& change : delta.structural_changes) {
        const auto found = std::find_if(pending.structural_changes.begin(), pending.structural_changes.end(), [&](const auto& existing) {
            return existing.raw.document_id == change.raw.document_id &&
                   existing.raw.display_path == change.raw.display_path;
        });
        if (found != pending.structural_changes.end()) {
            if (found->action != change.action) pending.structural_changes.erase(found);
            else *found = change;
        } else if (change.action == CampaignStructuralAction::Erase) {
            pending.structural_changes.push_back(change);
        }
    }
    for (const auto& batch : delta.document_mutation_batches) {
        const auto found = std::find_if(pending.document_mutation_batches.begin(),
            pending.document_mutation_batches.end(), [&](const auto& existing) {
                return existing.transaction_id == batch.transaction_id;
            });
        if (batch.cancel) {
            if (found != pending.document_mutation_batches.end()) pending.document_mutation_batches.erase(found);
        } else if (found == pending.document_mutation_batches.end()) {
            pending.document_mutation_batches.push_back(batch);
        } else {
            *found = batch;
        }
    }
    if (delta.district_system_snapshot) {
        if (!pending.district_system_snapshot) pending.district_system_snapshot = delta.district_system_snapshot;
        else {
            pending.district_system_snapshot->after_open = delta.district_system_snapshot->after_open;
            pending.district_system_snapshot->after_districts = delta.district_system_snapshot->after_districts;
        }
    }
    if (delta.trinket_inventory_snapshot) {
        if (!pending.trinket_inventory_snapshot) pending.trinket_inventory_snapshot = delta.trinket_inventory_snapshot;
        else pending.trinket_inventory_snapshot->after = delta.trinket_inventory_snapshot->after;
    }
    std::set<std::string, std::less<>> documents;
    for (const auto& change : pending.changes) documents.insert(change.raw.document_id);
    for (const auto& change : pending.structural_changes) documents.insert(change.raw.document_id);
    for (const auto& batch : pending.document_mutation_batches)
        for (const auto& mutation : batch.mutations) documents.insert(mutation.document_id);
    pending.affected_documents.assign(documents.begin(), documents.end());
}

core::Error stale_revision_error(std::uint64_t expected, std::uint64_t actual) {
    return {core::ErrorCode::StaleSessionRevision, "The edit session has changed since this operation was prepared",
            "CampaignEditSession", {{"expected_revision", std::to_string(expected)},
                                     {"actual_revision", std::to_string(actual)}}};
}

core::Error empty_history_error(std::string direction) {
    return {core::ErrorCode::ValidationFailed, "There is no campaign edit to " + direction,
            "CampaignEditSession", {{"action", std::move(direction)}}};
}

std::int32_t tree_hash(std::string_view tree_id) {
    return static_cast<std::int32_t>(core::dson::string_hash(tree_id));
}

std::optional<std::int32_t> hero_purchase_instance(const CampaignModel& model, const domain::Hero& hero) {
    if (!hero.class_id.value) return std::nullopt;
    std::int32_t hero_id{};
    const auto [end, error] = std::from_chars(hero.persistent_id.data(),
        hero.persistent_id.data() + hero.persistent_id.size(), hero_id);
    if (!hero.persistent_id.empty() && error == std::errc{} &&
        end == hero.persistent_id.data() + hero.persistent_id.size() && hero_id > 0)
        return hero_id;
    const auto wanted = tree_hash(*hero.class_id.value + ".weapon");
    std::set<std::int32_t> instances;
    for (const auto& node : model.upgrade_purchase_nodes)
        if (node.tree_id == wanted) instances.insert(node.instance_number);
    if (instances.size() != 1) return std::nullopt;
    return *instances.begin();
}

bool append_purchase_rank_changes(const CampaignModel& model, CompositeCampaignOperation& operation,
                                  std::int32_t instance, std::string_view tree_name,
                                  std::int32_t rank, std::int32_t maximum,
                                  char first_code) {
    if (rank < 0 || maximum <= 0 || rank > maximum || operation.mapped_operation_id.empty()) return false;
    const auto hash = tree_hash(tree_name);
    std::map<char, const domain::UpgradePurchaseNode*> matching;
    std::size_t next_index = 0;
    for (const auto& node : model.upgrade_purchase_nodes)
        next_index = std::max(next_index, node.index + 1);
    for (const auto& mutation : operation.document_mutations) {
        if (mutation.kind != CampaignDocumentMutationKind::AppendClone ||
            mutation.semantic_property != "Upgrade.PurchaseNode.Entry") continue;
        const auto slash = mutation.target_path.find_last_of('/');
        if (slash == std::string::npos) continue;
        std::size_t reserved{};
        const auto key = std::string_view{mutation.target_path}.substr(slash + 1);
        const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), reserved);
        if (error == std::errc{} && end == key.data() + key.size() &&
            reserved < std::numeric_limits<std::size_t>::max())
            next_index = std::max(next_index, reserved + 1);
    }
    const domain::UpgradePurchaseNode* template_node = nullptr;
    for (const auto& node : model.upgrade_purchase_nodes) {
        if (node.instance_number != instance) continue;
        if (template_node == nullptr) template_node = &node;
        if (node.tree_id != hash) continue;
        if (node.requirement_code < first_code || node.requirement_code >= first_code + maximum ||
            !node.is_purchased.value || !node.is_purchased.raw ||
            !matching.emplace(node.requirement_code, &node).second) return false;
    }
    if (template_node == nullptr)
        for (const auto& node : model.upgrade_purchase_nodes)
            if (node.is_purchased.value && !node.row_raw.display_path.empty()) {
                template_node = &node;
                break;
            }
    if (template_node == nullptr || template_node->row_raw.display_path.empty()) return false;

    for (std::int32_t offset = 0; offset < maximum; ++offset) {
        const auto code = static_cast<char>(first_code + offset);
        const bool desired = offset < rank;
        const auto found = matching.find(code);
        if (found != matching.end()) {
            const auto* node = found->second;
            if (*node->is_purchased.value == desired) continue;
            CampaignOperationTarget target{"Upgrade.PurchaseNode", std::to_string(instance), node->index,
                std::to_string(hash) + "|" + node->requirement_code};
            operation.operations.push_back({std::move(target), desired});
            continue;
        }
        if (!desired) continue;
        const auto row_path = "base_root/purchases/" + std::to_string(next_index++);
        auto& mutations = operation.document_mutations;
        mutations.push_back({CampaignDocumentMutationKind::AppendClone, "Upgrade.PurchaseNode.Entry",
            "persist.upgrades.json", row_path, template_node->row_raw.display_path,
            row_path.substr(row_path.find_last_of('/') + 1), core::dson::ValueKind::Object, std::nullopt, std::nullopt});
        const auto append_set = [&](std::string field_name, core::dson::ValueKind kind,
                                    CampaignValue before, CampaignValue after) {
            mutations.push_back({CampaignDocumentMutationKind::SetValue, "Upgrade.PurchaseNode.Entry",
                "persist.upgrades.json", row_path + "/" + field_name, {}, {}, kind,
                std::move(before), std::move(after)});
        };
        append_set("instance_number", core::dson::ValueKind::Integer,
                   template_node->instance_number, instance);
        append_set("tree_id", core::dson::ValueKind::Integer, template_node->tree_id, hash);
        append_set("requirement_code", core::dson::ValueKind::Character,
                   template_node->requirement_code, code);
        append_set("is_purchased", core::dson::ValueKind::Boolean,
                   *template_node->is_purchased.value, true);
    }
    return true;
}

core::Error progression_mapping_error(std::string message, std::string operation) {
    return {core::ErrorCode::MappingNotWritable, std::move(message), "CampaignOperationFactory",
            {{"operation", std::move(operation)}}};
}

} // namespace

bool ValidationReport::valid() const noexcept {
    return std::none_of(issues.begin(), issues.end(), [](const auto& issue) { return issue.blocking; });
}

const std::vector<CampaignOperationCapabilityDescriptor>& campaign_operation_capabilities() {
    static const std::vector<CampaignOperationCapabilityDescriptor> capabilities{
        {"campaign.resource.set_amount", "修改基础资源", CampaignOperationAvailability::Available,
         {"Estate.Resource.Amount"}, "可编辑并通过安全提交写入。"},
        {"campaign.hero.set_resolve_xp", "修改英雄经验", CampaignOperationAvailability::Available,
         {"Hero.ResolveXp"}, "经验可写入；等级阈值与 UI 等级转换由上层规则负责。"},
        {"campaign.hero.rename", "修改英雄名称", CampaignOperationAvailability::Available,
         {"Hero.Name"}, "支持修改现有英雄和本次会话新建英雄的名称。"},
        {"campaign.hero.set_stress", "设置英雄压力值", CampaignOperationAvailability::Available,
         {"Hero.Stress"}, "压力值可设置为任意有限非负数；设为 0 即清空压力。候选写回待游戏内验收。"},
        {"campaign.hero.set_affliction_state", "设置或清除英雄折磨", CampaignOperationAvailability::Available,
         {"Hero.AfflictionId", "Hero.AfflictionSeverity", "Hero.VirtueId"},
         "只支持折磨或非折磨状态；设置折磨时同时清除任务内美德状态。候选写回待游戏内验收。"},
        {"campaign.hero.set_quirk_locked", "锁定正面怪癖", CampaignOperationAvailability::Available,
         {"Hero.Quirk.Locked"}, "要求有效定义标记 can_modify_in_activity。"},
        {"campaign.hero.remove_quirk", "移除怪癖", CampaignOperationAvailability::Available,
         {"Hero.Quirk.Entry"}, "直接移除非疾病怪癖完整记录。"},
        {"campaign.trinket.destroy", "销毁饰品", CampaignOperationAvailability::Available,
         {"Hero.Trinket.Entry", "TrinketInventory.Entry", "TrinketInventory.Items"}, "可分别销毁装备栏或庄园库存条目。"},
        {"campaign.town.set_district_built", "设置小镇建筑状态", CampaignOperationAvailability::Available,
         {"Town.District.Built"}, "要求存档已经包含开放的小镇建筑系统状态。"},
        {"campaign.hero.add", "新增英雄", CampaignOperationAvailability::Available,
         {"Hero.PersistentId"}, "使用程序内置的空白 0 级 DSON 模板，并从当前内容环境初始化职业与技能。"},
        {"campaign.hero.delete", "删除英雄", CampaignOperationAvailability::Available,
         {"Hero.PersistentId"}, "从英雄名单删除完整记录；引用检查由入口执行。"},
        {"campaign.hero.reorder", "调整英雄名单顺序", CampaignOperationAvailability::Available,
         {"Hero.PersistentId"}, "按用户提交的完整英雄 ID 列表重排 DSON 名单。"},
        {"campaign.hero.set_equipment_ranks", "设置武器与防具等级", CampaignOperationAvailability::Available,
         {"Hero.WeaponRank", "Hero.ArmourRank", "Upgrade.PurchaseNode", "Upgrade.PurchaseNode.Entry"}, "武器、防具等级与购买节点在同一复合操作中原子写入；稀疏缺失节点按有效升级树补建。"},
        {"campaign.hero.set_combat_skill_rank", "设置战斗技能等级", CampaignOperationAvailability::Available,
         {"Upgrade.PurchaseNode", "Upgrade.PurchaseNode.Entry"}, "调用方必须传入有效内容环境解析出的技能节点上限。"},
        {"campaign.hero.set_camping_skill_learned", "学习生存技能", CampaignOperationAvailability::Available,
         {"Upgrade.PurchaseNode", "Upgrade.PurchaseNode.Entry"}, "训练营学习状态独立于 selected_camping_skills 装备集合。"},
        {"campaign.hero.maximize_progression", "升满英雄装备与技能", CampaignOperationAvailability::Available,
         {"Hero.WeaponRank", "Hero.ArmourRank", "Upgrade.PurchaseNode", "Upgrade.PurchaseNode.Entry"},
         "在一次操作中升满装备与当前战斗技能，并学习全部生存技能；不改变英雄等级。"},
        {"campaign.hero.set_camping_skill_equipped", "装备或取消装备生存技能", CampaignOperationAvailability::Available,
         {"Hero.SelectedCampingSkills"}, "按已装备技能的先后顺序轮换四个位置，训练解锁状态保持独立。"},
        {"campaign.hero.unequip_camping_skill", "取消装备生存技能", CampaignOperationAvailability::Available,
         {"Hero.SelectedCampingSkills"}, "从已装备集合移除技能，不影响训练营解锁状态。"},
        {"campaign.hero.lock_camping_skill", "锁定生存技能", CampaignOperationAvailability::Deferred,
         {}, "游戏内验证未通过，暂缓。"},
        {"campaign.hero.edit_disease", "编辑疾病", CampaignOperationAvailability::Deferred,
         {}, "等待包含真实疾病记录的存档样本。"},
        {"campaign.hero.add_or_replace_quirk", "增加或替换怪癖", CampaignOperationAvailability::Available,
         {"Hero.Quirks"}, "先校验有效定义的怪癖极性并初始化元数据；负面替换删除旧记录后克隆新增到原序位，并清除替换索引。"},
        {"campaign.trinket.add_inventory", "增加库存饰品", CampaignOperationAvailability::Available,
         {"TrinketInventory.Items"}, "按有效饰品定义追加一个库存条目，不对重复 ID 去重。"},
        {"campaign.trinket.reorder_inventory", "重排饰品库存", CampaignOperationAvailability::Available,
         {"TrinketInventory.Items"}, "仅重排存档饰品箱的有序条目，不更改饰品内容。"},
        {"campaign.hero.equip_trinket", "装备饰品", CampaignOperationAvailability::Available,
         {"Hero.Trinkets"}, "通过有效饰品定义检查职业限制后追加装备记录。"},
        {"campaign.town.set_upgrade_rank", "设置小镇升级进度", CampaignOperationAvailability::Available,
         {"Upgrade.PurchaseNode", "Upgrade.PurchaseNode.Entry"}, "按有效升级上限同步购买与锁定节点。"},
        {"campaign.town.set_district_system_open", "开放或锁定小镇建筑系统", CampaignOperationAvailability::Available,
         {"Town.DistrictSystem"}, "开放时只为有效内容环境中的 district 定义生成 built=false 状态对象。"},
    };
    return capabilities;
}

ValidationReport CampaignOperationValidator::validate(const CampaignModel& model,
                                                       const CampaignOperation& operation) const {
    ValidationReport report;
    if (const auto* district_system = std::get_if<SetDistrictSystemOperation>(&operation)) {
        if (district_system->open == model.district_system_open) {
            add_issue(report, ValidationSeverity::Error, "district.system_unchanged",
                      "The district system already has the requested state", {}, true);
            return report;
        }
        if (district_system->open && district_system->district_ids.empty()) {
            add_issue(report, ValidationSeverity::Error, "district.definitions_missing",
                      "No official district definitions are available", {}, true);
            return report;
        }
        ApplyCampaignDocumentMutationsOperation mapped{
            "campaign.town.set_district_system_open", district_system->mutations};
        return validate(model, CampaignOperation{std::move(mapped)});
    }
    if (const auto* composite = std::get_if<CompositeCampaignOperation>(&operation);
        composite && !composite->document_mutations.empty()) {
        if (composite->mapped_operation_id.empty()) {
            add_issue(report, ValidationSeverity::Error, "mutation.operation_id_missing",
                      "A composite structural mutation must declare its verified business operation", {}, true);
            return report;
        }
        ApplyCampaignDocumentMutationsOperation mapped{composite->mapped_operation_id,
                                                       composite->document_mutations};
        auto structural_report = validate(model, CampaignOperation{std::move(mapped)});
        report.issues.insert(report.issues.end(), structural_report.issues.begin(), structural_report.issues.end());
        if (!report.valid()) return report;
    }
    if (const auto* document_operation = std::get_if<ApplyCampaignDocumentMutationsOperation>(&operation)) {
        const auto capability = std::find_if(campaign_operation_capabilities().begin(),
            campaign_operation_capabilities().end(), [&](const auto& item) {
                return item.operation_id == document_operation->operation_id;
            });
        CampaignOperationTarget summary_target{document_operation->operation_id};
        if (capability == campaign_operation_capabilities().end() ||
            capability->availability != CampaignOperationAvailability::Available) {
            add_issue(report, ValidationSeverity::Error, "operation.not_available",
                      "This document operation has not passed the in-game validation gate", summary_target, true);
            return report;
        }
        if (document_operation->mutations.empty()) {
            add_issue(report, ValidationSeverity::Error, "operation.empty",
                      "The mapped document operation contains no DSON mutations", summary_target, true);
            return report;
        }
        if (document_operation->operation_id == "campaign.hero.set_camping_skill_equipped") {
            const auto& mutations = document_operation->mutations;
            const bool valid_shape = mutations.size() <= 2 &&
                mutations.front().semantic_property == "Hero.SelectedCampingSkills" &&
                (mutations.front().kind == CampaignDocumentMutationKind::AppendClone ||
                 (mutations.size() == 1 && mutations.front().kind == CampaignDocumentMutationKind::Erase)) &&
                (mutations.size() == 1 ||
                 (mutations[1].semantic_property == "Hero.SelectedCampingSkills" &&
                  mutations[1].kind == CampaignDocumentMutationKind::Erase));
            if (!valid_shape) {
                add_issue(report, ValidationSeverity::Error, "camping.equipment_shape_invalid",
                          "Camping skill equipment changes must append one skill and optionally remove the oldest",
                          CampaignOperationTarget{"Hero.SelectedCampingSkills"}, true);
                return report;
            }
        }
        for (const auto& mutation : document_operation->mutations) {
            CampaignOperationTarget target{mutation.semantic_property};
            if (document_operation->operation_id == "campaign.hero.add" &&
                (mutation.kind != CampaignDocumentMutationKind::AppendTemplate ||
                 mutation.semantic_property != "Hero.PersistentId")) {
                add_issue(report, ValidationSeverity::Error, "mutation.hero_add_shape_invalid",
                          "Hero creation only accepts built-in template appends", target, true);
                continue;
            }
            const auto* mapping = find_mapping(mutation.semantic_property);
            if (mapping == nullptr || mapping->capability() != CampaignMappingCapability::CommitWritable ||
                mapping->document_id != mutation.document_id || mutation.target_path.empty() ||
                !path_is_within_mapping(*mapping, mutation.target_path)) {
                add_issue(report, ValidationSeverity::Error, "mutation.mapping_mismatch",
                          "A DSON mutation is outside its game-verified mapping", target, true);
                continue;
            }
            const bool district_template_source = mutation.semantic_property == "Town.DistrictSystem" &&
                mutation.kind == CampaignDocumentMutationKind::AppendClone &&
                (mutation.source_path.starts_with("base_root/buildings/") ||
                 mutation.source_path.starts_with("base_root/"));
            const bool built_in_hero_template = document_operation->operation_id == "campaign.hero.add" &&
                mutation.kind == CampaignDocumentMutationKind::AppendTemplate &&
                mutation.semantic_property == "Hero.PersistentId" && mutation.document_id == "persist.roster.json" &&
                mutation.source_path == "base_root/heroes/1" && mutation.template_document != nullptr &&
                mutation.expected_kind == core::dson::ValueKind::Object && safe_dson_key(mutation.new_key) &&
                mutation.target_path == "base_root/heroes/" + mutation.new_key &&
                safe_dson_key(mutation.template_hero_class) && !mutation.template_combat_skills.empty() &&
                mutation.template_base_hit_points > 0.0F;
            if ((mutation.kind == CampaignDocumentMutationKind::AppendClone ||
                 mutation.kind == CampaignDocumentMutationKind::InsertClone) &&
                !path_is_within_mapping(*mapping, mutation.source_path) && !district_template_source &&
                !camping_scalar_clone_source_allowed(mutation)) {
                add_issue(report, ValidationSeverity::Error, "mutation.source_mapping_mismatch",
                          "A cloned DSON template must come from the same registered mapping", target, true);
                continue;
            }
            const bool kind_allowed =
                (mutation.kind == CampaignDocumentMutationKind::AppendTemplate && built_in_hero_template) ||
                (mutation.kind == CampaignDocumentMutationKind::AppendClone &&
                 (mutation.semantic_property == "Hero.PersistentId" || mutation.semantic_property == "Hero.Quirks" ||
                  mutation.semantic_property == "Hero.Trinkets" || mutation.semantic_property == "TrinketInventory.Items" ||
                  mutation.semantic_property == "Town.DistrictSystem" || mutation.semantic_property == "Town.Districts" ||
                  mutation.semantic_property == "Upgrade.PurchaseNode.Entry" ||
                  mutation.semantic_property == "Hero.SelectedCampingSkills")) ||
                (mutation.kind == CampaignDocumentMutationKind::CreateObject &&
                 mutation.semantic_property == "TrinketInventory.Items") ||
                (mutation.kind == CampaignDocumentMutationKind::InsertClone &&
                 mutation.semantic_property == "Hero.Quirks") ||
                (mutation.kind == CampaignDocumentMutationKind::Erase &&
                 (mutation.semantic_property == "Hero.PersistentId" || mutation.semantic_property == "Hero.Quirks" ||
                  mutation.semantic_property == "Hero.Trinkets" || mutation.semantic_property == "TrinketInventory.Items" ||
                  mutation.semantic_property == "Town.DistrictSystem" ||
                  mutation.semantic_property == "Hero.SelectedCampingSkills")) ||
                (mutation.kind == CampaignDocumentMutationKind::Rename &&
                 (mutation.semantic_property == "Hero.PersistentId" || mutation.semantic_property == "Hero.Quirks" ||
                  mutation.semantic_property == "TrinketInventory.Items")) ||
                (mutation.kind == CampaignDocumentMutationKind::ClearChildren &&
                 (mutation.semantic_property == "Hero.PersistentId" || mutation.semantic_property == "Town.DistrictSystem" ||
                  mutation.semantic_property == "Town.Districts")) ||
                (mutation.kind == CampaignDocumentMutationKind::SetValue &&
                 (mutation.semantic_property == "Hero.PersistentId" || mutation.semantic_property == "Hero.Quirks" ||
                  mutation.semantic_property == "Hero.Trinkets" || mutation.semantic_property == "TrinketInventory.Items" ||
                  mutation.semantic_property == "Town.DistrictSystem" || mutation.semantic_property == "Town.Districts" ||
                  mutation.semantic_property == "Upgrade.PurchaseNode.Entry"));
            if (!kind_allowed) {
                add_issue(report, ValidationSeverity::Error, "mutation.action_not_allowed",
                          "The requested structural action is not allowed for this mapped collection", target, true);
                continue;
            }
            if (mutation.semantic_property == "Upgrade.PurchaseNode.Entry") {
                constexpr std::string_view prefix{"base_root/purchases/"};
                const auto tail = mutation.target_path.starts_with(prefix)
                    ? std::string_view{mutation.target_path}.substr(prefix.size()) : std::string_view{};
                const auto separator = tail.find('/');
                const auto key = tail.substr(0, separator);
                std::size_t parsed_key{};
                const auto [key_end, key_error] = std::from_chars(key.data(), key.data() + key.size(), parsed_key);
                const bool numeric_key = !key.empty() && key_error == std::errc{} && key_end == key.data() + key.size();
                bool numeric_source = false;
                if (mutation.source_path.starts_with(prefix)) {
                    const auto source_key = std::string_view{mutation.source_path}.substr(prefix.size());
                    std::size_t source_index{};
                    const auto [source_end, source_error] = std::from_chars(
                        source_key.data(), source_key.data() + source_key.size(), source_index);
                    numeric_source = !source_key.empty() && source_key.find('/') == std::string_view::npos &&
                        source_error == std::errc{} && source_end == source_key.data() + source_key.size();
                }
                const bool append_row = mutation.kind == CampaignDocumentMutationKind::AppendClone &&
                    numeric_key && separator == std::string_view::npos && mutation.new_key == key &&
                    numeric_source && mutation.expected_kind == core::dson::ValueKind::Object;
                const auto field = separator == std::string_view::npos ? std::string_view{} : tail.substr(separator + 1);
                const bool set_row_field = mutation.kind == CampaignDocumentMutationKind::SetValue && numeric_key &&
                    (field == "instance_number" || field == "tree_id" || field == "requirement_code" ||
                     field == "is_purchased");
                if (!append_row && !set_row_field) {
                    add_issue(report, ValidationSeverity::Error, "mutation.purchase_node_shape_invalid",
                              "Purchase-node mutations may append one numeric row or set its four typed identity/state fields",
                              target, true);
                    continue;
                }
                if (set_row_field) {
                    const auto expected = field == "requirement_code" ? core::dson::ValueKind::Character :
                        field == "is_purchased" ? core::dson::ValueKind::Boolean : core::dson::ValueKind::Integer;
                    if (mutation.expected_kind != expected) {
                        add_issue(report, ValidationSeverity::Error, "mutation.purchase_node_type_invalid",
                                  "Purchase-node fields require their registered DSON scalar type", target, true);
                        continue;
                    }
                }
            }
            if (((mutation.kind == CampaignDocumentMutationKind::AppendClone ||
                  mutation.kind == CampaignDocumentMutationKind::InsertClone) &&
                 (mutation.source_path.empty() || !safe_dson_key(mutation.new_key) ||
                  mutation.expected_kind == core::dson::ValueKind::Unknown ||
                  (mutation.kind == CampaignDocumentMutationKind::InsertClone && !mutation.insertion_index))) ||
                (mutation.kind == CampaignDocumentMutationKind::Rename && !safe_dson_key(mutation.new_key)) ||
                (mutation.kind == CampaignDocumentMutationKind::CreateObject &&
                 (!safe_dson_key(mutation.new_key) || mutation.expected_kind != core::dson::ValueKind::Object ||
                  mutation.document_id != "persist.estate.json" ||
                  mutation.target_path != "base_root/trinkets/items/" + mutation.new_key)) ||
                (mutation.kind == CampaignDocumentMutationKind::SetValue &&
                 (!mutation.before || !mutation.after ||
                  !campaign_value_matches_kind(mutation.expected_kind, *mutation.before) ||
                  !campaign_value_matches_kind(mutation.expected_kind, *mutation.after))) ||
                (mutation.kind == CampaignDocumentMutationKind::Erase &&
                 mutation.expected_kind == core::dson::ValueKind::Unknown) ||
                (mutation.kind == CampaignDocumentMutationKind::ClearChildren &&
                 mutation.expected_kind != core::dson::ValueKind::Object) ||
                (mutation.kind == CampaignDocumentMutationKind::AppendTemplate && !built_in_hero_template)) {
                add_issue(report, ValidationSeverity::Error, "mutation.payload_invalid",
                          "The structural mutation is missing a required typed payload", target, true);
            }
        }
        return report;
    }
    const auto is_equipment_rank = [](const SetCampaignValueOperation& edit) {
        return edit.target.semantic_property == "Hero.WeaponRank" ||
               edit.target.semantic_property == "Hero.ArmourRank";
    };
    if (const auto* single = std::get_if<SetCampaignValueOperation>(&operation); single && is_equipment_rank(*single)) {
        add_issue(report, ValidationSeverity::Error, "equipment.rank_bundle_required",
                  "Weapon and armour ranks must be changed with their purchase-history nodes in one composite operation",
                  single->target, true);
        return report;
    }
    if (const auto* composite = std::get_if<CompositeCampaignOperation>(&operation)) {
        const SetCampaignValueOperation* weapon = nullptr;
        const SetCampaignValueOperation* armour = nullptr;
        for (const auto& edit : composite->operations) {
            if (edit.target.semantic_property == "Hero.WeaponRank") weapon = &edit;
            if (edit.target.semantic_property == "Hero.ArmourRank") armour = &edit;
        }
        if (weapon || armour) {
            const auto* rank_target = weapon ? weapon : armour;
            bool valid_bundle = weapon && armour && weapon->target.entity_id == armour->target.entity_id;
            const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
                return rank_target && item.persistent_id == rank_target->target.entity_id;
            });
            const auto instance = hero == model.heroes.end() ? std::nullopt : hero_purchase_instance(model, *hero);
            bool has_weapon_purchase_mapping = false;
            bool has_armour_purchase_mapping = false;
            if (instance && hero != model.heroes.end() && hero->class_id.value) {
                const auto weapon_tree = tree_hash(*hero->class_id.value + ".weapon");
                const auto armour_tree = tree_hash(*hero->class_id.value + ".armour");
                const auto mark_tree = [&](std::int32_t tree) {
                    if (tree == weapon_tree) has_weapon_purchase_mapping = true;
                    if (tree == armour_tree) has_armour_purchase_mapping = true;
                };
                for (const auto& edit : composite->operations) {
                    if (edit.target.semantic_property != "Upgrade.PurchaseNode" ||
                        edit.target.entity_id != std::to_string(*instance) || edit.target.member_id.empty()) continue;
                    const auto separator = edit.target.member_id.find('|');
                    if (separator == std::string::npos) continue;
                    const auto hash_text = std::string_view{edit.target.member_id}.substr(0, separator);
                    std::int32_t hash{};
                    const auto [end, error] = std::from_chars(hash_text.data(), hash_text.data() + hash_text.size(), hash);
                    if (error == std::errc{} && end == hash_text.data() + hash_text.size()) mark_tree(hash);
                }
                for (const auto& append : composite->document_mutations) {
                    if (append.kind != CampaignDocumentMutationKind::AppendClone ||
                        append.semantic_property != "Upgrade.PurchaseNode.Entry") continue;
                    std::optional<std::int32_t> row_instance;
                    std::optional<std::int32_t> row_tree;
                    for (const auto& field : composite->document_mutations) {
                        if (field.kind != CampaignDocumentMutationKind::SetValue || !field.after ||
                            !field.target_path.starts_with(append.target_path + "/")) continue;
                        const auto name = std::string_view{field.target_path}.substr(append.target_path.size() + 1);
                        if (name == "instance_number") {
                            if (const auto* value = std::get_if<std::int32_t>(&*field.after)) row_instance = *value;
                        } else if (name == "tree_id") {
                            if (const auto* value = std::get_if<std::int32_t>(&*field.after)) row_tree = *value;
                        }
                    }
                    if (row_instance == instance && row_tree) mark_tree(*row_tree);
                }
            }
            const auto rank_changed = [&](const SetCampaignValueOperation* edit,
                                          const domain::LocatedValue<std::int32_t>& current) {
                const auto* requested = edit ? std::get_if<std::int32_t>(&edit->value) : nullptr;
                return requested != nullptr && current.value && *requested != *current.value;
            };
            const bool weapon_changed = hero != model.heroes.end() && rank_changed(weapon, hero->weapon_rank);
            const bool armour_changed = hero != model.heroes.end() && rank_changed(armour, hero->armour_rank);
            valid_bundle = valid_bundle && (!weapon_changed || has_weapon_purchase_mapping) &&
                           (!armour_changed || has_armour_purchase_mapping);
            if (!valid_bundle) {
                const auto target = rank_target ? rank_target->target : CampaignOperationTarget{"Hero.WeaponRank"};
                add_issue(report, ValidationSeverity::Error, "equipment.rank_bundle_required",
                          "Weapon and armour ranks must be changed for one hero together with mapped purchase-history nodes",
                          target, true);
                return report;
            }
        }
    }
    if (const auto* state = std::get_if<SetHeroAfflictionStateOperation>(&operation)) {
        const CampaignOperationTarget target{"Hero.AfflictionId", state->hero_id};
        const bool afflicted = state->state == HeroAfflictionState::Afflicted;
        if (afflicted && !safe_dson_key(state->affliction_id)) {
            add_issue(report, ValidationSeverity::Error, "affliction_state.invalid_id",
                      "An afflicted state requires a safe, non-empty affliction identifier", target, true);
            return report;
        }
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
            return item.persistent_id == state->hero_id;
        });
        if (hero == model.heroes.end() || !hero->affliction_id.raw ||
            !hero->affliction_severity.raw || !hero->virtue_id.raw) {
            add_issue(report, ValidationSeverity::Error, "affliction_state.mapping_missing",
                      "The hero must have serialized affliction and virtue fields available", target, true);
            return report;
        }
        if (hero->state == EntityState::Invalid || hero->state == EntityState::Unresolved) {
            add_issue(report, ValidationSeverity::Error, "affliction_state.hero_unavailable",
                      "The target hero is invalid or unresolved", target, true);
            return report;
        }
    }

    const auto operations = flatten(operation);
    if (!operations.empty()) {
        const bool affliction_state_operation = std::holds_alternative<SetHeroAfflictionStateOperation>(operation);
        for (const auto& edit : operations) validate_one(model, edit, report, affliction_state_operation);
        return report;
    }

    if (const auto* remove = std::get_if<RemoveHeroQuirkOperation>(&operation)) {
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
            return item.persistent_id == remove->hero_id;
        });
        const auto count = hero == model.heroes.end() ? 0 : std::count_if(hero->quirks.begin(), hero->quirks.end(), [&](const auto& item) {
            return item.id == remove->quirk_id && !item.is_disease;
        });
        CampaignOperationTarget target{"Hero.Quirk.Entry", remove->hero_id, std::nullopt, remove->quirk_id};
        if (find_mapping(target.semantic_property) == nullptr || !find_mapping(target.semantic_property)->semantically_writable)
            add_issue(report, ValidationSeverity::Error, "mapping.not_writable", "Quirk removal has no writable mapping", target, true);
        else if (hero == model.heroes.end() || count != 1)
            add_issue(report, ValidationSeverity::Error, "target.missing", "The requested non-disease quirk is not uniquely present", target, true);
        else if (hero->state == EntityState::Invalid || hero->state == EntityState::Unresolved)
            add_issue(report, ValidationSeverity::Error, "target.unavailable", "The target hero is invalid or unresolved", target, true);
        return report;
    }

    if (const auto* unequip = std::get_if<UnequipHeroCampingSkillOperation>(&operation)) {
        CampaignOperationTarget target{"Hero.SelectedCampingSkills", unequip->hero_id,
                                       std::nullopt, unequip->skill_id};
        const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
            return item.persistent_id == unequip->hero_id;
        });
        const auto count = hero == model.heroes.end() ? 0 : std::count_if(
            hero->camping_skills.begin(), hero->camping_skills.end(), [&](const auto& item) {
                return item.camping && item.id == unequip->skill_id;
            });
        const auto* mapping = find_mapping(target.semantic_property);
        if (mapping == nullptr || !mapping->semantically_writable || !mapping->game_mutation_verified)
            add_issue(report, ValidationSeverity::Error, "mapping.not_writable",
                      "Camping skill unequip has no game-verified mapping", target, true);
        else if (count != 1)
            add_issue(report, ValidationSeverity::Error, "target.missing",
                      "The requested camping skill is not uniquely equipped on this hero", target, true);
        return report;
    }

    if (const auto* destroy = std::get_if<DestroyTrinketOperation>(&operation)) {
        const auto semantic = destroy->hero_id.empty() ? "TrinketInventory.Entry" : "Hero.Trinket.Entry";
        CampaignOperationTarget target{semantic, destroy->hero_id, std::nullopt, destroy->item_key};
        const auto* mapping = find_mapping(semantic);
        if (mapping == nullptr || !mapping->semantically_writable) {
            add_issue(report, ValidationSeverity::Error, "mapping.not_writable", "Trinket destruction has no writable mapping", target, true);
        } else if (destroy->hero_id.empty()) {
            const auto count = std::count_if(model.trinket_inventory.begin(), model.trinket_inventory.end(), [&](const auto& item) {
                return item.raw_key == destroy->item_key || item.raw.display_path == destroy->item_key;
            });
            if (count != 1) add_issue(report, ValidationSeverity::Error, "target.missing", "The inventory trinket key is not unique", target, true);
        } else {
            const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
                return item.persistent_id == destroy->hero_id;
            });
            const auto count = hero == model.heroes.end() ? 0 : std::count_if(hero->trinkets.begin(), hero->trinkets.end(), [&](const auto& item) {
                return item.raw.display_path == destroy->item_key;
            });
            if (hero == model.heroes.end() || count != 1)
                add_issue(report, ValidationSeverity::Error, "target.missing", "The equipped trinket path is not unique on the hero", target, true);
        }
        return report;
    }

    if (const auto* composite = std::get_if<CompositeCampaignOperation>(&operation);
        composite && composite->operations.empty() && !composite->document_mutations.empty())
        return report;
    if (operations.empty()) {
        report.issues.push_back({ValidationSeverity::Error, "operation.empty",
            "A campaign operation must contain at least one field edit", {}, {}, true});
        return report;
    }
    return report;
}

RiskAssessment CampaignOperationRiskAssessor::assess(const CampaignOperation& operation) const {
    RiskAssessment result{CampaignRiskLevel::Low, {}};
    for (const auto& edit : flatten(operation)) {
        const auto* mapping = find_mapping(edit.target.semantic_property);
        if (mapping == nullptr) {
            result.level = CampaignRiskLevel::High;
            result.reasons.push_back("No mapping exists for " + edit.target.semantic_property + ".");
            continue;
        }
        if (!mapping->game_mutation_verified) {
            if (result.level < CampaignRiskLevel::Moderate) result.level = CampaignRiskLevel::Moderate;
            result.reasons.push_back(edit.target.semantic_property + " has no in-game mutation evidence; this session only stages an in-memory change.");
        }
        if (edit.target.semantic_property == "Hero.ResolveXp" ||
            edit.target.semantic_property == "Hero.WeaponRank" ||
            edit.target.semantic_property == "Hero.ArmourRank") {
            result.level = CampaignRiskLevel::High;
            result.reasons.push_back(edit.target.semantic_property +
                " changes progression state and depends on its corresponding game purchase or level rules.");
        }
    }
    if (std::holds_alternative<RemoveHeroQuirkOperation>(operation) ||
        std::holds_alternative<UnequipHeroCampingSkillOperation>(operation) ||
        std::holds_alternative<DestroyTrinketOperation>(operation)) {
        result.level = std::max(result.level, CampaignRiskLevel::Moderate);
        result.reasons.push_back("This operation destroys a complete collection entry and can only be restored after commit from the verified backup.");
    }
    if (std::holds_alternative<ApplyCampaignDocumentMutationsOperation>(operation) ||
        (std::get_if<CompositeCampaignOperation>(&operation) &&
         !std::get<CompositeCampaignOperation>(operation).document_mutations.empty())) {
        result.level = std::max(result.level, CampaignRiskLevel::Moderate);
        result.reasons.push_back("This operation changes a mapped collection structure; review its preview before commit.");
    }
    std::sort(result.reasons.begin(), result.reasons.end());
    result.reasons.erase(std::unique(result.reasons.begin(), result.reasons.end()), result.reasons.end());
    return result;
}

core::Result<CampaignOperation, core::Error>
make_set_hero_equipment_ranks_operation(const domain::CampaignModel& model, std::string_view hero_id,
                                       std::int32_t weapon_rank, std::int32_t armour_rank,
                                       std::int32_t effective_weapon_max_rank,
                                       std::int32_t effective_armour_max_rank) {
    const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
        return item.persistent_id == hero_id;
    });
    if (hero == model.heroes.end() || !hero->class_id.value || !hero->weapon_rank.raw || !hero->armour_rank.raw)
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Hero class and both mapped equipment rank fields must be available",
                                      "campaign.hero.set_equipment_ranks"));
    const auto instance = hero_purchase_instance(model, *hero);
    if (!instance)
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Hero purchase instance is not uniquely mapped",
                                      "campaign.hero.set_equipment_ranks"));

    CompositeCampaignOperation operation{"Set hero equipment ranks", {}, {}, {}};
    operation.mapped_operation_id = "campaign.hero.set_equipment_ranks";
    operation.operations.push_back({{"Hero.WeaponRank", hero->persistent_id}, weapon_rank});
    operation.operations.push_back({{"Hero.ArmourRank", hero->persistent_id}, armour_rank});
    const auto weapon_tree = *hero->class_id.value + ".weapon";
    const auto armour_tree = *hero->class_id.value + ".armour";
    const auto weapon_max = effective_weapon_max_rank;
    const auto armour_max = effective_armour_max_rank;
    const auto weapon_mapped = append_purchase_rank_changes(model, operation, *instance, weapon_tree,
                                                              weapon_rank, weapon_max, '0');
    const auto armour_mapped = append_purchase_rank_changes(model, operation, *instance, armour_tree,
                                                              armour_rank, armour_max, '0');
    if (!weapon_mapped || !armour_mapped)
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Equipment rank exceeds or does not match mapped purchase nodes (class=" +
                                      *hero->class_id.value + ", instance=" + std::to_string(*instance) +
                                      ", weapon=" + std::to_string(weapon_rank) + "/" + std::to_string(weapon_max) +
                                      (weapon_mapped ? " mapped" : " unmapped") + ", armour=" +
                                      std::to_string(armour_rank) + "/" + std::to_string(armour_max) +
                                      (armour_mapped ? " mapped" : " unmapped") + ")",
                                      "campaign.hero.set_equipment_ranks"));
    return core::Result<CampaignOperation, core::Error>::success(std::move(operation));
}

core::Result<CampaignOperation, core::Error>
make_set_hero_combat_skill_rank_operation(const domain::CampaignModel& model, std::string_view hero_id,
                                          std::string_view skill_id, std::int32_t rank,
                                          std::int32_t effective_max_rank) {
    const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
        return item.persistent_id == hero_id;
    });
    if (hero == model.heroes.end() || !hero->class_id.value)
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Hero class is unavailable", "campaign.hero.set_combat_skill_rank"));
    const auto instance = hero_purchase_instance(model, *hero);
    if (!instance)
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Hero purchase instance is not uniquely mapped",
                                      "campaign.hero.set_combat_skill_rank"));
    const auto separator = skill_id.find(':');
    const auto suffix = separator == std::string_view::npos ? skill_id : skill_id.substr(separator + 1);
    const auto tree = *hero->class_id.value + "." + std::string{suffix};
    CompositeCampaignOperation operation{"Set combat skill rank", {}, {}, {}};
    operation.mapped_operation_id = "campaign.hero.set_combat_skill_rank";
    if (!append_purchase_rank_changes(model, operation, *instance, tree, rank, effective_max_rank, '0') ||
        (operation.operations.empty() && operation.document_mutations.empty()))
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Combat skill rank does not match its effective mapped upgrade tree",
                                      "campaign.hero.set_combat_skill_rank"));
    return core::Result<CampaignOperation, core::Error>::success(std::move(operation));
}

core::Result<CampaignOperation, core::Error>
make_set_hero_camping_skill_learned_operation(const domain::CampaignModel& model, std::string_view hero_id,
                                              std::string_view skill_id, bool learned) {
    const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
        return item.persistent_id == hero_id;
    });
    if (hero == model.heroes.end() || !hero->class_id.value)
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Hero class is unavailable", "campaign.hero.set_camping_skill_learned"));
    const auto instance = hero_purchase_instance(model, *hero);
    if (!instance)
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Hero purchase instance is not uniquely mapped",
                                      "campaign.hero.set_camping_skill_learned"));
    const auto tree = *hero->class_id.value + "." + std::string{skill_id};
    CompositeCampaignOperation operation{"Set camping skill learned state", {}, {}, {}};
    operation.mapped_operation_id = "campaign.hero.set_camping_skill_learned";
    if (!append_purchase_rank_changes(model, operation, *instance, tree, learned ? 1 : 0, 1, '0') ||
        (operation.operations.empty() && operation.document_mutations.empty()))
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Camping skill is not represented by a unique training node",
                                      "campaign.hero.set_camping_skill_learned"));
    return core::Result<CampaignOperation, core::Error>::success(std::move(operation));
}

core::Result<CampaignOperation, core::Error>
make_maximize_hero_progression_operation(
    const domain::CampaignModel& model, std::string_view hero_id,
    std::int32_t weapon_max_rank, std::int32_t armour_max_rank,
    const std::vector<std::pair<std::string, std::int32_t>>& combat_skill_max_ranks,
    const std::vector<std::string>& camping_skill_ids) {
    const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
        return item.persistent_id == hero_id;
    });
    if (hero == model.heroes.end() || !hero->class_id.value || !hero->weapon_rank.raw ||
        !hero->armour_rank.raw || weapon_max_rank <= 0 || armour_max_rank <= 0)
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Hero equipment or effective upgrade limits are unavailable",
                                      "campaign.hero.maximize_progression"));
    const auto instance = hero_purchase_instance(model, *hero);
    if (!instance)
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Hero purchase instance is unavailable", "campaign.hero.maximize_progression"));
    CompositeCampaignOperation operation{"Maximize hero equipment and skills", {}, {}, {}};
    operation.mapped_operation_id = "campaign.hero.maximize_progression";
    operation.operations.push_back({{"Hero.WeaponRank", hero->persistent_id}, weapon_max_rank});
    operation.operations.push_back({{"Hero.ArmourRank", hero->persistent_id}, armour_max_rank});
    const auto class_id = *hero->class_id.value;
    const auto append_max = [&](std::string_view suffix, std::int32_t rank) {
        return append_purchase_rank_changes(model, operation, *instance,
            class_id + "." + std::string{suffix}, rank, rank, '0');
    };
    if (!append_max("weapon", weapon_max_rank) || !append_max("armour", armour_max_rank))
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Equipment purchase nodes cannot be mapped", "campaign.hero.maximize_progression"));
    std::set<std::string, std::less<>> combat_ids;
    for (const auto& [skill_id, maximum] : combat_skill_max_ranks) {
        const auto separator = skill_id.find(':');
        const auto suffix = separator == std::string::npos ? skill_id : skill_id.substr(separator + 1);
        if (!combat_ids.insert(suffix).second || maximum <= 0 ||
            !append_purchase_rank_changes(model, operation, *instance, class_id + "." + suffix,
                                          maximum, maximum, '0'))
            return core::Result<CampaignOperation, core::Error>::failure(
                progression_mapping_error("A combat skill has no valid effective upgrade tree: " + skill_id,
                                          "campaign.hero.maximize_progression"));
    }
    std::set<std::string, std::less<>> camping_ids;
    for (const auto& skill_id : camping_skill_ids) {
        if (!camping_ids.insert(skill_id).second ||
            !append_purchase_rank_changes(model, operation, *instance, class_id + "." + skill_id,
                                          1, 1, '0'))
            return core::Result<CampaignOperation, core::Error>::failure(
                progression_mapping_error("A camping skill cannot be learned: " + skill_id,
                                          "campaign.hero.maximize_progression"));
    }
    return core::Result<CampaignOperation, core::Error>::success(std::move(operation));
}

core::Result<CampaignOperation, core::Error>
make_set_hero_camping_skill_equipped_operation(const domain::CampaignModel& model,
                                               std::string_view hero_id,
                                               std::string_view skill_id,
                                               bool equipped) {
    const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
        return item.persistent_id == hero_id;
    });
    if (hero == model.heroes.end() || skill_id.empty())
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Hero or camping skill is unavailable", "campaign.hero.set_camping_skill_equipped"));
    const auto selected = std::find_if(hero->camping_skills.begin(), hero->camping_skills.end(),
        [&](const auto& item) { return item.id == skill_id; });
    if (equipped == (selected != hero->camping_skills.end()))
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Camping skill equipment state is already current",
                                      "campaign.hero.set_camping_skill_equipped"));
    std::vector<CampaignDocumentMutation> mutations;
    if (equipped) {
        std::string source;
        if (!hero->camping_skills.empty()) source = hero->camping_skills.front().raw.display_path;
        else for (const auto& other : model.heroes)
            if (!other.camping_skills.empty()) {
                source = other.camping_skills.front().raw.display_path;
                break;
            }
        if (source.empty() && !hero->combat_skills.empty())
            source = hero->combat_skills.front().raw.display_path;
        if (source.empty())
            return core::Result<CampaignOperation, core::Error>::failure(
                progression_mapping_error("No mapped skill entry is available as a scalar template",
                                          "campaign.hero.set_camping_skill_equipped"));
        const auto target = hero->raw.display_path +
            "/hero_file_data/raw_data => base_root/skills/selected_camping_skills/" + std::string{skill_id};
        mutations.push_back({CampaignDocumentMutationKind::AppendClone, "Hero.SelectedCampingSkills",
            "persist.roster.json", target, source, std::string{skill_id}, core::dson::ValueKind::Integer});
        if (hero->camping_skills.size() >= 4) {
            mutations.push_back({CampaignDocumentMutationKind::Erase, "Hero.SelectedCampingSkills",
                "persist.roster.json", hero->camping_skills.front().raw.display_path, {}, {},
                core::dson::ValueKind::Integer});
        }
    } else {
        mutations.push_back({CampaignDocumentMutationKind::Erase, "Hero.SelectedCampingSkills",
            "persist.roster.json", selected->raw.display_path, {}, {}, core::dson::ValueKind::Integer});
    }
    return core::Result<CampaignOperation, core::Error>::success(
        ApplyCampaignDocumentMutationsOperation{"campaign.hero.set_camping_skill_equipped", std::move(mutations)});
}

core::Result<CampaignOperation, core::Error>
make_set_town_upgrade_rank_operation(const domain::CampaignModel& model, std::string_view tree_id,
                                     std::int32_t rank, std::int32_t effective_max_rank) {
    CompositeCampaignOperation operation{"Set town upgrade rank", {}, {}, {}};
    operation.mapped_operation_id = "campaign.town.set_upgrade_rank";
    if (!append_purchase_rank_changes(model, operation, 0, tree_id, rank, effective_max_rank, 'a') ||
        (operation.operations.empty() && operation.document_mutations.empty()))
        return core::Result<CampaignOperation, core::Error>::failure(
            progression_mapping_error("Town upgrade rank does not match its effective mapped purchase tree",
                                      "campaign.town.set_upgrade_rank"));
    return core::Result<CampaignOperation, core::Error>::success(std::move(operation));
}

CampaignEditSession::CampaignEditSession(CampaignModel initial_model)
    : working_model_(std::move(initial_model)) {}

void CampaignEditSession::mark_committed() noexcept {
    pending_changes_ = {};
    undo_stack_.clear();
    redo_stack_.clear();
}

core::Result<CampaignEditResult, core::Error>
CampaignEditSession::apply(const CampaignOperation& operation, std::uint64_t expected_revision) {
    if (expected_revision != revision_)
        return core::Result<CampaignEditResult, core::Error>::failure(stale_revision_error(expected_revision, revision_));
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        return core::Result<CampaignEditResult, core::Error>::failure(
            {core::ErrorCode::ValidationFailed, "The edit session revision counter is exhausted", "CampaignEditSession"});

    auto validation = validator_.validate(working_model_, operation);
    if (!validation.valid())
        return core::Result<CampaignEditResult, core::Error>::failure(report_error(validation));

    auto candidate = working_model_;
    std::vector<CampaignFieldChange> changes;
    std::vector<CampaignStructuralChange> structural_changes;
    for (const auto& edit : flatten(operation)) {
        auto resolved = resolve_target(candidate, edit.target);
        if (!resolved || !resolved->assign)
            return core::Result<CampaignEditResult, core::Error>::failure(
                {core::ErrorCode::ValidationFailed, "The mapped target could not be edited", "CampaignEditSession",
                 {{"property", edit.target.semantic_property}}});

        const auto found = std::find_if(changes.begin(), changes.end(), [&](const auto& change) {
            return same_target(change.target, edit.target);
        });
        if (found == changes.end()) {
            if (resolved->value == edit.value) continue;
            changes.push_back({edit.target, resolved->raw, resolved->value, edit.value});
        } else {
            found->after = edit.value;
            if (found->before == found->after) changes.erase(found);
        }
        resolved->assign(edit.value);
    }

    if (std::holds_alternative<RemoveHeroQuirkOperation>(operation) ||
        std::holds_alternative<UnequipHeroCampingSkillOperation>(operation) ||
        std::holds_alternative<DestroyTrinketOperation>(operation)) {
        auto removed = remove_structural_entry(candidate, operation);
        if (!removed)
            return core::Result<CampaignEditResult, core::Error>::failure(
                {core::ErrorCode::ValidationFailed, "The structural target could not be removed from the working model",
                 "CampaignEditSession"});
        structural_changes.push_back(std::move(*removed));
    }

    auto change_set = changes_for(changes, structural_changes);
    auto risk = risk_assessor_.assess(operation);
    const std::string* mapped_operation_id = nullptr;
    const std::vector<CampaignDocumentMutation>* mapped_mutations = nullptr;
    if (const auto* document_operation = std::get_if<ApplyCampaignDocumentMutationsOperation>(&operation)) {
        mapped_operation_id = &document_operation->operation_id;
        mapped_mutations = &document_operation->mutations;
    } else if (const auto* composite = std::get_if<CompositeCampaignOperation>(&operation);
               composite && !composite->document_mutations.empty()) {
        mapped_operation_id = &composite->mapped_operation_id;
        mapped_mutations = &composite->document_mutations;
    } else if (const auto* district_system = std::get_if<SetDistrictSystemOperation>(&operation)) {
        mapped_operation_id = nullptr;
        mapped_mutations = &district_system->mutations;
    }
    const auto* district_system_operation = std::get_if<SetDistrictSystemOperation>(&operation);
    if (district_system_operation) {
        const auto open_id = std::string{"campaign.town.set_district_system_open"};
        CampaignDocumentMutationBatch batch;
        batch.operation_id = open_id;
        batch.transaction_id = open_id + "#" + std::to_string(revision_ + 1);
        batch.mutations = district_system_operation->mutations;
        change_set.document_mutation_batches.push_back(std::move(batch));
        std::set<std::string, std::less<>> documents(change_set.affected_documents.begin(),
                                                      change_set.affected_documents.end());
        documents.insert("persist.town.json");
        change_set.affected_documents.assign(documents.begin(), documents.end());
        ChangeSet::DistrictSystemSnapshot snapshot;
        snapshot.before_open = candidate.district_system_open;
        snapshot.before_districts = candidate.districts;
        snapshot.after_open = district_system_operation->open;
        if (district_system_operation->open) {
            for (const auto& id : district_system_operation->district_ids) {
                domain::DistrictState district;
                district.id = id;
                district.read_only = false;
                district.raw = {"persist.town.json", {}, "base_root/districts/buildings/" + id};
                district.built = {false, domain::RawLocator{"persist.town.json", {},
                    "base_root/districts/buildings/" + id + "/built"}};
                district.definition.raw_id = id;
                candidate.districts.push_back(std::move(district));
            }
            candidate.district_system_open = true;
        } else {
            candidate.districts.clear();
            candidate.district_system_open = false;
        }
        snapshot.after_districts = candidate.districts;
        change_set.district_system_snapshot = std::move(snapshot);
    }
    if (mapped_operation_id && mapped_mutations) {
        const bool projects_hero_roster = std::any_of(mapped_mutations->begin(), mapped_mutations->end(),
            [](const auto& mutation) { return mutation.semantic_property == "Hero.PersistentId"; });
        const bool projects_camping_skills = std::any_of(mapped_mutations->begin(), mapped_mutations->end(),
            [](const auto& mutation) { return mutation.semantic_property == "Hero.SelectedCampingSkills"; });
        ChangeSet::HeroRosterSnapshot hero_snapshot;
        if (projects_hero_roster || projects_camping_skills) hero_snapshot.before = candidate.heroes;
        const bool projects_trinket_inventory = std::any_of(mapped_mutations->begin(), mapped_mutations->end(),
            [](const auto& mutation) { return mutation.semantic_property == "TrinketInventory.Items"; });
        ChangeSet::TrinketInventorySnapshot trinket_snapshot;
        if (projects_trinket_inventory) trinket_snapshot.before = candidate.trinket_inventory;
        const bool projects_district_state = std::any_of(mapped_mutations->begin(), mapped_mutations->end(),
            [](const auto& mutation) {
                return mutation.semantic_property == "Town.DistrictSystem" &&
                       mutation.kind == CampaignDocumentMutationKind::SetValue &&
                       mutation.target_path.starts_with("base_root/districts/buildings/") &&
                       mutation.target_path.ends_with("/built");
            });
        ChangeSet::DistrictSystemSnapshot district_snapshot;
        if (projects_district_state) {
            district_snapshot.before_open = candidate.district_system_open;
            district_snapshot.before_districts = candidate.districts;
        }
        CampaignDocumentMutationBatch batch;
        batch.operation_id = *mapped_operation_id;
        batch.transaction_id = *mapped_operation_id + "#" + std::to_string(revision_ + 1);
        batch.mutations = *mapped_mutations;
        change_set.document_mutation_batches.push_back(std::move(batch));
        std::set<std::string, std::less<>> documents(change_set.affected_documents.begin(),
                                                      change_set.affected_documents.end());
        for (const auto& mutation : *mapped_mutations) documents.insert(mutation.document_id);
        change_set.affected_documents.assign(documents.begin(), documents.end());
        std::string projection_failure;
        if (!apply_purchase_row_mutations(candidate, *mapped_mutations, true, &projection_failure))
            return core::Result<CampaignEditResult, core::Error>::failure(
                {core::ErrorCode::ValidationFailed,
                 "The mapped purchase-node append could not be projected into the campaign session: " + projection_failure,
                 "CampaignEditSession"});
        if (projects_hero_roster && !apply_hero_roster_mutations(candidate, *mapped_mutations))
            return core::Result<CampaignEditResult, core::Error>::failure(
                {core::ErrorCode::ValidationFailed,
                 "The hero roster mutation could not be projected into the campaign session",
                 "CampaignEditSession"});
        if (projects_camping_skills && !apply_camping_skill_mutations(candidate, *mapped_mutations))
            return core::Result<CampaignEditResult, core::Error>::failure(
                {core::ErrorCode::ValidationFailed,
                 "The camping skill mutation could not be projected into the campaign session",
                 "CampaignEditSession"});
        if (projects_hero_roster || projects_camping_skills) {
            hero_snapshot.after = candidate.heroes;
            change_set.hero_roster_snapshot = std::move(hero_snapshot);
        }
        if (projects_trinket_inventory && !apply_trinket_inventory_mutations(candidate, *mapped_mutations, true))
            return core::Result<CampaignEditResult, core::Error>::failure(
                {core::ErrorCode::ValidationFailed,
                 "The trinket inventory mutation could not be projected into the campaign session",
                 "CampaignEditSession"});
        if (projects_trinket_inventory) {
            trinket_snapshot.after = candidate.trinket_inventory;
            change_set.trinket_inventory_snapshot = std::move(trinket_snapshot);
        }
        if (projects_district_state) {
            if (!project_district_state_mutations(candidate, *mapped_mutations))
                return core::Result<CampaignEditResult, core::Error>::failure(
                    {core::ErrorCode::ValidationFailed,
                     "The district state mutation could not be projected into the campaign session",
                     "CampaignEditSession"});
            district_snapshot.after_open = candidate.district_system_open;
            district_snapshot.after_districts = candidate.districts;
            change_set.district_system_snapshot = std::move(district_snapshot);
        }
    }
    if (!change_set.empty()) {
        working_model_ = std::move(candidate);
        ++revision_;
        merge_pending_changes(pending_changes_, change_set);
        undo_stack_.push_back({operation_label(operation), change_set, risk});
        redo_stack_.clear();
    }
    return core::Result<CampaignEditResult, core::Error>::success(
        {revision_, std::move(change_set), std::move(validation), std::move(risk)});
}

core::Result<CampaignEditResult, core::Error>
CampaignEditSession::replay(const HistoryRecord& record, bool forward, std::uint64_t expected_revision) {
    if (expected_revision != revision_)
        return core::Result<CampaignEditResult, core::Error>::failure(stale_revision_error(expected_revision, revision_));
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        return core::Result<CampaignEditResult, core::Error>::failure(
            {core::ErrorCode::ValidationFailed, "The edit session revision counter is exhausted", "CampaignEditSession"});

    auto candidate = working_model_;
    auto changes = record.changes;
    if (!forward) std::reverse(changes.changes.begin(), changes.changes.end());
    for (const auto& change : changes.changes) {
        auto resolved = resolve_target(candidate, change.target);
        if (!resolved || !resolved->assign || resolved->raw.document_id != change.raw.document_id ||
            resolved->raw.display_path != change.raw.display_path) {
            return core::Result<CampaignEditResult, core::Error>::failure(
                {core::ErrorCode::ValidationFailed, "The edit history no longer resolves to its original mapping",
                 "CampaignEditSession", {{"property", change.target.semantic_property},
                                          {"document", change.raw.document_id},
                                          {"path", change.raw.display_path}}});
        }
        resolved->assign(forward ? change.after : change.before);
    }
    if (!forward) std::reverse(changes.structural_changes.begin(), changes.structural_changes.end());
    for (const auto& change : changes.structural_changes) {
        if (!replay_structural_entry(candidate, change, forward))
            return core::Result<CampaignEditResult, core::Error>::failure(
                {core::ErrorCode::ValidationFailed, "The structural edit history no longer resolves to its original entry",
                 "CampaignEditSession", {{"property", change.target.semantic_property},
                                          {"path", change.raw.display_path}}});
    }
    if (!forward) {
        for (auto& change : changes.structural_changes) change.action = CampaignStructuralAction::Restore;
    }
    for (auto& batch : changes.document_mutation_batches) {
        batch.cancel = !forward;
        if (!apply_purchase_row_mutations(candidate, batch.mutations, forward))
            return core::Result<CampaignEditResult, core::Error>::failure(
                {core::ErrorCode::ValidationFailed,
                 "The purchase-node edit history could not be projected into the campaign session",
                 "CampaignEditSession"});
    }
    if (changes.trinket_inventory_snapshot) {
        candidate.trinket_inventory = forward ? changes.trinket_inventory_snapshot->after
                                              : changes.trinket_inventory_snapshot->before;
        if (!forward) std::swap(changes.trinket_inventory_snapshot->before,
                                changes.trinket_inventory_snapshot->after);
    }
    if (changes.hero_roster_snapshot) {
        candidate.heroes = forward ? changes.hero_roster_snapshot->after : changes.hero_roster_snapshot->before;
        if (!forward) std::swap(changes.hero_roster_snapshot->before, changes.hero_roster_snapshot->after);
    }
    if (!forward) {
        for (auto& change : changes.changes) std::swap(change.before, change.after);
        std::reverse(changes.changes.begin(), changes.changes.end());
    }
    if (changes.district_system_snapshot) {
        candidate.district_system_open = forward ? changes.district_system_snapshot->after_open
                                                  : changes.district_system_snapshot->before_open;
        candidate.districts = forward ? changes.district_system_snapshot->after_districts
                                      : changes.district_system_snapshot->before_districts;
        if (!forward) {
            std::swap(changes.district_system_snapshot->before_open, changes.district_system_snapshot->after_open);
            std::swap(changes.district_system_snapshot->before_districts,
                      changes.district_system_snapshot->after_districts);
        }
    }
    working_model_ = std::move(candidate);
    ++revision_;
    return core::Result<CampaignEditResult, core::Error>::success(
        {revision_, std::move(changes), {}, record.risk});
}

core::Result<CampaignEditResult, core::Error>
CampaignEditSession::undo(std::uint64_t expected_revision) {
    if (undo_stack_.empty())
        return core::Result<CampaignEditResult, core::Error>::failure(empty_history_error("undo"));
    auto result = replay(undo_stack_.back(), false, expected_revision);
    if (result) {
        merge_pending_changes(pending_changes_, result.value().changes);
        redo_stack_.push_back(std::move(undo_stack_.back()));
        undo_stack_.pop_back();
    }
    return result;
}

core::Result<CampaignEditResult, core::Error>
CampaignEditSession::redo(std::uint64_t expected_revision) {
    if (redo_stack_.empty())
        return core::Result<CampaignEditResult, core::Error>::failure(empty_history_error("redo"));
    auto result = replay(redo_stack_.back(), true, expected_revision);
    if (result) {
        merge_pending_changes(pending_changes_, result.value().changes);
        undo_stack_.push_back(std::move(redo_stack_.back()));
        redo_stack_.pop_back();
    }
    return result;
}

} // namespace ddse::application
