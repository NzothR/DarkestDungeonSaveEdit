#include "ddse/application/save_commit.hpp"
#include "ddse/application/hero_template.hpp"

#include "ddse/application/campaign_mappings.hpp"
#include "ddse/core/dson/dson_document_editor.hpp"
#include "ddse/core/dson/dson_reader.hpp"
#include "ddse/core/dson/dson_writer.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace ddse::application {
namespace {

using core::dson::DsonDocument;
using core::dson::DsonField;
using core::dson::ValueKind;

struct DirectoryTree {
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> directories;
};

const CampaignMappingDescriptor* find_mapping(std::string_view property) {
    return find_campaign_mapping(property);
}

std::string expand_path(std::string path, const CampaignOperationTarget& target) {
    auto replace_all = [&](std::string_view token, std::string value) {
        std::size_t position = 0;
        while ((position = path.find(token, position)) != std::string::npos) {
            path.replace(position, token.size(), value);
            position += value.size();
        }
    };
    if (target.occurrence_index) replace_all("{index}", std::to_string(*target.occurrence_index));
    if (!target.entity_id.empty()) replace_all("{guid}", target.entity_id);
    if (!target.member_id.empty()) replace_all("{memberId}", target.member_id);
    if (!target.member_id.empty()) replace_all("{skillId}", target.member_id);
    return path;
}

std::string_view expected_leaf(const CampaignMappingDescriptor& mapping) {
    auto path = std::string_view{mapping.raw_path_template};
    const auto embedded = path.rfind(" => ");
    if (embedded != std::string_view::npos) path.remove_prefix(embedded + 4);
    const auto slash = path.find_last_of('/');
    if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
    return path;
}

core::Error adapter_error(core::ErrorCode code, std::string message,
                          std::map<std::string, std::string> context = {}) {
    return {code, std::move(message), "SaveAdapter", std::move(context)};
}

core::Error backup_error(const core::Error& cause, const std::filesystem::path& path) {
    core::Error error{core::ErrorCode::BackupFailed, "The complete profile backup could not be verified",
                      "SafeSaveCommitter", {{"backup_directory", path.string()}}};
    error.cause = std::make_shared<core::Error>(cause);
    return error;
}

std::string joined(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += ",";
        result += value;
    }
    return result;
}

core::Error commit_error(core::ErrorCode code, std::string message,
                         const std::filesystem::path& backup_path,
                         const std::vector<std::string>& committed,
                         std::string failed_document, const core::Error* cause = nullptr) {
    std::map<std::string, std::string> context{
        {"backup_directory", backup_path.string()},
        {"committed_documents", joined(committed)},
        {"partial_commit", code == core::ErrorCode::PartialCommit ? "true" : "false"}};
    if (!failed_document.empty()) context.emplace("failed_document", std::move(failed_document));
    core::Error error{code, std::move(message), "SafeSaveCommitter", std::move(context)};
    if (cause != nullptr) error.cause = std::make_shared<core::Error>(*cause);
    return error;
}

bool as_campaign_value(const core::dson::Value& value, CampaignValue& output) {
    if (const auto* integer = std::get_if<std::int32_t>(&value)) output = *integer;
    else if (const auto* number = std::get_if<float>(&value)) output = *number;
    else if (const auto* text = std::get_if<std::string>(&value)) output = *text;
    else if (const auto* boolean = std::get_if<bool>(&value)) output = *boolean;
    else if (const auto* character = std::get_if<char>(&value)) output = *character;
    else return false;
    return true;
}

bool value_matches(const DsonField& field, const CampaignValue& value) {
    CampaignValue actual;
    return as_campaign_value(field.value, actual) && actual == value;
}

bool type_matches(ValueKind kind, const CampaignValue& value) {
    return (kind == ValueKind::Integer && std::holds_alternative<std::int32_t>(value)) ||
           (kind == ValueKind::Float && std::holds_alternative<float>(value)) ||
           (kind == ValueKind::String && std::holds_alternative<std::string>(value)) ||
           (kind == ValueKind::Boolean && std::holds_alternative<bool>(value)) ||
           (kind == ValueKind::Character && std::holds_alternative<char>(value));
}

std::optional<std::reference_wrapper<DsonField>> locate_field_by_path(
    DsonDocument& document, std::string_view display_path, std::string& reason);

bool purchase_target_matches(DsonDocument& document, const CampaignOperationTarget& target,
                             std::string_view purchase_path, std::string& reason) {
    if (target.semantic_property != "Upgrade.PurchaseNode") return true;
    std::int32_t expected_instance{};
    const auto [instance_end, instance_error] = std::from_chars(
        target.entity_id.data(), target.entity_id.data() + target.entity_id.size(), expected_instance);
    const auto separator = target.member_id.find('|');
    if (instance_error != std::errc{} || instance_end != target.entity_id.data() + target.entity_id.size() ||
        separator == std::string::npos || target.member_id.size() - separator != 2U) {
        reason = "Purchase-node target does not contain a valid instance/tree/code identity";
        return false;
    }
    std::int32_t expected_tree{};
    const auto tree_text = std::string_view{target.member_id}.substr(0, separator);
    const auto [tree_end, tree_error] = std::from_chars(tree_text.data(), tree_text.data() + tree_text.size(), expected_tree);
    if (tree_error != std::errc{} || tree_end != tree_text.data() + tree_text.size()) {
        reason = "Purchase-node target tree identifier is invalid";
        return false;
    }
    const auto slash = purchase_path.find_last_of('/');
    if (slash == std::string_view::npos) {
        reason = "Purchase-node path has no row parent";
        return false;
    }
    const auto row_path = purchase_path.substr(0, slash);
    auto sibling = [&](std::string_view name) -> std::optional<std::reference_wrapper<DsonField>> {
        auto found = locate_field_by_path(document, std::string{row_path} + "/" + std::string{name}, reason);
        return found;
    };
    const auto instance = sibling("instance_number");
    const auto tree = sibling("tree_id");
    const auto code = sibling("requirement_code");
    if (!instance || !tree || !code) {
        reason = "Purchase-node row identity fields are missing";
        return false;
    }
    const auto* actual_instance = std::get_if<std::int32_t>(&instance->get().value);
    const auto* actual_tree = std::get_if<std::int32_t>(&tree->get().value);
    const auto* actual_code = std::get_if<char>(&code->get().value);
    if (!actual_instance || !actual_tree || !actual_code || *actual_instance != expected_instance ||
        *actual_tree != expected_tree || *actual_code != target.member_id.back()) {
        reason = "Purchase-node row identity differs from the mapped instance/tree/requirement code";
        return false;
    }
    return true;
}

void replace_field_value(DsonField& field, const CampaignValue& value) {
    std::visit([&](const auto& typed) { field.replace_value(typed); }, value);
}

std::vector<std::string> locator_path_segments(std::string_view path) {
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (true) {
        const auto separator = path.find(" => ", start);
        const auto end = separator == std::string_view::npos ? path.size() : separator;
        segments.emplace_back(path.substr(start, end - start));
        if (separator == std::string_view::npos) break;
        start = separator + 4;
    }
    return segments;
}

std::optional<std::reference_wrapper<DsonField>> locate_field_by_path(
    DsonDocument& document, std::string_view display_path, std::string& reason) {
    const auto segments = locator_path_segments(display_path);
    DsonDocument* current = &document;
    for (std::size_t segment = 0; segment < segments.size(); ++segment) {
        DsonField* match = nullptr;
        for (auto& field : current->fields) {
            if (field.path != segments[segment]) continue;
            if (match != nullptr) {
                reason = "Raw locator path is ambiguous in the current DSON document";
                return std::nullopt;
            }
            match = &field;
        }
        if (match == nullptr) {
            reason = "Raw locator path no longer exists in the current DSON document";
            return std::nullopt;
        }
        if (segment + 1 == segments.size()) return std::ref(*match);
        if (match->kind != ValueKind::EmbeddedDson || !match->embedded_document) {
            reason = "Raw locator path expects an embedded DSON document which is unavailable";
            return std::nullopt;
        }
        current = match->embedded_document.get();
    }
    reason = "Raw locator path did not resolve a field";
    return std::nullopt;
}

std::optional<std::pair<std::reference_wrapper<DsonDocument>, std::string>> locate_document_for_path(
    DsonDocument& document, std::string_view display_path, std::string& reason) {
    auto segments = locator_path_segments(display_path);
    if (segments.empty()) {
        reason = "Raw locator has no path segments";
        return std::nullopt;
    }
    DsonDocument* current = &document;
    for (std::size_t segment = 0; segment + 1 < segments.size(); ++segment) {
        DsonField* match = nullptr;
        for (auto& field : current->fields) {
            if (field.path != segments[segment]) continue;
            if (match != nullptr) {
                reason = "Raw locator path is ambiguous in the current DSON document";
                return std::nullopt;
            }
            match = &field;
        }
        if (match == nullptr || match->kind != ValueKind::EmbeddedDson || !match->embedded_document) {
            reason = "Raw locator embedded-document path does not resolve";
            return std::nullopt;
        }
        current = match->embedded_document.get();
    }
    return std::pair{std::ref(*current), segments.back()};
}

std::optional<std::reference_wrapper<DsonField>> locate_field(DsonDocument& document,
                                                               const domain::RawLocator& locator,
                                                               std::string& reason) {
    if (!locator.display_path.empty()) {
        auto by_path = locate_field_by_path(document, locator.display_path, reason);
        if (by_path) return by_path;
        // Keep the indexed resolver as a fallback for legacy locators which
        // display non-canonical paths, but never use it after a structural edit.
        if (document.structural_dirty) return std::nullopt;
    }
    if (locator.steps.empty()) {
        reason = "Raw locator has no field steps";
        return std::nullopt;
    }
    DsonDocument* current = &document;
    for (std::size_t step_index = 0; step_index < locator.steps.size(); ++step_index) {
        const auto& step = locator.steps[step_index];
        if (step.field_index >= current->fields.size()) {
            reason = "Raw locator field index is outside the DSON document";
            return std::nullopt;
        }
        auto& field = current->fields[step.field_index];
        if (field.name != step.field_name) {
            reason = "Raw locator field name does not match the DSON document";
            return std::nullopt;
        }
        if (step_index + 1 == locator.steps.size()) return std::ref(field);

        if (step.enters_embedded_document) {
            if (field.kind != ValueKind::EmbeddedDson || !field.embedded_document) {
                reason = "Raw locator expects an embedded DSON document which is unavailable";
                return std::nullopt;
            }
            current = field.embedded_document.get();
        } else {
            const auto next_index = locator.steps[step_index + 1].field_index;
            if (next_index >= current->fields.size() || current->fields[next_index].parent_index != step.field_index) {
                reason = "Raw locator steps do not follow the DSON parent-child structure";
                return std::nullopt;
            }
        }
    }
    reason = "Raw locator did not resolve a field";
    return std::nullopt;
}

