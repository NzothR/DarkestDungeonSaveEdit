#pragma once

#include "ddse/core/dson/dson_document.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ddse::application {

enum class CampaignMappingCapability {
    ReadOnly,
    SessionOnly,
    CandidateWritable,
    CommitWritable,
};

struct CampaignMappingDescriptor {
    std::string semantic_property;
    std::string document_id;
    std::string raw_path_template;
    core::dson::ValueKind expected_type{core::dson::ValueKind::Unknown};
    std::string evidence_level;
    bool semantically_writable{};
    bool game_mutation_verified{};
    std::string notes;
    bool editable_in_session{};

    [[nodiscard]] CampaignMappingCapability capability() const noexcept {
        if (semantically_writable && game_mutation_verified) return CampaignMappingCapability::CommitWritable;
        if (semantically_writable) return CampaignMappingCapability::CandidateWritable;
        if (editable_in_session) return CampaignMappingCapability::SessionOnly;
        return CampaignMappingCapability::ReadOnly;
    }
};

// Central lookup used by both Operation validation and the save adapter. Keeping
// this in one place prevents controller/UI code from inventing raw paths.
[[nodiscard]] const CampaignMappingDescriptor* find_campaign_mapping(std::string_view semantic_property);

struct CampaignResourceValue {
    std::size_t wallet_index{};
    std::string id;
    std::int32_t amount{};
    std::string raw_object_path;
};

struct CampaignResourceObservation {
    std::size_t wallet_index{};
    std::optional<std::string> id;
    std::optional<std::int32_t> amount;
    std::string raw_object_path;
};

struct CampaignMappingIssue {
    std::string path;
    std::string message;
};

struct CampaignResourceScan {
    bool wallet_found{};
    std::vector<CampaignResourceObservation> entries;
    std::vector<CampaignMappingIssue> issues;
};

[[nodiscard]] const std::vector<CampaignMappingDescriptor>& stage7_resource_mappings();
[[nodiscard]] const std::vector<CampaignMappingDescriptor>& stage8_campaign_mappings();

[[nodiscard]] CampaignResourceScan scan_campaign_resources(const core::dson::DsonDocument& estate_document);

[[nodiscard]] core::Result<std::vector<CampaignResourceValue>, core::Error>
read_campaign_resources(const core::dson::DsonDocument& estate_document);

} // namespace ddse::application
