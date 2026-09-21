#include "ddse/application/save_commit.hpp"

#include "ddse/application/campaign_mappings.hpp"
#include "ddse/core/dson/dson_document_editor.hpp"
#include "ddse/core/dson/dson_reader.hpp"
#include "ddse/core/dson/dson_writer.hpp"

#include <algorithm>
#include <cctype>
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
           (kind == ValueKind::Boolean && std::holds_alternative<bool>(value));
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
        if (!target && before.kind != ValueKind::EmbeddedDson &&
            (before.value != after.value || before.raw_data != after.raw_data)) {
            reason = "Candidate changed a DSON field outside the ChangeSet";
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

bool validate_decoded_candidate(const DsonDocument& original, const DsonDocument& candidate,
                                const std::vector<CampaignFieldChange>& changes,
                                const std::vector<CampaignStructuralChange>& structural_changes,
                                std::string& reason) {
    if (structural_changes.empty()) return validate_decoded_candidate(original, candidate, changes, reason);
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

    for (const auto& change : structural_changes) {
        const auto target = before.find(change.raw.display_path);
        if (target == before.end() || target->second.size() != 1U || after.contains(change.raw.display_path)) {
            reason = "Structural candidate did not remove exactly the requested mapped entry";
            return false;
        }
    }
    for (const auto& [path, fields] : before) {
        if (path_is_removed(path, structural_changes)) continue;
        const auto candidate_field = after.find(path);
        if (candidate_field == after.end() || candidate_field->second.size() != fields.size()) {
            reason = "Candidate removed a DSON field outside the structural ChangeSet";
            return false;
        }
        const bool scalar_target = std::any_of(changes.begin(), changes.end(), [&](const auto& change) {
            return change.raw.display_path == path;
        });
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto& field = fields[index];
            const auto& current = candidate_field->second[index];
            if (field.name != current.name || field.kind != current.kind) {
                reason = "Candidate changed a DSON field identity outside its mapped ChangeSet: " + path;
                return false;
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
            reason = "Candidate added a DSON field outside the structural ChangeSet";
            return false;
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

} // namespace

core::Result<SaveCandidate, core::Error>
SaveAdapter::build_candidate(const RawSaveProfile& profile, const ChangeSet& changes) const {
    if (changes.empty())
        return core::Result<SaveCandidate, core::Error>::failure(
            adapter_error(core::ErrorCode::ValidationFailed, "Cannot build a save candidate from an empty ChangeSet"));

    std::map<std::string, std::vector<CampaignFieldChange>, std::less<>> grouped_fields;
    std::map<std::string, std::vector<CampaignStructuralChange>, std::less<>> grouped_structural;
    std::set<std::string, std::less<>> expected_documents;
    std::set<std::string, std::less<>> unique_targets;
    for (const auto& change : changes.changes) {
        const auto* mapping = find_mapping(change.target.semantic_property);
        if (mapping == nullptr || mapping->capability() < CampaignMappingCapability::CandidateWritable)
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "ChangeSet contains a property without an implemented writable mapping",
                              {{"property", change.target.semantic_property}}));
        if (mapping->document_id != change.raw.document_id || change.raw.steps.empty() ||
            change.raw.display_path != expand_path(mapping->raw_path_template, change.target))
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "ChangeSet locator does not match the registered mapping",
                              {{"property", change.target.semantic_property},
                               {"document", change.raw.document_id}, {"path", change.raw.display_path}}));
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
            change.expected_kind != mapping->expected_type || change.expected_kind != ValueKind::Object ||
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

    const std::set<std::string, std::less<>> declared_documents(changes.affected_documents.begin(),
                                                                changes.affected_documents.end());
    if (declared_documents != expected_documents || declared_documents.size() != changes.affected_documents.size())
        return core::Result<SaveCandidate, core::Error>::failure(
            adapter_error(core::ErrorCode::ValidationFailed,
                          "ChangeSet affected_documents does not exactly match its value and structural changes"));

    SaveCandidate candidate;
    candidate.affected_documents.assign(expected_documents.begin(), expected_documents.end());
    core::dson::DsonWriter writer;
    core::dson::DsonReader reader;
    for (const auto& document_id : expected_documents) {
        const auto& document_changes = grouped_fields[document_id];
        const auto& structural_changes = grouped_structural[document_id];
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
        for (const auto& change : document_changes) {
            std::string reason;
            auto field = locate_field(cloned, change.raw, reason);
            if (!field)
                return core::Result<SaveCandidate, core::Error>::failure(
                    adapter_error(core::ErrorCode::MappingNotWritable, std::move(reason),
                                  {{"document", document_id}, {"path", change.raw.display_path}}));
            auto& target = field->get();
            const auto* mapping = find_mapping(change.target.semantic_property);
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

        auto encoded = writer.encode(cloned);
        if (!encoded)
            return core::Result<SaveCandidate, core::Error>::failure(encoded.error());
        const auto bytes = bytes_to_string(encoded.value());
        const auto* data = reinterpret_cast<const std::byte*>(bytes.data());
        auto decoded = reader.parse(std::span<const std::byte>{data, bytes.size()}, document_id);
        if (!decoded)
            return core::Result<SaveCandidate, core::Error>::failure(decoded.error());
        std::string reason;
        if (!validate_decoded_candidate(original, decoded.value(), document_changes, structural_changes, reason))
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::ValidationFailed, std::move(reason), {{"document", document_id}}));

        auto diff = writer.compare_binary(original, encoded.value());
        if (diff.binary_identical)
            return core::Result<SaveCandidate, core::Error>::failure(
                adapter_error(core::ErrorCode::ValidationFailed,
                              "Candidate bytes are unchanged despite a non-empty ChangeSet",
                              {{"document", document_id}}));
        CandidateDocument output;
        output.id = document_id;
        output.bytes = bytes;
        output.binary_diff = std::move(diff);
        for (const auto& change : document_changes) output.expected_field_paths.push_back(change.raw.display_path);
        for (const auto& change : structural_changes) output.expected_field_paths.push_back(change.raw.display_path);
        candidate.documents.push_back(std::move(output));
    }
    return core::Result<SaveCandidate, core::Error>::success(std::move(candidate));
}