std::optional<std::reference_wrapper<const DsonField>> locate_field(const DsonDocument& document,
                                                                     const domain::RawLocator& locator,
                                                                     std::string& reason) {
    auto& mutable_document = const_cast<DsonDocument&>(document);
    const auto found = locate_field(mutable_document, locator, reason);
    if (!found) return std::nullopt;
    return std::cref(found->get());
}

DsonDocument clone_document(const DsonDocument& source) {
    auto clone = source;
    for (std::size_t index = 0; index < source.fields.size(); ++index) {
        if (source.fields[index].embedded_document) {
            clone.fields[index].embedded_document = std::make_shared<DsonDocument>(
                clone_document(*source.fields[index].embedded_document));
        }
    }
    return clone;
}

bool document_has_dirty_fields(const DsonDocument& document) {
    for (const auto& field : document.fields) {
        if (field.dirty) return true;
        if (field.embedded_document && document_has_dirty_fields(*field.embedded_document)) return true;
    }
    return false;
}

std::map<std::vector<std::size_t>, std::set<std::size_t>> allowed_fields(
    const std::vector<CampaignFieldChange>& changes) {
    std::map<std::vector<std::size_t>, std::set<std::size_t>> result;
    for (const auto& change : changes) {
        std::vector<std::size_t> document_path;
        const auto& steps = change.raw.steps;
        if (steps.empty()) continue;
        for (std::size_t index = 0; index + 1 < steps.size(); ++index)
            if (steps[index].enters_embedded_document) document_path.push_back(steps[index].field_index);
        result[document_path].insert(steps.back().field_index);
    }
    return result;
}

bool same_header_structure(const DsonDocument& lhs, const DsonDocument& rhs) {
    const auto& a = lhs.header;
    const auto& b = rhs.header;
    return a.magic == b.magic && a.revision == b.revision && a.header_length == b.header_length &&
           a.reserved_1 == b.reserved_1 && a.meta1_size == b.meta1_size && a.meta1_count == b.meta1_count &&
           a.meta1_offset == b.meta1_offset && a.reserved_2 == b.reserved_2 && a.reserved_3 == b.reserved_3 &&
           a.meta2_count == b.meta2_count && a.meta2_offset == b.meta2_offset &&
           a.reserved_4 == b.reserved_4 && a.data_offset == b.data_offset;
}

bool same_meta1(const core::dson::Meta1Entry& lhs, const core::dson::Meta1Entry& rhs) {
    return lhs.parent_index == rhs.parent_index && lhs.meta2_entry_index == rhs.meta2_entry_index &&
           lhs.direct_children == rhs.direct_children && lhs.all_descendants == rhs.all_descendants;
}

bool same_structure_except_targets(const DsonDocument& original, const DsonDocument& candidate,
                                   const std::map<std::vector<std::size_t>, std::set<std::size_t>>& allowed,
                                   const std::vector<std::size_t>& document_path,
                                   std::string& reason) {
    if (!same_header_structure(original, candidate) || original.meta1.size() != candidate.meta1.size() ||
        original.meta2.size() != candidate.meta2.size() || original.fields.size() != candidate.fields.size() ||
        original.root_fields != candidate.root_fields) {
        reason = "Candidate changed DSON document structure";
        return false;
    }
    for (std::size_t index = 0; index < original.meta1.size(); ++index) {
        if (!same_meta1(original.meta1[index], candidate.meta1[index])) {
            reason = "Candidate changed DSON object topology";
            return false;
        }
    }
    for (std::size_t index = 0; index < original.meta2.size(); ++index) {
        if (original.meta2[index].name_hash != candidate.meta2[index].name_hash ||
            original.meta2[index].field_info != candidate.meta2[index].field_info) {
            reason = "Candidate changed DSON field metadata";
            return false;
        }
    }

    const auto allowed_here = allowed.find(document_path);
    for (std::size_t index = 0; index < original.fields.size(); ++index) {
        const auto& before = original.fields[index];
        const auto& after = candidate.fields[index];
        if (before.name != after.name || before.path != after.path || before.kind != after.kind ||
            before.type_evidence != after.type_evidence || before.parent_index != after.parent_index ||
            before.meta2_entry_index != after.meta2_entry_index || before.meta1_entry_index != after.meta1_entry_index ||
            before.name_hash != after.name_hash || before.field_info != after.field_info ||
            before.children != after.children ||
            static_cast<bool>(before.embedded_document) != static_cast<bool>(after.embedded_document)) {
            reason = "Candidate changed a DSON field identity or object relationship";
            return false;
        }
        const bool target = allowed_here != allowed.end() && allowed_here->second.contains(index);
        if (!target && before.kind != ValueKind::EmbeddedDson && before.value != after.value) {
            reason = "Candidate changed a DSON value outside the ChangeSet: " + before.path;
            return false;
        }
        if (!target && before.kind == ValueKind::Unknown && before.raw_data != after.raw_data) {
            reason = "Candidate changed unknown raw DSON bytes outside the ChangeSet: " + before.path;
            return false;
        }
        if (before.embedded_document) {
            auto child_path = document_path;
            child_path.push_back(index);
            if (!same_structure_except_targets(*before.embedded_document, *after.embedded_document,
                                               allowed, child_path, reason)) return false;
        }
    }
    return true;
}

bool validate_decoded_candidate(const DsonDocument& original, const DsonDocument& candidate,
                                const std::vector<CampaignFieldChange>& changes,
                                std::string& reason) {
    const auto allowed = allowed_fields(changes);
    if (!same_structure_except_targets(original, candidate, allowed, {}, reason)) return false;
    for (const auto& change : changes) {
        const auto field = locate_field(candidate, change.raw, reason);
        if (!field) return false;
        if (!value_matches(field->get(), change.after)) {
            reason = "Read-back value does not match the ChangeSet target value";
            return false;
        }
    }
    return true;
}

struct FieldSnapshot {
    std::string name;
    ValueKind kind{ValueKind::Unknown};
    core::dson::Value value;
    std::vector<std::byte> raw_data;
};

void collect_field_snapshots(const DsonDocument& document, std::string_view prefix,
                             std::map<std::string, std::vector<FieldSnapshot>, std::less<>>& output) {
    for (const auto& field : document.fields) {
        const auto full_path = prefix.empty() ? field.path : std::string{prefix} + field.path;
        output[full_path].push_back({field.name, field.kind, field.value, field.raw_data});
        if (field.embedded_document)
            collect_field_snapshots(*field.embedded_document, full_path + " => ", output);
    }
}

bool path_is_removed(std::string_view field_path, const std::vector<CampaignStructuralChange>& changes) {
    return std::any_of(changes.begin(), changes.end(), [&](const auto& change) {
        const auto& removed = change.raw.display_path;
        return field_path == removed || field_path.starts_with(removed + "/") ||
               field_path.starts_with(removed + " => ");
    });
}

std::vector<std::string_view> split_path_parts(std::string_view path) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (true) {
        const auto arrow = path.find(" => ", start);
        const auto end = arrow == std::string_view::npos ? path.size() : arrow;
        result.push_back(path.substr(start, end - start));
        if (arrow == std::string_view::npos) break;
        start = arrow + 4;
    }
    return result;
}

bool path_part_matches_prefix(std::string_view pattern, std::string_view value) {
    std::size_t pattern_start = 0, value_start = 0;
    while (pattern_start <= pattern.size()) {
        const auto pattern_end = pattern.find('/', pattern_start);
        const auto value_end = value.find('/', value_start);
        const auto pattern_segment = pattern.substr(pattern_start,
            pattern_end == std::string_view::npos ? pattern.size() - pattern_start : pattern_end - pattern_start);
        const auto value_segment = value.substr(value_start,
            value_end == std::string_view::npos ? value.size() - value_start : value_end - value_start);
        if (pattern_segment.size() >= 2 && pattern_segment.front() == '{' && pattern_segment.back() == '}') {
            if (value_segment.empty()) return false;
        } else if (pattern_segment != value_segment) return false;
        if (pattern_end == std::string_view::npos) return true;
        if (value_end == std::string_view::npos) return false;
        pattern_start = pattern_end + 1;
        value_start = value_end + 1;
    }
    return true;
}

bool mutation_path_matches_mapping(const CampaignMappingDescriptor& mapping, std::string_view path) {
    const auto pattern_parts = split_path_parts(mapping.raw_path_template);
    const auto path_parts = split_path_parts(path);
    if (path_parts.size() < pattern_parts.size()) return false;
    for (std::size_t index = 0; index < pattern_parts.size(); ++index)
        if (!path_part_matches_prefix(pattern_parts[index], path_parts[index])) return false;
    return true;
}

