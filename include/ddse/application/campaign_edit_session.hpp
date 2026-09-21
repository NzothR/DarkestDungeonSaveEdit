#pragma once

#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"
#include "ddse/domain/campaign_model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ddse::application {

using CampaignValue = std::variant<std::int32_t, float, std::string>;

struct CampaignOperationTarget {
    std::string semantic_property;
    // Hero persistent ID. Resource operations use occurrence_index instead.
    std::string entity_id;
    std::optional<std::size_t> occurrence_index;
};

struct SetCampaignValueOperation {
    CampaignOperationTarget target;
    CampaignValue value;
};

struct CompositeCampaignOperation {
    std::string label;
    std::vector<SetCampaignValueOperation> operations;
};

using CampaignOperation = std::variant<SetCampaignValueOperation, CompositeCampaignOperation>;

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

struct ChangeSet {
    std::vector<CampaignFieldChange> changes;
    std::vector<std::string> affected_documents;

    [[nodiscard]] bool empty() const noexcept { return changes.empty(); }
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
    std::uint64_t revision_{};
    std::vector<HistoryRecord> undo_stack_;
    std::vector<HistoryRecord> redo_stack_;
    CampaignOperationValidator validator_;
    CampaignOperationRiskAssessor risk_assessor_;
};

} // namespace ddse::application
