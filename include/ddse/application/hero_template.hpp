#pragma once

#include "ddse/core/dson/dson_document.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ddse::application {

// Builds an independent copy of the application-owned resolve-level-zero DSON
// template, initialized with effective class and starter skill definitions.
[[nodiscard]] core::Result<std::shared_ptr<core::dson::DsonDocument>, core::Error>
build_blank_level_zero_hero_template(std::string_view hero_class,
                                    std::string_view hero_name,
                                    const std::vector<std::string>& combat_skills,
                                    const std::vector<std::string>& camping_skills,
                                    float base_hit_points);

} // namespace ddse::application