bool district_system_clone_source_allowed(const CampaignDocumentMutation& mutation) {
    if (mutation.semantic_property != "Town.DistrictSystem" ||
        (mutation.kind != CampaignDocumentMutationKind::AppendClone &&
         mutation.kind != CampaignDocumentMutationKind::InsertClone) ||
        mutation.document_id != "persist.town.json") return false;
    constexpr std::string_view root_prefix{"base_root/"};
    if (!mutation.source_path.starts_with(root_prefix)) return false;
    const auto tail = std::string_view{mutation.source_path}.substr(root_prefix.size());
    if (tail.empty()) return false;
    const auto safe_path = [](std::string_view path) {
        std::size_t start = 0;
        while (start < path.size()) {
            const auto end = path.find('/', start);
            const auto segment = path.substr(start, end == std::string_view::npos
                ? path.size() - start : end - start);
            if (segment.empty() || !std::all_of(segment.begin(), segment.end(), [](unsigned char value) {
                    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                           (value >= '0' && value <= '9') || value == '_' || value == '-' ||
                           value == '.' || value == ':';
                })) return false;
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        return true;
    };
    if (!safe_path(tail)) return false;
    if (tail == "districts") return true;

    constexpr std::string_view building_prefix{"buildings/"};
    if (tail.starts_with(building_prefix)) {
        const auto key = tail.substr(building_prefix.size());
        if (!key.empty() && key.find('/') == std::string_view::npos) return true;
    }

    constexpr std::string_view built_suffix{"/built"};
    constexpr std::string_view district_prefix{"base_root/districts/buildings/"};
    if (mutation.expected_kind == ValueKind::Boolean &&
        !mutation.source_path.starts_with("base_root/districts/") &&
        mutation.target_path.starts_with(district_prefix) &&
        mutation.target_path.ends_with(built_suffix)) {
        const auto district_id = std::string_view{mutation.target_path}.substr(
            district_prefix.size(), mutation.target_path.size() - district_prefix.size() - built_suffix.size());
        return !district_id.empty() && district_id.find('/') == std::string_view::npos;
    }
    return false;
}

bool camping_scalar_clone_source_allowed(const CampaignDocumentMutation& mutation) {
    if (mutation.semantic_property != "Hero.SelectedCampingSkills" ||
        mutation.kind != CampaignDocumentMutationKind::AppendClone ||
        mutation.expected_kind != ValueKind::Integer ||
        mutation.document_id != "persist.roster.json") return false;
    const auto* combat = find_mapping("Hero.SelectedCombatSkills");
    return combat != nullptr && mutation_path_matches_mapping(*combat, mutation.source_path);
}

bool purchase_entry_mutation_allowed(const CampaignDocumentMutation& mutation) {
    constexpr std::string_view prefix{"base_root/purchases/"};
    if (mutation.semantic_property != "Upgrade.PurchaseNode.Entry" ||
        !mutation.target_path.starts_with(prefix)) return false;
    const auto tail = std::string_view{mutation.target_path}.substr(prefix.size());
    const auto separator = tail.find('/');
    const auto key = tail.substr(0, separator);
    std::size_t index{};
    const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), index);
    if (key.empty() || error != std::errc{} || end != key.data() + key.size()) return false;
        if (mutation.kind == CampaignDocumentMutationKind::AppendClone) {
        if (separator != std::string_view::npos || mutation.new_key != key ||
            !mutation.source_path.starts_with(prefix)) return false;
        const auto source_key = std::string_view{mutation.source_path}.substr(prefix.size());
        if (source_key.empty() || source_key.find('/') != std::string_view::npos) return false;
        std::size_t source_index{};
        const auto [source_end, source_error] = std::from_chars(
            source_key.data(), source_key.data() + source_key.size(), source_index);
        return source_error == std::errc{} && source_end == source_key.data() + source_key.size() &&
               mutation.expected_kind == ValueKind::Object;
    }
    if (mutation.kind != CampaignDocumentMutationKind::SetValue || separator == std::string_view::npos)
        return false;
    const auto field = tail.substr(separator + 1);
    const auto expected = field == "requirement_code" ? ValueKind::Character :
        field == "is_purchased" ? ValueKind::Boolean :
        (field == "instance_number" || field == "tree_id") ? ValueKind::Integer : ValueKind::Unknown;
    return expected != ValueKind::Unknown && mutation.expected_kind == expected;
}

bool safe_dson_key(std::string_view key) {
    return !key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '.' || value == ':';
    });
}

bool path_is_removed(std::string_view field_path, const std::vector<CampaignDocumentMutation>& mutations) {
    return std::any_of(mutations.begin(), mutations.end(), [&](const auto& mutation) {
        const auto& root = mutation.target_path;
        if (mutation.kind == CampaignDocumentMutationKind::Erase ||
            mutation.kind == CampaignDocumentMutationKind::Rename)
            return field_path == root || field_path.starts_with(root + "/") || field_path.starts_with(root + " => ");
        if (mutation.kind == CampaignDocumentMutationKind::ClearChildren)
            return field_path.starts_with(root + "/") || field_path.starts_with(root + " => ");
        return false;
    });
}

bool path_is_added(std::string_view field_path, const std::vector<CampaignDocumentMutation>& mutations) {
    return std::any_of(mutations.begin(), mutations.end(), [&](const auto& mutation) {
        if (mutation.kind == CampaignDocumentMutationKind::AppendClone ||
            mutation.kind == CampaignDocumentMutationKind::AppendTemplate ||
            mutation.kind == CampaignDocumentMutationKind::CreateObject ||
            mutation.kind == CampaignDocumentMutationKind::InsertClone)
            return field_path == mutation.target_path || field_path.starts_with(mutation.target_path + "/") ||
                   field_path.starts_with(mutation.target_path + " => ");
        if (mutation.kind == CampaignDocumentMutationKind::Rename) {
            const auto slash = mutation.target_path.find_last_of('/');
            if (slash == std::string::npos) return false;
            const auto renamed = mutation.target_path.substr(0, slash + 1) + mutation.new_key;
            return field_path == renamed || field_path.starts_with(renamed + "/") ||
                   field_path.starts_with(renamed + " => ");
        }
        return false;
    });
}

// Appending a hero from the built-in template also advances the roster's
// identity allocator. This is the only existing scalar the template operation
// may change; the expected value is derived from the declared new hero IDs.
bool expected_template_next_guid(const DsonDocument& original,
                                 const std::vector<CampaignDocumentMutation>& mutations,
                                 std::int32_t& expected) {
    const auto original_field = std::find_if(original.fields.begin(), original.fields.end(),
        [](const auto& field) { return field.path == "base_root/nextGuid"; });
    if (original_field == original.fields.end() || original_field->kind != ValueKind::Integer ||
        !std::holds_alternative<std::int32_t>(original_field->value)) return false;
    expected = std::get<std::int32_t>(original_field->value);
    if (expected < 0) return false;
    bool has_template_append = false;
    for (const auto& mutation : mutations) {
        if (mutation.kind != CampaignDocumentMutationKind::AppendTemplate ||
            mutation.semantic_property != "Hero.PersistentId" ||
            mutation.document_id != "persist.roster.json") continue;
        has_template_append = true;
        std::int32_t hero_id{};
        const auto [end, error] = std::from_chars(mutation.new_key.data(),
            mutation.new_key.data() + mutation.new_key.size(), hero_id);
        if (error != std::errc{} || end != mutation.new_key.data() + mutation.new_key.size() ||
            hero_id < 0 || hero_id == std::numeric_limits<std::int32_t>::max()) return false;
        expected = std::max(expected, hero_id + 1);
    }
    return has_template_append;
}

bool validate_decoded_candidate(const DsonDocument& original, const DsonDocument& candidate,
                                const std::vector<CampaignFieldChange>& changes,
                                const std::vector<CampaignStructuralChange>& structural_changes,
                                const std::vector<CampaignDocumentMutation>& mutations,
                                std::string& reason) {
    if (structural_changes.empty() && mutations.empty()) return validate_decoded_candidate(original, candidate, changes, reason);
    const auto& original_header = original.header;
    const auto& candidate_header = candidate.header;
    if (original_header.magic != candidate_header.magic || original_header.revision != candidate_header.revision ||
        original_header.header_length != candidate_header.header_length ||
        original_header.reserved_1 != candidate_header.reserved_1 ||
        original_header.reserved_2 != candidate_header.reserved_2 ||
        original_header.reserved_3 != candidate_header.reserved_3 ||
        original_header.reserved_4 != candidate_header.reserved_4) {
        reason = "Structural candidate changed immutable DSON header fields";
        return false;
    }
    std::map<std::string, std::vector<FieldSnapshot>, std::less<>> before;
    std::map<std::string, std::vector<FieldSnapshot>, std::less<>> after;
    collect_field_snapshots(original, {}, before);
    collect_field_snapshots(candidate, {}, after);
    std::int32_t expected_next_guid{};
    const bool template_updates_next_guid = expected_template_next_guid(
        original, mutations, expected_next_guid);

    for (const auto& change : structural_changes) {
        const auto target = before.find(change.raw.display_path);
        if (target == before.end() || target->second.size() != 1U || after.contains(change.raw.display_path)) {
            reason = "Structural candidate did not remove exactly the requested mapped entry";
            return false;
        }
    }
    for (const auto& [path, fields] : before) {
        if (path_is_removed(path, structural_changes) || path_is_removed(path, mutations)) continue;
        const auto candidate_field = after.find(path);
        if (candidate_field == after.end() || candidate_field->second.size() != fields.size()) {
            reason = "Candidate removed a DSON field outside the structural ChangeSet";
            return false;
        }
        const bool scalar_target = std::any_of(changes.begin(), changes.end(), [&](const auto& change) {
            return change.raw.display_path == path;
        }) || std::any_of(mutations.begin(), mutations.end(), [&](const auto& mutation) {
            return mutation.kind == CampaignDocumentMutationKind::SetValue && mutation.target_path == path;
        });
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto& field = fields[index];
            const auto& current = candidate_field->second[index];
            if (field.name != current.name || field.kind != current.kind) {
                reason = "Candidate changed a DSON field identity outside its mapped ChangeSet: " + path;
                return false;
            }
            if (template_updates_next_guid && path == "base_root/nextGuid") {
                if (fields.size() != 1U || current.kind != ValueKind::Integer ||
                    !std::holds_alternative<std::int32_t>(current.value) ||
                    std::get<std::int32_t>(current.value) != expected_next_guid) {
                    reason = "Candidate roster nextGuid does not match the template-created hero IDs";
                    return false;
                }
                continue;
            }
            if (!scalar_target && field.value != current.value) {
                reason = "Candidate changed a DSON value outside its mapped ChangeSet: " + path;
                return false;
            }
            if (field.kind == ValueKind::Unknown && !scalar_target && field.raw_data != current.raw_data) {
                reason = "Candidate changed raw DSON bytes outside its mapped ChangeSet: " + path;
                return false;
            }
        }
    }
    for (const auto& [path, fields] : after) {
        (void)fields;
        if (!before.contains(path)) {
            if (!path_is_added(path, mutations)) {
                reason = "Candidate added a DSON field outside the structural ChangeSet";
                return false;
            }
        }
    }
    for (const auto& mutation : mutations) {
        if (mutation.kind == CampaignDocumentMutationKind::SetValue &&
            !path_is_removed(mutation.target_path, mutations)) {
            const auto value = locate_field_by_path(const_cast<DsonDocument&>(candidate), mutation.target_path, reason);
            if (!value || value->get().kind != mutation.expected_kind || !mutation.after ||
                !value_matches(value->get(), *mutation.after)) {
                if (reason.empty()) reason = "Read-back value does not match a mapped document mutation";
                return false;
            }
        }
    }
    for (const auto& change : changes) {
        const auto field = locate_field(candidate, change.raw, reason);
        if (!field || !value_matches(field->get(), change.after)) {
            if (reason.empty()) reason = "Read-back value does not match the ChangeSet target value";
            return false;
        }
    }
    return true;
}