core::Result<SaveCommitResult, core::Error>
SafeSaveCommitter::commit(const RawSaveProfile& source_profile, const ChangeSet& changes,
                          const std::filesystem::path& target_profile_root,
                          const std::filesystem::path& backup_directory) const {
    const auto& source_root = source_profile.descriptor.root_path;
    if (source_root.empty() || target_profile_root.empty() || backup_directory.empty())
        return core::Result<SaveCommitResult, core::Error>::failure(
            {core::ErrorCode::InvalidConfiguration, "Source, target copy, and backup paths are required",
             "SafeSaveCommitter"});
    if (same_path_or_parent(source_root, target_profile_root) ||
        same_path_or_parent(source_root, backup_directory) ||
        same_path_or_parent(target_profile_root, backup_directory))
        return core::Result<SaveCommitResult, core::Error>::failure(
            {core::ErrorCode::InvalidConfiguration,
             "Source profile, output profile copy, and backup directory must be separate directory trees",
             "SafeSaveCommitter", {{"source_profile", source_root.string()},
                                   {"target_profile", target_profile_root.string()},
                                   {"backup_directory", backup_directory.string()}}});

    for (const auto& change : changes.changes) {
        const auto* mapping = find_mapping(change.target.semantic_property);
        if (mapping == nullptr || mapping->capability() != CampaignMappingCapability::CommitWritable)
            return core::Result<SaveCommitResult, core::Error>::failure(
                adapter_error(core::ErrorCode::MappingNotWritable,
                              "Safe commit requires a writable mapping with in-game mutation evidence",
                              {{"property", change.target.semantic_property}}));
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
        std::string reason;
        if (!validate_decoded_candidate(*source_document->second.decoded, decoded.value(), document_changes,
                                        document_structural_changes, reason)) {
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
