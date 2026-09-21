#pragma once

#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"
#include "ddse/core/dson/dson_document.hpp"
#include "ddse/domain/campaign_model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ddse::application {

using CampaignValue = std::variant<std::int32_t, float, std::string, bool>;

struct CampaignOperationTarget {
    CampaignOperationTarget() = default;
    CampaignOperationTarget(std::string property, std::string entity = {},
                            std::optional<std::size_t> occurrence = std::nullopt,
                            std::string member = {})
        : semantic_property(std::move(property)), entity_id(std::move(entity)),
          occurrence_index(occurrence), member_id(std::move(member)) {}

    std::string semantic_property;
    // Hero persistent ID. Resource operations use occurrence_index instead.
    std::string entity_id;
    std::optional<std::size_t> occurrence_index;
    // Quirk id, district id, or a stable raw collection key, depending on mapping.
    std::string member_id;
};

struct SetCampaignValueOperation {
    CampaignOperationTarget target;
    CampaignValue value;
};

struct CompositeCampaignOperation {
    std::string label;
    std::vector<SetCampaignValueOperation> operations;
};

struct SetHeroQuirkLockedOperation {
    std::string hero_id;
    std::string quirk_id;
    bool locked{};
};

struct SetDistrictBuiltOperation {
    std::string district_id;
    bool built{};
};

struct RemoveHeroQuirkOperation {
    std::string hero_id;
    std::string quirk_id;
};

struct DestroyTrinketOperation {
    // Empty hero_id selects the estate inventory; otherwise item_key is the
    // stable raw key/path for an equipped slot on that hero.
    std::string hero_id;
    std::string item_key;
};

enum class CampaignOperationAvailability { Available, NotImplemented, Deferred };

struct CampaignOperationCapabilityDescriptor {
    std::string operation_id;
    std::string display_name;
    CampaignOperationAvailability availability{CampaignOperationAvailability::NotImplemented};
    std::vector<std::string> mappings;
    std::string explanation;
};

[[nodiscard]] const std::vector<CampaignOperationCapabilityDescriptor>& campaign_operation_capabilities();

using CampaignOperation = std::variant<SetCampaignValueOperation, CompositeCampaignOperation,
                                       SetHeroQuirkLockedOperation, SetDistrictBuiltOperation,
                                       RemoveHeroQuirkOperation, DestroyTrinketOperation>;

enum class ValidationSeverity { Warning, Error };

struct ValidationIssue {
    ValidationSeverity severity{ValidationSeverity::Error};
    std::string code;
    std::string message;
    std::string semantic_property;
    std::string entity_id;
    bool blocking{true};
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;

    [[nodiscard]] bool valid() const noexcept;
};

enum class CampaignRiskLevel { Low, Moderate, High };

struct RiskAssessment {
    CampaignRiskLevel level{CampaignRiskLevel::Low};
    std::vector<std::string> reasons;
};

struct CampaignFieldChange {
    CampaignOperationTarget target;
    domain::RawLocator raw;
    CampaignValue before;
    CampaignValue after;
};

enum class CampaignStructuralAction { Erase, Restore };

struct CampaignStructuralChange {
    CampaignOperationTarget target;
    domain::RawLocator raw;
    // SaveAdapter persists Erase; Restore is an undo event used to cancel that
    // pending erase. The pre-edit model entry makes it reversible in-session.
    core::dson::ValueKind expected_kind{core::dson::ValueKind::Object};
    std::size_t original_index{};
    std::variant<domain::HeroQuirk, domain::HeroTrinket, domain::TrinketInventoryEntry> removed_entry;
    CampaignStructuralAction action{CampaignStructuralAction::Erase};
};

struct ChangeSet {
    std::vector<CampaignFieldChange> changes;
    std::vector<CampaignStructuralChange> structural_changes;
    std::vector<std::string> affected_documents;

    [[nodiscard]] bool empty() const noexcept { return changes.empty() && structural_changes.empty(); }
};

struct CampaignEditResult {
    std::uint64_t revision{};
    ChangeSet changes;
    ValidationReport validation;
    RiskAssessment risk;
};

class CampaignOperationValidator {
public:
    [[nodiscard]] ValidationReport validate(const domain::CampaignModel& model,
                                            const CampaignOperation& operation) const;
};

class CampaignOperationRiskAssessor {
public:
    [[nodiscard]] RiskAssessment assess(const CampaignOperation& operation) const;
};

// An edit session owns a private semantic-model copy. It never edits DSON bytes or files.
class CampaignEditSession {
public:
    explicit CampaignEditSession(domain::CampaignModel initial_model);

    [[nodiscard]] const domain::CampaignModel& model() const noexcept { return working_model_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    // Net delta from the session's loaded model. Pass this to SaveAdapter;
    // per-operation results are intended for UI feedback and history display.
    [[nodiscard]] const ChangeSet& pending_changes() const noexcept { return pending_changes_; }
    [[nodiscard]] bool can_undo() const noexcept { return !undo_stack_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_stack_.empty(); }

    [[nodiscard]] core::Result<CampaignEditResult, core::Error>
    apply(const CampaignOperation& operation, std::uint64_t expected_revision);

    [[nodiscard]] core::Result<CampaignEditResult, core::Error>
    undo(std::uint64_t expected_revision);

    [[nodiscard]] core::Result<CampaignEditResult, core::Error>
    redo(std::uint64_t expected_revision);

private:
    struct HistoryRecord {
        std::string label;
        ChangeSet changes;
        RiskAssessment risk;
    };

    [[nodiscard]] core::Result<CampaignEditResult, core::Error>
    replay(const HistoryRecord& record, bool forward, std::uint64_t expected_revision);

    domain::CampaignModel working_model_;
    ChangeSet pending_changes_;
    std::uint64_t revision_{};
    std::vector<HistoryRecord> undo_stack_;
    std::vector<HistoryRecord> redo_stack_;
    CampaignOperationValidator validator_;
    CampaignOperationRiskAssessor risk_assessor_;
};

} // namespace ddse::application