bool same_path_or_parent(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    auto normalize = [](const std::filesystem::path& path) {
        std::error_code ec;
        auto absolute = std::filesystem::absolute(path, ec);
        if (ec) absolute = path;
        auto value = absolute.lexically_normal().generic_string();
#ifdef _WIN32
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
#endif
        while (value.size() > 1 && value.back() == '/') value.pop_back();
        return value;
    };
    const auto a = normalize(lhs);
    const auto b = normalize(rhs);
    const auto inside = [](const std::string& child, const std::string& parent) {
        return child == parent || (child.size() > parent.size() && child.starts_with(parent) &&
                                   child[parent.size()] == '/');
    };
    return inside(a, b) || inside(b, a);
}

core::Result<DirectoryTree, core::Error> collect_tree(IFileSystem& file_system,
                                                       const std::filesystem::path& root) {
    DirectoryTree tree;
    std::vector<std::filesystem::path> pending{root};
    std::set<std::string, std::less<>> visited;
    while (!pending.empty()) {
        auto directory = std::move(pending.back());
        pending.pop_back();
        const auto relative_directory = directory.lexically_relative(root);
        if (std::distance(relative_directory.begin(), relative_directory.end()) > 64)
            return core::Result<DirectoryTree, core::Error>::failure(
                adapter_error(core::ErrorCode::IoError, "Profile directory nesting exceeds the safe traversal limit",
                              {{"path", directory.string()}}));
        if (!visited.insert(directory.lexically_normal().generic_string()).second) continue;
        if (visited.size() > 100000)
            return core::Result<DirectoryTree, core::Error>::failure(
                adapter_error(core::ErrorCode::IoError, "Profile tree exceeds the safe traversal limit",
                              {{"path", root.string()}}));
        auto files = file_system.list_files(directory);
        if (!files) return core::Result<DirectoryTree, core::Error>::failure(files.error());
        tree.files.insert(tree.files.end(), files.value().begin(), files.value().end());
        auto directories = file_system.list_directories(directory);
        if (!directories) return core::Result<DirectoryTree, core::Error>::failure(directories.error());
        for (const auto& child : directories.value()) {
            tree.directories.push_back(child);
            pending.push_back(child);
        }
    }
    return core::Result<DirectoryTree, core::Error>::success(std::move(tree));
}

bool safe_relative_path(const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute()) return false;
    return relative.begin() == relative.end() || *relative.begin() != "..";
}

core::Result<void, core::Error> create_profile_backup(IFileSystem& file_system,
                                                       const std::filesystem::path& source,
                                                       const std::filesystem::path& backup) {
    auto exists = file_system.exists(backup);
    if (!exists) return core::Result<void, core::Error>::failure(backup_error(exists.error(), backup));
    if (exists.value())
        return core::Result<void, core::Error>::failure(
            backup_error(adapter_error(core::ErrorCode::BackupFailed, "Backup destination already exists",
                                       {{"path", backup.string()}}), backup));

    auto tree = collect_tree(file_system, source);
    if (!tree) return core::Result<void, core::Error>::failure(backup_error(tree.error(), backup));
    auto created = file_system.create_directories(backup);
    if (!created) return core::Result<void, core::Error>::failure(backup_error(created.error(), backup));

    for (const auto& directory : tree.value().directories) {
        const auto relative = directory.lexically_relative(source);
        if (!safe_relative_path(relative))
            return core::Result<void, core::Error>::failure(
                backup_error(adapter_error(core::ErrorCode::BackupFailed, "Profile directory escaped its root"), backup));
        auto made = file_system.create_directories(backup / relative);
        if (!made) return core::Result<void, core::Error>::failure(backup_error(made.error(), backup));
    }
    for (const auto& file : tree.value().files) {
        const auto relative = file.lexically_relative(source);
        if (!safe_relative_path(relative))
            return core::Result<void, core::Error>::failure(
                backup_error(adapter_error(core::ErrorCode::BackupFailed, "Profile file escaped its root"), backup));
        const auto contents = file_system.read_file(file);
        if (!contents) return core::Result<void, core::Error>::failure(backup_error(contents.error(), backup));
        auto made = file_system.create_directories((backup / relative).parent_path());
        if (!made) return core::Result<void, core::Error>::failure(backup_error(made.error(), backup));
        auto written = file_system.write_file(backup / relative, contents.value());
        if (!written) return core::Result<void, core::Error>::failure(backup_error(written.error(), backup));
    }

    const auto source_fingerprint = SaveProfileDiscovery::fingerprint_profile(file_system, source);
    const auto backup_fingerprint = SaveProfileDiscovery::fingerprint_profile(file_system, backup);
    if (!source_fingerprint) return core::Result<void, core::Error>::failure(backup_error(source_fingerprint.error(), backup));
    if (!backup_fingerprint) return core::Result<void, core::Error>::failure(backup_error(backup_fingerprint.error(), backup));
    if (source_fingerprint.value() != backup_fingerprint.value())
        return core::Result<void, core::Error>::failure(
            backup_error(adapter_error(core::ErrorCode::BackupFailed,
                                       "Backup fingerprint differs from the source profile"), backup));
    return core::Result<void, core::Error>::success();
}

core::Result<void, core::Error> verify_output_copy(const RawSaveProfile& profile, IFileSystem& file_system,
                                                    const std::filesystem::path& target) {
    const auto fingerprint = SaveProfileDiscovery::fingerprint_profile(file_system, target);
    if (!fingerprint) return core::Result<void, core::Error>::failure(fingerprint.error());
    if (fingerprint.value() != profile.baseline_fingerprint)
        return core::Result<void, core::Error>::failure(
            {core::ErrorCode::ConcurrentSaveChanged,
             "The output profile copy does not match the loaded source snapshot", "SafeSaveCommitter",
             {{"source_profile", profile.descriptor.root_path.string()}, {"target_profile", target.string()}}});
    return core::Result<void, core::Error>::success();
}

std::string bytes_to_string(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

ChangeSet effective_hero_deletion_changes(const ChangeSet& requested) {
    ChangeSet result = requested;
    std::set<std::string, std::less<>> deleted_hero_paths;
    for (const auto& batch : requested.document_mutation_batches) {
        if (batch.operation_id != "campaign.hero.delete" || batch.cancel) continue;
        for (const auto& mutation : batch.mutations)
            if (mutation.kind == CampaignDocumentMutationKind::Erase &&
                mutation.semantic_property == "Hero.PersistentId" &&
                mutation.document_id == "persist.roster.json" &&
                mutation.target_path.starts_with("base_root/heroes/"))
                deleted_hero_paths.insert(mutation.target_path);
    }
    const auto inside_deleted_hero = [&](const std::string& document_id, const std::string& path) {
        if (document_id != "persist.roster.json") return false;
        return std::any_of(deleted_hero_paths.begin(), deleted_hero_paths.end(), [&](const auto& hero_path) {
            return path.starts_with(hero_path + "/");
        });
    };
    std::erase_if(result.changes, [&](const auto& change) {
        return inside_deleted_hero(change.raw.document_id, change.raw.display_path);
    });
    std::erase_if(result.structural_changes, [&](const auto& change) {
        return inside_deleted_hero(change.raw.document_id, change.raw.display_path);
    });
    return result;
}

} // namespace

