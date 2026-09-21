#pragma once

#include "ddse/core/dson/dson_document.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ddse::application {

struct CampaignMappingDescriptor {
    std::string semantic_property;
    std::string document_id;
    std::string raw_path_template;
    core::dson::ValueKind expected_type{core::dson::ValueKind::Unknown};
    std::string evidence_level;
    bool semantically_writable{};
    bool game_mutation_verified{};
    std::string notes;
};

struct CampaignResourceValue {
    std::size_t wallet_index{};
    std::string id;
    std::int32_t amount{};
    std::string raw_object_path;
};

[[nodiscard]] const std::vector<CampaignMappingDescriptor>& stage7_resource_mappings();

[[nodiscard]] core::Result<std::vector<CampaignResourceValue>, core::Error>
read_campaign_resources(const core::dson::DsonDocument& estate_document);

} // namespace ddse::application
