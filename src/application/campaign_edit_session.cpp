#include "ddse/application/campaign_edit_session.hpp"

#include "ddse/application/campaign_mappings.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
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
    for (const auto& mapping : stage7_resource_mappings())
        if (mapping.semantic_property == property) return &mapping;
    for (const auto& mapping : stage8_campaign_mappings())
        if (mapping.semantic_property == property) return &mapping;
    return nullptr;
}

bool is_editable_property(std::string_view property) {
    const auto* mapping = find_mapping(property);
    return mapping != nullptr && mapping->editable_in_session;
}

std::vector<SetCampaignValueOperation> flatten(const CampaignOperation& operation) {
    if (const auto* single = std::get_if<SetCampaignValueOperation>(&operation)) return {*single};
    return std::get<CompositeCampaignOperation>(operation).operations;
}

std::string operation_label(const CampaignOperation& operation) {
    if (const auto* single = std::get_if<SetCampaignValueOperation>(&operation))
        return single->target.semantic_property;
    const auto& composite = std::get<CompositeCampaignOperation>(operation);
    return composite.label.empty() ? "Composite campaign operation" : composite.label;
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

    if (target.entity_id.empty()) return std::nullopt;
    const auto match_count = std::count_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
        return item.persistent_id == target.entity_id;
    });
    if (match_count != 1) return std::nullopt;
    const auto hero = std::find_if(model.heroes.begin(), model.heroes.end(), [&](const auto& item) {
        return item.persistent_id == target.entity_id;
    });
    if (hero == model.heroes.end()) return std::nullopt;

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
        (mapping->expected_type == core::dson::ValueKind::String && std::holds_alternative<std::string>(operation.value));
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

bool same_target(const CampaignOperationTarget& lhs, const CampaignOperationTarget& rhs) {
    return lhs.semantic_property == rhs.semantic_property && lhs.entity_id == rhs.entity_id &&
           lhs.occurrence_index == rhs.occurrence_index;
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

ValidationReport CampaignOperationValidator::validate(const CampaignModel& model,
                                                       const CampaignOperation& operation) const {
    ValidationReport report;
    const auto operations = flatten(operation);
    if (operations.empty()) {
        report.issues.push_back({ValidationSeverity::Error, "operation.empty",
            "A campaign operation must contain at least one field edit", {}, {}, true});
        return report;
    }
    for (const auto& edit : operations) validate_one(model, edit, report);
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

    auto change_set = changes_for(changes);
    auto risk = risk_assessor_.assess(operation);
    if (!change_set.empty()) {
        working_model_ = std::move(candidate);
        ++revision_;
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
        undo_stack_.push_back(std::move(redo_stack_.back()));
        redo_stack_.pop_back();
    }
    return result;
}

} // namespace ddse::application