core::Result<SaveCandidate, core::Error>
SaveAdapter::build_candidate(const RawSaveProfile& profile, const ChangeSet& requested_changes) const {
    const auto normalized = effective_hero_deletion_changes(requested_changes);
    const auto& changes = normalized;
    if (changes.empty())
        return core::Result<SaveCandidate, core::Error>::failure(
            adapter_error(core::ErrorCode::ValidationFailed, "Cannot build a save candidate from an empty ChangeSet"));

    std::set<std::string, std::less<>> template_created_hero_ids;
    for (const auto& batch : changes.document_mutation_batches) {
        if (batch.operation_id != "campaign.hero.add") continue;
        for (const auto& mutation : batch.mutations) {
            if (mutation.kind == CampaignDocumentMutationKind::AppendTemplate &&
                mutation.semantic_property == "Hero.PersistentId" &&
                mutation.document_id == "persist.roster.json")
                template_created_hero_ids.insert(mutation.new_key);
        }
    }

    std::map<std::string, std::vector<CampaignFieldChange>, std::less<>> grouped_fields;
    std::map<std::string, std::vector<CampaignStructuralChange>, std::less<>> grouped_structural;
    std::map<std::string, std::vector<CampaignDocumentMutation>, std::less<>> grouped_mutations;
    std::set<std::string, std::less<>> expected_documents;
    std::set<std::string, std::less<>> unique_targets;
    std::map<std::string, std::vector<CampaignFieldChange>, std::less<>> template_created_hero_fields;
    for (const auto& change : changes.changes) {
        const auto* mapping = find_mapping(change.target.semantic_property);
        const bool built_in_template_field = change.target.semantic_property.starts_with("Hero.") &&
            template_created_hero_ids.contains(change.target.entity_id) &&
            change.raw.document_id == "persist.roster.json" &&
            change.raw.steps.empty() &&
            change.raw.display_path.starts_with("base_root/heroes/" + change.target.entity_id +
                "/hero_file_data/raw_data => base_root/");
        if (mapping == nullptr || mapping->capability() < CampaignMappingCapability::CandidateWritable)
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "ChangeSet contains a property without an implemented writable mapping",
                              {{"property", change.target.semantic_property}}));
        if (mapping->document_id != change.raw.document_id ||
            (change.raw.steps.empty() && !built_in_template_field) ||
            change.raw.display_path != expand_path(mapping->raw_path_template, change.target))
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "ChangeSet locator does not match the registered mapping",
                              {{"property", change.target.semantic_property},
                               {"document", change.raw.document_id}, {"path", change.raw.display_path},
                               {"expected_path", expand_path(mapping->raw_path_template, change.target)},
                               {"locator_step_count", std::to_string(change.raw.steps.size())}}));
        if (built_in_template_field) {
            if (!type_matches(mapping->expected_type, change.before) ||
                !type_matches(mapping->expected_type, change.after) || change.before == change.after)
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::ValidationFailed,
                                  "Template-created hero field has an invalid mapped value",
                                  {{"hero_id", change.target.entity_id},
                                   {"property", change.target.semantic_property}}));
            template_created_hero_fields[change.target.entity_id].push_back(change);
            const auto target_key = change.raw.document_id + "|" + change.raw.display_path;
            if (!unique_targets.insert(target_key).second)
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::ValidationFailed,
                                  "ChangeSet contains a duplicate raw target", {{"path", change.raw.display_path}}));
            expected_documents.insert(change.raw.document_id);
            // This scalar is written into the built-in hero template before it
            // is appended. It is not an indexed field in the original save.
            continue;
        }
        if (change.raw.steps.empty())
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "ChangeSet locator has no field steps",
                              {{"property", change.target.semantic_property}, {"path", change.raw.display_path}}));
        if (change.raw.steps.back().field_name != expected_leaf(*mapping))
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "ChangeSet leaf field does not match the registered mapping",
                              {{"property", change.target.semantic_property}, {"path", change.raw.display_path}}));
        if (!type_matches(mapping->expected_type, change.before) ||
            !type_matches(mapping->expected_type, change.after) || change.before == change.after)
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::ValidationFailed,
                              "ChangeSet values do not match the mapped type or contain a no-op",
                              {{"property", change.target.semantic_property}}));
        const auto target_key = change.raw.document_id + "|" + change.raw.display_path;
        if (!unique_targets.insert(target_key).second)
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::ValidationFailed, "ChangeSet contains a duplicate raw target",
                              {{"path", change.raw.display_path}}));

        expected_documents.insert(change.raw.document_id);
        grouped_fields[change.raw.document_id].push_back(change);
    }

    for (const auto& change : changes.structural_changes) {
        const auto* mapping = find_mapping(change.target.semantic_property);
        if (mapping == nullptr || mapping->capability() < CampaignMappingCapability::CandidateWritable ||
            mapping->document_id != change.raw.document_id ||
            change.raw.display_path != expand_path(mapping->raw_path_template, change.target) ||
            change.action != CampaignStructuralAction::Erase ||
            change.expected_kind != mapping->expected_type ||
            change.raw.steps.empty())
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "Structural ChangeSet does not match a registered writable object mapping",
                              {{"property", change.target.semantic_property},
                               {"document", change.raw.document_id}, {"path", change.raw.display_path}}));
        const auto target_key = change.raw.document_id + "|" + change.raw.display_path;
        if (!unique_targets.insert(target_key).second)
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::ValidationFailed, "ChangeSet contains a duplicate structural target",
                              {{"path", change.raw.display_path}}));
        expected_documents.insert(change.raw.document_id);
        grouped_structural[change.raw.document_id].push_back(change);
    }

    auto ordered_mutation_batches = changes.document_mutation_batches;
    const auto trinket_batch_order = [](std::string_view id) {
        if (id == "campaign.trinket.add_inventory") return 0;
        if (id == "campaign.trinket.destroy") return 1;
        if (id == "campaign.trinket.reorder_inventory") return 2;
        return 1;
    };
    std::stable_sort(ordered_mutation_batches.begin(), ordered_mutation_batches.end(),
        [&](const auto& left, const auto& right) {
            return trinket_batch_order(left.operation_id) < trinket_batch_order(right.operation_id);
        });
    for (const auto& batch : ordered_mutation_batches) {
        if (batch.cancel || batch.mutations.empty())
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::ValidationFailed,
                              "Canceled or empty document mutation batch reached the SaveAdapter",
                              {{"operation", batch.operation_id}}));
        const auto capability = std::find_if(campaign_operation_capabilities().begin(),
            campaign_operation_capabilities().end(), [&](const auto& item) {
                return item.operation_id == batch.operation_id &&
                       item.availability == CampaignOperationAvailability::Available;
            });
        if (capability == campaign_operation_capabilities().end())
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "Document mutation operation has not passed its in-game validation gate",
                              {{"operation", batch.operation_id}}));
        for (const auto& mutation : batch.mutations) {
            if (batch.operation_id == "campaign.hero.add" &&
                (mutation.kind != CampaignDocumentMutationKind::AppendTemplate ||
                 mutation.semantic_property != "Hero.PersistentId"))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable,
                                  "Hero creation only accepts built-in template appends"));
            const auto* mapping = find_mapping(mutation.semantic_property);
            if (mapping == nullptr || mapping->capability() != CampaignMappingCapability::CommitWritable ||
                mapping->document_id != mutation.document_id || mutation.target_path.empty() ||
                !mutation_path_matches_mapping(*mapping, mutation.target_path))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable,
                                  "Document mutation path is outside the registered game-verified mapping",
                                  {{"property", mutation.semantic_property}, {"path", mutation.target_path}}));
            const bool valid_builtin_hero_template = batch.operation_id == "campaign.hero.add" &&
                mutation.kind == CampaignDocumentMutationKind::AppendTemplate &&
                mutation.semantic_property == "Hero.PersistentId" && mutation.document_id == "persist.roster.json" &&
                mutation.source_path == "base_root/heroes/1" && mutation.expected_kind == ValueKind::Object &&
                mutation.template_document != nullptr && safe_dson_key(mutation.new_key) &&
                mutation.target_path == "base_root/heroes/" + mutation.new_key &&
                safe_dson_key(mutation.template_hero_class) && mutation.template_base_hit_points > 0.0F &&
                !mutation.template_combat_skills.empty();
            if (mutation.kind == CampaignDocumentMutationKind::AppendTemplate && !valid_builtin_hero_template)
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable, "Built-in hero template mutation is invalid",
                                  {{"path", mutation.target_path}}));
            if (valid_builtin_hero_template) {
                const auto expected_template = build_blank_level_zero_hero_template(
                    mutation.template_hero_class, mutation.template_class_name, mutation.template_combat_skills,
                    mutation.template_camping_skills, mutation.template_base_hit_points);
                const auto supplied_bytes = core::dson::DsonWriter{}.encode(*mutation.template_document);
                const auto expected_bytes = expected_template
                    ? core::dson::DsonWriter{}.encode(*expected_template.value())
                    : core::Result<std::vector<std::byte>, core::Error>::failure(expected_template.error());
                if (!supplied_bytes || !expected_bytes || supplied_bytes.value() != expected_bytes.value())
                    return core::Result<SaveCandidate, core::Error>::failure(
                        adapter_error(core::ErrorCode::MappingNotWritable,
                                      "Hero template does not match the versioned built-in template",
                                      {{"path", mutation.target_path}}));
            }
            if ((mutation.kind == CampaignDocumentMutationKind::AppendClone ||
                 mutation.kind == CampaignDocumentMutationKind::InsertClone) &&
                !mutation_path_matches_mapping(*mapping, mutation.source_path) &&
                !district_system_clone_source_allowed(mutation) &&
                !camping_scalar_clone_source_allowed(mutation))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable,
                                  "Clone source is outside the registered mapping",
                                  {{"property", mutation.semantic_property}, {"path", mutation.source_path}}));
            if (mutation.semantic_property == "Upgrade.PurchaseNode.Entry" &&
                !purchase_entry_mutation_allowed(mutation))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable,
                                  "Purchase-node document mutation is outside its typed row/field allowlist",
                                  {{"path", mutation.target_path}}));
            if ((mutation.kind == CampaignDocumentMutationKind::AppendClone ||
                 mutation.kind == CampaignDocumentMutationKind::InsertClone) &&
                (mutation.source_path.empty() || !safe_dson_key(mutation.new_key) ||
                 mutation.expected_kind == ValueKind::Unknown ||
                 (mutation.kind == CampaignDocumentMutationKind::InsertClone && !mutation.insertion_index)))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::ValidationFailed, "Append-clone mutation payload is invalid"));
            if (mutation.kind == CampaignDocumentMutationKind::CreateObject &&
                ((mutation.semantic_property != "TrinketInventory.Items" && mutation.semantic_property != "Hero.Trinkets" &&
                  mutation.semantic_property != "Hero.Quirks") ||
                 mutation.expected_kind != ValueKind::Object || !safe_dson_key(mutation.new_key) ||
                 (mutation.semantic_property == "TrinketInventory.Items"
                     ? mutation.target_path != "base_root/trinkets/items/" + mutation.new_key
                     : mutation.semantic_property == "Hero.Trinkets"
                         ? !mutation.target_path.ends_with(" => base_root/trinkets/items/" + mutation.new_key)
                         : !mutation.target_path.ends_with(" => base_root/quirks/" + mutation.new_key))))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::ValidationFailed, "Create-object mutation payload is invalid",
                                  {{"path", mutation.target_path}}));
            if (mutation.kind == CampaignDocumentMutationKind::Rename && !safe_dson_key(mutation.new_key))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::ValidationFailed, "Rename mutation key is invalid"));
            if (mutation.kind == CampaignDocumentMutationKind::SetValue &&
                (!mutation.before || !mutation.after ||
                 !type_matches(mutation.expected_kind, *mutation.before) ||
                 !type_matches(mutation.expected_kind, *mutation.after)))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::ValidationFailed, "Set-value mutation payload is invalid"));
            if ((mutation.kind == CampaignDocumentMutationKind::Erase && mutation.expected_kind == ValueKind::Unknown) ||
                (mutation.kind == CampaignDocumentMutationKind::ClearChildren && mutation.expected_kind != ValueKind::Object))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::ValidationFailed, "Structural mutation value kind is invalid"));
            expected_documents.insert(mutation.document_id);
            grouped_mutations[mutation.document_id].push_back(mutation);
        }
    }

    const std::set<std::string, std::less<>> declared_documents(changes.affected_documents.begin(),
                                                                changes.affected_documents.end());
    if (declared_documents != expected_documents || declared_documents.size() != changes.affected_documents.size())
        return core::Result<SaveCandidate, core::Error>::failure(
            adapter_error(core::ErrorCode::ValidationFailed,
                          "ChangeSet affected_documents does not exactly match its value and structural changes"));

    SaveCandidate candidate;
    core::dson::DsonWriter writer;
    core::dson::DsonReader reader;
    for (const auto& document_id : expected_documents) {
        const auto& document_changes = grouped_fields[document_id];
        const auto& structural_changes = grouped_structural[document_id];
        const auto& document_mutations = grouped_mutations[document_id];
        const auto source = profile.documents.find(document_id);
        if (source == profile.documents.end() || !source->second.decoded)
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "A changed document is missing or was not decoded",
                              {{"document", document_id}}));
        const auto& original = *source->second.decoded;
        if (document_has_dirty_fields(original))
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::ValidationFailed,
                              "Loaded DSON already contains untracked dirty fields",
                              {{"document", document_id}}));
        const auto original_bytes = bytes_to_string(original.original_bytes);
        if (original_bytes != source->second.bytes)
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::ConcurrentSaveChanged,
                              "Decoded document bytes differ from the loaded raw document",
                              {{"document", document_id}}));

        auto cloned = clone_document(original);
        auto source_snapshot = clone_document(original);
        std::size_t active_mutation_index{};
        const auto clone_entry = [&](const CampaignDocumentMutation& mutation, bool insert_at_position)
            -> core::Result<void, core::Error> {
            std::string reason;
            if (locate_field_by_path(cloned, mutation.target_path, reason))
                return core::Result<void, core::Error>::failure(
                    adapter_error(core::ErrorCode::ConcurrentSaveChanged,
                                  "Clone destination already exists",
                                  {{"document", document_id}, {"path", mutation.target_path}}));
            reason.clear();
            DsonDocument prepared_template;
            DsonDocument* source_document_tree = &source_snapshot;
            // Roster reordering must carry earlier edits to a hero in this session.
            // The original snapshot would restore stale name/skill/equipment bytes.
            if (mutation.kind == CampaignDocumentMutationKind::AppendClone &&
                mutation.semantic_property == "Hero.PersistentId")
                source_document_tree = &cloned;
            if (mutation.kind == CampaignDocumentMutationKind::AppendTemplate) {
                if (mutation.template_document == nullptr)
                    return core::Result<void, core::Error>::failure(
                        adapter_error(core::ErrorCode::MappingNotWritable, "Built-in hero template is unavailable"));
                prepared_template = clone_document(*mutation.template_document);
                source_document_tree = &prepared_template;
                const auto desired_fields = template_created_hero_fields.find(mutation.new_key);
                if (desired_fields != template_created_hero_fields.end())
                    for (const auto& change : desired_fields->second) {
                        const auto template_path = mutation.source_path +
                            change.raw.display_path.substr(mutation.target_path.size());
                        reason.clear();
                        auto field = locate_field_by_path(prepared_template, template_path, reason);
                        const auto* mapping = find_mapping(change.target.semantic_property);
                        if (!field || !mapping || field->get().kind != mapping->expected_type ||
                            !value_matches(field->get(), change.before))
                        return core::Result<void, core::Error>::failure(
                            adapter_error(core::ErrorCode::MappingNotWritable,
                                          reason.empty() ? "Built-in hero template field is invalid" : reason,
                                          {{"hero_id", mutation.new_key}, {"path", template_path}}));
                        replace_field_value(field->get(), change.after);
                    }
            }
            if (source_document_tree == nullptr)
                return core::Result<void, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable, "Built-in hero template is unavailable"));
            auto source_field = locate_field_by_path(*source_document_tree, mutation.source_path, reason);
            if (!source_field) {
                // Some ordered batches intentionally clone a container created by an earlier
                // mutation in this same batch (for example districts -> districts/buildings).
                reason.clear();
                if (mutation.kind != CampaignDocumentMutationKind::AppendTemplate) {
                    source_field = locate_field_by_path(cloned, mutation.source_path, reason);
                    if (source_field) source_document_tree = &cloned;
                }
            }
            if (!source_field || source_field->get().kind != mutation.expected_kind)
                return core::Result<void, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable,
                                  reason.empty() ? "Clone source has a different DSON value kind" : reason,
                                  {{"document", document_id}, {"path", mutation.source_path},
                                   {"source_path", mutation.source_path},
                                   {"target_path", mutation.target_path},
                                   {"mutation_index", std::to_string(active_mutation_index)},
                                   {"phase", "clone_source_lookup"}}));
            reason.clear();
            const auto slash = mutation.target_path.find_last_of('/');
            if (slash == std::string::npos || mutation.target_path.substr(slash + 1) != mutation.new_key)
                return core::Result<void, core::Error>::failure(
                    adapter_error(core::ErrorCode::ValidationFailed,
                                  "Clone key does not match the mapped destination path",
                                  {{"path", mutation.target_path}}));
            auto destination = locate_document_for_path(cloned, mutation.target_path.substr(0, slash), reason);
            auto source_document = locate_document_for_path(*source_document_tree, mutation.source_path, reason);
            if (!destination || !source_document)
                return core::Result<void, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable, std::move(reason),
                                  {{"document", document_id}, {"path", mutation.target_path}}));
            const auto result = insert_at_position
                ? core::dson::DsonDocumentEditor::insert_clone_at(
                    destination->first.get(), destination->second,
                    source_document->first.get(), source_document->second,
                    mutation.new_key, *mutation.insertion_index)
                : core::dson::DsonDocumentEditor::append_clone(
                    destination->first.get(), destination->second,
                    source_document->first.get(), source_document->second, mutation.new_key);
            if (!result) return core::Result<void, core::Error>::failure(result.error());
            if (mutation.kind == CampaignDocumentMutationKind::AppendTemplate) {
                const auto next_guid_field = locate_field_by_path(cloned, "base_root/nextGuid", reason);
                std::uint64_t hero_guid{};
                const auto [end, parse_error] = std::from_chars(
                    mutation.new_key.data(), mutation.new_key.data() + mutation.new_key.size(), hero_guid);
                if (!next_guid_field || next_guid_field->get().kind != ValueKind::Integer ||
                    parse_error != std::errc{} || end != mutation.new_key.data() + mutation.new_key.size() ||
                    hero_guid >= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
                    return core::Result<void, core::Error>::failure(
                        adapter_error(core::ErrorCode::ValidationFailed,
                                      "Built-in hero append has an invalid persistent ID or missing nextGuid"));
                auto& next_guid = next_guid_field->get();
                if (!std::holds_alternative<std::int32_t>(next_guid.value))
                    return core::Result<void, core::Error>::failure(
                        adapter_error(core::ErrorCode::ConcurrentSaveChanged,
                                      "Roster nextGuid has an unexpected value type"));
                const auto current_next = std::get<std::int32_t>(next_guid.value);
                const auto desired_next = static_cast<std::int32_t>(hero_guid + 1);
                if (current_next < desired_next) next_guid.replace_value(desired_next);
            }
            return core::Result<void, core::Error>::success();
        };
        std::vector<const CampaignFieldChange*> deferred_field_changes;
        for (const auto& change : document_changes) {
            std::string reason;
            auto field = locate_field(cloned, change.raw, reason);
            if (!field) {
                deferred_field_changes.push_back(&change);
                continue;
            }
            auto& target = field->get();
            const auto* mapping = find_mapping(change.target.semantic_property);
            if (!purchase_target_matches(cloned, change.target, change.raw.display_path, reason))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable, std::move(reason),
                                  {{"document", document_id}, {"path", change.raw.display_path}}));
            if (target.kind != mapping->expected_type || !value_matches(target, change.before))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::ConcurrentSaveChanged,
                                  "DSON field no longer matches the ChangeSet before-value",
                                  {{"document", document_id}, {"path", change.raw.display_path}}));
            replace_field_value(target, change.after);
        }
        for (const auto& change : structural_changes) {
            std::string reason;
            auto located = locate_field_by_path(cloned, change.raw.display_path, reason);
            if (!located || located->get().kind != change.expected_kind)
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::ConcurrentSaveChanged,
                                  reason.empty() ? "Mapped structural target has a different DSON type" : reason,
                                  {{"document", document_id}, {"path", change.raw.display_path}}));
            auto containing = locate_document_for_path(cloned, change.raw.display_path, reason);
            if (!containing)
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable, std::move(reason),
                                  {{"document", document_id}, {"path", change.raw.display_path}}));
            auto erased = core::dson::DsonDocumentEditor::erase(containing->first.get(), containing->second);
            if (!erased)
                return core::Result<SaveCandidate, core::Error>::failure(erased.error());
        }
        active_mutation_index = 0;
        for (const auto& mutation : document_mutations) {
            ++active_mutation_index;
            std::string reason;
            if (mutation.kind == CampaignDocumentMutationKind::AppendClone ||
                mutation.kind == CampaignDocumentMutationKind::AppendTemplate ||
                mutation.kind == CampaignDocumentMutationKind::InsertClone) {
                auto appended = clone_entry(mutation, mutation.kind == CampaignDocumentMutationKind::InsertClone);
                if (!appended) {
                    auto error = appended.error();
                    error.context["semantic_property"] = mutation.semantic_property;
                    error.context["mutation_index"] = std::to_string(active_mutation_index);
                    error.context["target_path"] = mutation.target_path;
                    error.context["source_path"] = mutation.source_path;
                    return core::Result<SaveCandidate, core::Error>::failure(std::move(error));
                }
            } else if (mutation.kind == CampaignDocumentMutationKind::CreateObject) {
                const std::vector<std::pair<std::string, core::dson::Value>> trinket_fields{
                    {"id", std::string{}},
                    {"type", std::string{"trinket"}},
                    {"amount", std::int32_t{1}},
                    {"added_buffs", std::int32_t{0}},
                    {"hero_name", std::string{}},
                    {"previous_trinket_id", std::string{}},
                    {"did_transform", false},
                    {"trinkets_gained_count", std::int32_t{0}},
                };
                const std::vector<std::pair<std::string, core::dson::Value>> quirk_fields{
                    {"is_new", true}, {"is_locked", false}, {"trinketId", std::int32_t{0}},
                    {"mission_count", std::int32_t{0}}, {"replaces_quirk", std::int32_t{0}},
                    {"replaces_quirk_viewed", false}, {"evolution_duration_remaining", std::int32_t{0}},
                };
                const auto parent_path = mutation.target_path.substr(0, mutation.target_path.find_last_of('/'));
                auto destination = locate_document_for_path(cloned, parent_path, reason);
                if (!destination)
                    return core::Result<SaveCandidate, core::Error>::failure(
                        adapter_error(core::ErrorCode::MappingNotWritable, std::move(reason),
                                      {{"path", parent_path}}));
                auto created = mutation.insertion_index && mutation.semantic_property == "Hero.Quirks"
                    ? core::dson::DsonDocumentEditor::insert_object_at(
                        destination->first.get(), destination->second, mutation.new_key,
                        quirk_fields, *mutation.insertion_index)
                    : core::dson::DsonDocumentEditor::append_object(
                        destination->first.get(), destination->second, mutation.new_key,
                        mutation.semantic_property == "Hero.Quirks" ? quirk_fields : trinket_fields);
                if (!created) {
                    auto error = created.error();
                    error.context["semantic_property"] = mutation.semantic_property;
                    error.context["mutation_index"] = std::to_string(active_mutation_index);
                    error.context["target_path"] = mutation.target_path;
                    return core::Result<SaveCandidate, core::Error>::failure(std::move(error));
                }
            } else if (mutation.kind == CampaignDocumentMutationKind::Erase) {
                auto target = locate_field_by_path(cloned, mutation.target_path, reason);
                if (!target || target->get().kind != mutation.expected_kind)
                    return core::Result<SaveCandidate, core::Error>::failure(
                        adapter_error(core::ErrorCode::ConcurrentSaveChanged,
                                      reason.empty() ? "Erase target has a different DSON value kind" : reason,
                                      {{"document", document_id}, {"path", mutation.target_path}}));
                auto containing = locate_document_for_path(cloned, mutation.target_path, reason);
                if (!containing)
                    return core::Result<SaveCandidate, core::Error>::failure(
                        adapter_error(core::ErrorCode::MappingNotWritable, std::move(reason)));
                auto erased = core::dson::DsonDocumentEditor::erase(containing->first.get(), containing->second);
                if (!erased) return core::Result<SaveCandidate, core::Error>::failure(erased.error());
            } else if (mutation.kind == CampaignDocumentMutationKind::Rename) {
                auto target = locate_field_by_path(cloned, mutation.target_path, reason);
                if (!target || target->get().kind != mutation.expected_kind)
                    return core::Result<SaveCandidate, core::Error>::failure(
                        adapter_error(core::ErrorCode::ConcurrentSaveChanged,
                                      reason.empty() ? "Rename target has a different DSON value kind" : reason,
                                      {{"document", document_id}, {"path", mutation.target_path}}));
                const auto slash = mutation.target_path.find_last_of('/');
                if (slash == std::string::npos)
                    return core::Result<SaveCandidate, core::Error>::failure(
                        adapter_error(core::ErrorCode::ValidationFailed, "Rename target has no collection parent"));
                const auto new_path = mutation.target_path.substr(0, slash + 1) + mutation.new_key;
                if (locate_field_by_path(cloned, new_path, reason))
                    return core::Result<SaveCandidate, core::Error>::failure(
                        adapter_error(core::ErrorCode::ConcurrentSaveChanged, "Rename destination already exists",
                                      {{"path", new_path}}));
                auto containing = locate_document_for_path(cloned, mutation.target_path, reason);
                if (!containing)
                    return core::Result<SaveCandidate, core::Error>::failure(
                        adapter_error(core::ErrorCode::MappingNotWritable, std::move(reason)));
                auto renamed = core::dson::DsonDocumentEditor::rename(
                    containing->first.get(), containing->second, mutation.new_key);
                if (!renamed) return core::Result<SaveCandidate, core::Error>::failure(renamed.error());
            } else if (mutation.kind == CampaignDocumentMutationKind::ClearChildren) {
                auto container = locate_field_by_path(cloned, mutation.target_path, reason);
                if (!container || container->get().kind != ValueKind::Object)
                    return core::Result<SaveCandidate, core::Error>::failure(
                        adapter_error(core::ErrorCode::MappingNotWritable,
                                      reason.empty() ? "Clear-children target is not an object" : reason,
                                      {{"path", mutation.target_path}}));
                auto containing = locate_document_for_path(cloned, mutation.target_path, reason);
                if (!containing)
                    return core::Result<SaveCandidate, core::Error>::failure(
                        adapter_error(core::ErrorCode::MappingNotWritable, std::move(reason)));
                while (true) {
                    auto current = locate_field_by_path(cloned, mutation.target_path, reason);
                    if (!current || current->get().children.empty()) break;
                    const auto child_index = current->get().children.front();
                    const auto local_child_path = containing->first.get().fields.at(child_index).path;
                    auto erased = core::dson::DsonDocumentEditor::erase(containing->first.get(), local_child_path);
                    if (!erased) return core::Result<SaveCandidate, core::Error>::failure(erased.error());
                }
            } else if (mutation.kind == CampaignDocumentMutationKind::SetValue) {
                auto target = locate_field_by_path(cloned, mutation.target_path, reason);
                if (!target || target->get().kind != mutation.expected_kind || !mutation.before || !mutation.after ||
                    !value_matches(target->get(), *mutation.before))
                    return core::Result<SaveCandidate, core::Error>::failure(
                        adapter_error(core::ErrorCode::ConcurrentSaveChanged,
                                      reason.empty() ? "Set-value target no longer matches its mapped before-value" : reason,
                                      {{"document", document_id}, {"path", mutation.target_path}}));
                replace_field_value(target->get(), *mutation.after);
            }
        }
        // A later edit in the same session may target a row created by an earlier
        // pending collection mutation. Resolve those scalar edits after the ordered
        // structural operations have materialized their targets.
        for (const auto* change : deferred_field_changes) {
            std::string reason;
            auto field = locate_field(cloned, change->raw, reason);
            if (!field)
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable, std::move(reason),
                                  {{"document", document_id}, {"path", change->raw.display_path}}));
            auto& target = field->get();
            const auto* mapping = find_mapping(change->target.semantic_property);
            if (mapping == nullptr || !purchase_target_matches(cloned, change->target,
                                                               change->raw.display_path, reason))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable,
                                  mapping == nullptr ? "The changed field has no registered mapping" : std::move(reason),
                                  {{"document", document_id}, {"path", change->raw.display_path}}));
            if (target.kind != mapping->expected_type || !value_matches(target, change->before))
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::ConcurrentSaveChanged,
                                  "DSON field no longer matches the ChangeSet before-value after collection edits",
                                  {{"document", document_id}, {"path", change->raw.display_path}}));
            replace_field_value(target, change->after);
        }

        if (document_id == "persist.roster.json")
            for (const auto& [hero_id, fields] : template_created_hero_fields)
                for (const auto& change : fields) {
                    std::string reason;
                    auto field = locate_field_by_path(cloned, change.raw.display_path, reason);
                    if (!field || !value_matches(field->get(), change.after))
                        return core::Result<SaveCandidate, core::Error>::failure(
                            adapter_error(core::ErrorCode::ValidationFailed,
                                          reason.empty() ? "Built-in hero template edit did not survive materialization" : reason,
                                          {{"hero_id", hero_id}, {"path", change.raw.display_path}}));
                }

        auto encoded = writer.encode(cloned);
        if (!encoded)
            return core::Result<SaveCandidate, core::Error>::failure(encoded.error());
        const auto bytes = bytes_to_string(encoded.value());
        const auto* data = reinterpret_cast<const std::byte*>(bytes.data());
        auto decoded = reader.parse(std::span<const std::byte>{data, bytes.size()}, document_id);
        if (!decoded)
            return core::Result<SaveCandidate, core::Error>::failure(decoded.error());
        std::string reason;
        if (!validate_decoded_candidate(original, decoded.value(), document_changes, structural_changes,
                                        document_mutations, reason))
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::ValidationFailed, std::move(reason), {{"document", document_id}}));

        auto diff = writer.compare_binary(original, encoded.value());
        if (diff.binary_identical)
            continue;
        CandidateDocument output;
        output.id = document_id;
        output.bytes = bytes;
        output.binary_diff = std::move(diff);
        for (const auto& change : document_changes) output.expected_field_paths.push_back(change.raw.display_path);
        for (const auto& change : structural_changes) output.expected_field_paths.push_back(change.raw.display_path);
        for (const auto& mutation : document_mutations) output.expected_field_paths.push_back(mutation.target_path);
        for (const auto& [hero_id, fields] : template_created_hero_fields)
            if (document_id == "persist.roster.json")
                for (const auto& change : fields) output.expected_field_paths.push_back(change.raw.display_path);
        if (document_id == "persist.roster.json" && std::any_of(document_mutations.begin(), document_mutations.end(),
                [](const auto& mutation) { return mutation.kind == CampaignDocumentMutationKind::AppendTemplate; }))
            output.expected_field_paths.push_back("base_root/nextGuid");
        candidate.documents.push_back(std::move(output));
        candidate.affected_documents.push_back(document_id);
    }
    return core::Result<SaveCandidate, core::Error>::success(std::move(candidate));
}

