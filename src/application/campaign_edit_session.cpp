#include "ddse/application/campaign_edit_session.hpp"

#include "ddse/application/campaign_mappings.hpp"

#include <algorithm>
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

bool is_editable_property(std::string_view property) {
    const auto* mapping = find_mapping(property);
    return mapping != nullptr && mapping->editable_in_session;
}

std::vector<SetCampaignValueOperation> flatten(const CampaignOperation& operation) {
    if (const auto* single = std::get_if<SetCampaignValueOperation>(&operation)) return {*single};
    if (const auto* composite = std::get_if<CompositeCampaignOperation>(&operation)) return composite->operations;
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
    if (std::holds_alternative<SetDistrictBuiltOperation>(operation)) return "Set district built state";
    if (std::holds_alternative<RemoveHeroQuirkOperation>(operation)) return "Remove hero quirk";
    return "Destroy trinket";
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
                  ValidationReport& report) {
    const auto& target = operation.target;
    const auto* mapping = find_mapping(target.semantic_property);
    if (mapping == nullptr) {
        add_issue(report, ValidationSeverity::Error, "mapping.missing",
                  "No campaign mapping exists for this semantic property", target, true);
        return;
    }
    if (!is_editable_property(target.semantic_property)) {
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
        (mapping->expected_type == core::dson::ValueKind::Boolean && std::holds_alternative<bool>(operation.value));
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
    if (const auto* name = std::get_if<std::string>(&operation.value); name && name->empty()) {
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
    std::set<std::string, std::less<>> documents;
    for (const auto& change : pending.changes) documents.insert(change.raw.document_id);
    for (const auto& change : pending.structural_changes) documents.insert(change.raw.document_id);
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
        {"campaign.hero.set_quirk_locked", "锁定正面怪癖", CampaignOperationAvailability::Available,
         {"Hero.Quirk.Locked"}, "要求有效定义标记 can_modify_in_activity。"},
        {"campaign.hero.remove_quirk", "移除怪癖", CampaignOperationAvailability::Available,
         {"Hero.Quirk.Entry"}, "直接移除非疾病怪癖完整记录。"},
        {"campaign.trinket.destroy", "销毁饰品", CampaignOperationAvailability::Available,
         {"Hero.Trinket.Entry", "TrinketInventory.Entry"}, "可分别销毁装备栏或庄园库存条目。"},
        {"campaign.town.set_district_built", "设置小镇建筑状态", CampaignOperationAvailability::Available,
         {"Town.District.Built"}, "要求存档已经包含开放的小镇建筑系统状态。"},
        {"campaign.hero.add", "新增英雄", CampaignOperationAvailability::NotImplemented,
         {"Hero.PersistentId"}, "需要 HeroFactory、身份生成和跨文档引用校验。"},
        {"campaign.hero.set_equipment_ranks", "设置武器与防具等级", CampaignOperationAvailability::NotImplemented,
         {"Hero.WeaponRank", "Hero.ArmourRank"}, "需要与 persist.upgrades.json 购买节点组成原子复合操作。"},
        {"campaign.hero.set_combat_skill_rank", "设置战斗技能等级", CampaignOperationAvailability::NotImplemented,
         {"Upgrade.PurchaseNode"}, "需要按有效 Mod 技能树映射购买节点及技能上限。"},
        {"campaign.hero.set_camping_skill_learned", "学习生存技能", CampaignOperationAvailability::NotImplemented,
         {"Upgrade.PurchaseNode"}, "技能学习与 selected_camping_skills 装备集合需分开建模。"},
        {"campaign.hero.unequip_camping_skill", "取消装备生存技能", CampaignOperationAvailability::NotImplemented,
         {"Hero.SelectedCampingSkills"}, "与训练营锁定不同；等待独立 Operation 接入。"},
        {"campaign.hero.lock_camping_skill", "锁定生存技能", CampaignOperationAvailability::Deferred,
         {}, "游戏内验证未通过，暂缓。"},
        {"campaign.hero.edit_disease", "编辑疾病", CampaignOperationAvailability::Deferred,
         {}, "等待包含真实疾病记录的存档样本。"},
        {"campaign.hero.add_or_replace_quirk", "增加或替换怪癖", CampaignOperationAvailability::NotImplemented,
         {"Hero.Quirks"}, "已通过游戏测试，尚未接入结构插入和有效定义校验。"},
        {"campaign.trinket.add_inventory", "增加库存饰品", CampaignOperationAvailability::NotImplemented,
         {"TrinketInventory.Items"}, "已通过游戏测试，尚未接入安全结构插入。"},
        {"campaign.hero.equip_trinket", "装备饰品", CampaignOperationAvailability::NotImplemented,
         {"Hero.Trinkets"}, "需要职业限制校验和饰品记录模板。"},
        {"campaign.town.set_upgrade_rank", "设置小镇升级进度", CampaignOperationAvailability::NotImplemented,
         {"Upgrade.PurchaseNode"}, "需要同步购买节点以及锁定后续节点。"},
        {"campaign.town.set_district_system_open", "开放或锁定小镇建筑系统", CampaignOperationAvailability::NotImplemented,
         {"Town.Districts"}, "开放时必须从有效内容环境组装各 District 状态对象。"},
    };
    return capabilities;
}

ValidationReport CampaignOperationValidator::validate(const CampaignModel& model,
                                                       const CampaignOperation& operation) const {
    ValidationReport report;
    const auto operations = flatten(operation);
    if (!operations.empty()) {
        for (const auto& edit : operations) validate_one(model, edit, report);
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
            result.reasons.push_back(edit.target.semantic_property + " may depend on progression or purchase-history fields not modeled yet.");
        }
    }
    if (std::holds_alternative<RemoveHeroQuirkOperation>(operation) ||
        std::holds_alternative<DestroyTrinketOperation>(operation)) {
        result.level = std::max(result.level, CampaignRiskLevel::Moderate);
        result.reasons.push_back("This operation destroys a complete collection entry and can only be restored after commit from the verified backup.");
    }
    std::sort(result.reasons.begin(), result.reasons.end());
    result.reasons.erase(std::unique(result.reasons.begin(), result.reasons.end()), result.reasons.end());
    return result;
}

CampaignEditSession::CampaignEditSession(CampaignModel initial_model)
    : working_model_(std::move(initial_model)) {}

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
    if (!forward) {
        for (auto& change : changes.changes) std::swap(change.before, change.after);
        std::reverse(changes.changes.begin(), changes.changes.end());
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