core::Result<SaveCommitResult, core::Error>
SafeSaveCommitter::commit(const RawSaveProfile& source_profile, const ChangeSet& requested_changes,
                          const std::filesystem::path& target_profile_root,
                          const std::filesystem::path& backup_directory,
                          SaveCommitMode mode) const {
    const auto normalized = effective_hero_deletion_changes(requested_changes);
    const auto& changes = normalized;
    const auto& source_root = source_profile.descriptor.root_path;
    if (source_root.empty() || target_profile_root.empty() || backup_directory.empty())
        return core::Result<SaveCommitResult, core::Error>::failure(
            {core::ErrorCode::InvalidConfiguration, "Source, target copy, and backup paths are required",
             "SafeSaveCommitter"});
    const bool direct_source = mode == SaveCommitMode::DirectSource;
    if ((!direct_source && same_path_or_parent(source_root, target_profile_root)) ||
        same_path_or_parent(source_root, backup_directory) ||
        same_path_or_parent(target_profile_root, backup_directory))
        return core::Result<SaveCommitResult, core::Error>::failure(
            {core::ErrorCode::InvalidConfiguration,
             "Source profile, output profile copy, and backup directory must be separate directory trees",
             "SafeSaveCommitter", {{"source_profile", source_root.string()},
                                   {"target_profile", target_profile_root.string()},
                                   {"backup_directory", backup_directory.string()}}});

    if (direct_source && source_root.lexically_normal() != target_profile_root.lexically_normal())
        return core::Result<SaveCommitResult, core::Error>::failure(
            {core::ErrorCode::InvalidConfiguration,
             "Direct source commits must target the loaded source profile", "SafeSaveCommitter"});

    for (const auto& change : changes.changes) {
        const auto* mapping = find_mapping(change.target.semantic_property);
        const bool game_verified = mapping &&
            mapping->capability() == CampaignMappingCapability::CommitWritable;
        const bool acceptance_candidate = mapping && mode == SaveCommitMode::AcceptanceTestCandidate &&
            mapping->capability() == CampaignMappingCapability::CandidateWritable;
        if (!game_verified && !acceptance_candidate)
            return core::Result<SaveCommitResult, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "This commit mode does not permit the mapping without game evidence",
                              {{"property", change.target.semantic_property}}));
    }
    for (const auto& batch : changes.document_mutation_batches) {
        const auto capability = std::find_if(campaign_operation_capabilities().begin(),
            campaign_operation_capabilities().end(), [&](const auto& item) {
                return item.operation_id == batch.operation_id &&
                       item.availability == CampaignOperationAvailability::Available;
            });
        if (batch.cancel || capability == campaign_operation_capabilities().end())
            return core::Result<SaveCommitResult, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "Safe commit requires an active game-verified document operation",
                              {{"operation", batch.operation_id}}));
        for (const auto& mutation : batch.mutations) {
            const auto* mapping = find_mapping(mutation.semantic_property);
            if (mapping == nullptr || mapping->capability() != CampaignMappingCapability::CommitWritable)
                return core::Result<SaveCommitResult, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable,
                                  "Safe commit requires every document mutation to use a game-verified mapping",
                                  {{"property", mutation.semantic_property}}));
        }
    }
    for (const auto& change : changes.structural_changes) {
        const auto* mapping = find_mapping(change.target.semantic_property);
        if (mapping == nullptr || mapping->capability() != CampaignMappingCapability::CommitWritable ||
            change.action != CampaignStructuralAction::Erase)
            return core::Result<SaveCommitResult, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "Safe commit requires an erase change with a writable mapping and in-game mutation evidence",
                              {{"property", change.target.semantic_property}}));
    }

    auto source_matches = source_profile.matches_disk_baseline(file_system_);
    if (!source_matches)
        return core::Result<SaveCommitResult, core::Error>::failure(source_matches.error());
    if (!source_matches.value())
        return core::Result<SaveCommitResult, core::Error>::failure(
            {core::ErrorCode::ConcurrentSaveChanged, "The loaded source profile changed after it was opened",
             "SafeSaveCommitter", {{"source_profile", source_root.string()}}});

    auto target_matches = verify_output_copy(source_profile, file_system_, target_profile_root);
    if (!target_matches) return core::Result<SaveCommitResult, core::Error>::failure(target_matches.error());

    SaveAdapter adapter;
    auto candidate = adapter.build_candidate(source_profile, changes);
    if (!candidate) return core::Result<SaveCommitResult, core::Error>::failure(candidate.error());
    if (candidate.value().documents.empty())
        return core::Result<SaveCommitResult, core::Error>::success(
            {target_profile_root, {}, {}, {}});

    auto backup = create_profile_backup(file_system_, target_profile_root, backup_directory);
    if (!backup) return core::Result<SaveCommitResult, core::Error>::failure(backup.error());

    source_matches = source_profile.matches_disk_baseline(file_system_);
    target_matches = verify_output_copy(source_profile, file_system_, target_profile_root);
    if (!source_matches || !target_matches || !source_matches.value()) {
        core::Error error{core::ErrorCode::ConcurrentSaveChanged,
                          "The source or output profile changed while its verified backup was being created",
                          "SafeSaveCommitter", {{"source_profile", source_root.string()},
                                                {"target_profile", target_profile_root.string()},
                                                {"backup_directory", backup_directory.string()}}};
        if (!source_matches) error.cause = std::make_shared<core::Error>(source_matches.error());
        else if (!target_matches) error.cause = std::make_shared<core::Error>(target_matches.error());
        return core::Result<SaveCommitResult, core::Error>::failure(std::move(error));
    }

    std::vector<std::string> committed;
    std::vector<core::dson::BinaryDiffReport> diffs;
    for (const auto& document : candidate.value().documents) {
        const auto source_document = source_profile.documents.find(document.id);
        if (source_document == source_profile.documents.end())
            return core::Result<SaveCommitResult, core::Error>::failure(
                commit_error(core::ErrorCode::CommitFailed, "Candidate document disappeared before write",
                             backup_directory, committed, document.id));
        const auto target_path = target_profile_root / source_document->second.path.filename();
        const auto current = file_system_.read_file(target_path);
        if (!current || current.value() != source_document->second.bytes) {
            const auto concurrent = current
                ? core::Error{core::ErrorCode::ConcurrentSaveChanged,
                              "Target document changed after its baseline check", "SafeSaveCommitter",
                              {{"document", document.id}, {"path", target_path.string()}}}
                : current.error();
            const auto code = committed.empty() ? core::ErrorCode::ConcurrentSaveChanged : core::ErrorCode::PartialCommit;
            return core::Result<SaveCommitResult, core::Error>::failure(
                commit_error(code, "Target document changed before candidate replacement",
                             backup_directory, committed, document.id, &concurrent));
        }

        auto written = file_system_.write_file_atomic(target_path, document.bytes);
        if (!written) {
            const auto code = committed.empty() ? core::ErrorCode::CommitFailed : core::ErrorCode::PartialCommit;
            return core::Result<SaveCommitResult, core::Error>::failure(
                commit_error(code, "Candidate file replacement failed; use the verified backup to recover",
                             backup_directory, committed, document.id, &written.error()));
        }
        committed.push_back(document.id);
        diffs.push_back(document.binary_diff);
    }

    core::dson::DsonReader reader;
    for (const auto& document : candidate.value().documents) {
        const auto source_document = source_profile.documents.find(document.id);
        const auto target_path = target_profile_root / source_document->second.path.filename();
        const auto readback = file_system_.read_file(target_path);
        if (!readback || readback.value() != document.bytes) {
            const core::Error* cause = readback ? nullptr : &readback.error();
            return core::Result<SaveCommitResult, core::Error>::failure(
                commit_error(core::ErrorCode::PartialCommit,
                             "Read-back bytes differ from the candidate; use the verified backup to recover",
                             backup_directory, committed, document.id, cause));
        }
        const auto* data = reinterpret_cast<const std::byte*>(readback.value().data());
        auto decoded = reader.parse(std::span<const std::byte>{data, readback.value().size()}, document.id);
        if (!decoded) {
            return core::Result<SaveCommitResult, core::Error>::failure(
                commit_error(core::ErrorCode::PartialCommit,
                             "Written document failed DSON read-back validation; use the verified backup to recover",
                             backup_directory, committed, document.id, &decoded.error()));
        }
        std::vector<CampaignFieldChange> document_changes;
        for (const auto& change : changes.changes)
            if (change.raw.document_id == document.id) document_changes.push_back(change);
        std::vector<CampaignStructuralChange> document_structural_changes;
        for (const auto& change : changes.structural_changes)
            if (change.raw.document_id == document.id) document_structural_changes.push_back(change);
        std::vector<CampaignDocumentMutation> document_mutations;
        for (const auto& batch : changes.document_mutation_batches)
            for (const auto& mutation : batch.mutations)
                if (mutation.document_id == document.id) document_mutations.push_back(mutation);
        std::string reason;
        if (!validate_decoded_candidate(*source_document->second.decoded, decoded.value(), document_changes,
                                        document_structural_changes, document_mutations, reason)) {
            const auto failure = adapter_error(core::ErrorCode::ValidationFailed, std::move(reason),
                                               {{"document", document.id}});
            return core::Result<SaveCommitResult, core::Error>::failure(
                commit_error(core::ErrorCode::PartialCommit,
                             "Written document failed semantic read-back validation; use the verified backup to recover",
                             backup_directory, committed, document.id, &failure));
        }
    }

    return core::Result<SaveCommitResult, core::Error>::success(
        {target_profile_root, backup_directory, std::move(committed), std::move(diffs)});
}

} // namespace ddse::application
