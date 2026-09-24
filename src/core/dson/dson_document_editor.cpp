#include "ddse/core/dson/dson_document_editor.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace ddse::core::dson {
namespace {

Error edit_error(std::string message, std::string path = {}) {
    Error error{ErrorCode::DsonEncodeFailed, std::move(message), "DsonDocumentEditor"};
    if (!path.empty()) error.context.emplace("path", std::move(path));
    return error;
}

std::optional<std::size_t> find_field(const DsonDocument& document, std::string_view path) {
    for (std::size_t index = 0; index < document.fields.size(); ++index)
        if (document.fields[index].path == path) return index;
    return std::nullopt;
}

bool is_descendant_of(const DsonDocument& document, std::size_t candidate, std::size_t ancestor) {
    if (candidate >= document.fields.size() || ancestor >= document.fields.size()) return false;
    auto parent = document.fields[candidate].parent_index;
    while (parent != DsonField::no_index) {
        if (parent == ancestor) return true;
        if (parent >= document.fields.size()) return false;
        parent = document.fields[parent].parent_index;
    }
    return false;
}

std::size_t subtree_end(const DsonDocument& document, std::size_t root) {
    auto end = root + 1;
    while (end < document.fields.size() && is_descendant_of(document, end, root)) ++end;
    return end;
}

DsonDocument deep_clone(const DsonDocument& source) {
    auto copy = source;
    for (std::size_t index = 0; index < source.fields.size(); ++index) {
        if (source.fields[index].embedded_document)
            copy.fields[index].embedded_document = std::make_shared<DsonDocument>(
                deep_clone(*source.fields[index].embedded_document));
    }
    return copy;
}

ValueKind value_kind(const Value& value) {
    return std::visit([](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>) return ValueKind::Boolean;
        else if constexpr (std::is_same_v<T, char>) return ValueKind::Character;
        else if constexpr (std::is_same_v<T, std::array<bool, 2>>) return ValueKind::TwoBoolean;
        else if constexpr (std::is_same_v<T, std::string>) return ValueKind::String;
        else if constexpr (std::is_same_v<T, std::int32_t>) return ValueKind::Integer;
        else if constexpr (std::is_same_v<T, float>) return ValueKind::Float;
        else if constexpr (std::is_same_v<T, std::vector<std::int32_t>>) return ValueKind::IntegerVector;
        else if constexpr (std::is_same_v<T, std::vector<std::string>>) return ValueKind::StringVector;
        else if constexpr (std::is_same_v<T, std::vector<float>>) return ValueKind::FloatArray;
        else if constexpr (std::is_same_v<T, std::array<std::int32_t, 2>>) return ValueKind::TwoInteger;
        else return ValueKind::Unknown;
    }, value);
}

} // namespace

Result<std::size_t, Error> DsonDocumentEditor::append_clone(
    DsonDocument& target, std::string_view parent_path,
    const DsonDocument& source, std::string_view source_path,
    std::string_view child_name) {
    const auto parent_index = find_field(target, parent_path);
    if (!parent_index || target.fields[*parent_index].kind != ValueKind::Object)
        return Result<std::size_t, Error>::failure(edit_error("Destination path is not an object", std::string{parent_path}));
    return insert_clone_at(target, parent_path, source, source_path, child_name,
                           target.fields[*parent_index].children.size());
}

Result<std::size_t, Error> DsonDocumentEditor::insert_clone_at(
    DsonDocument& target, std::string_view parent_path,
    const DsonDocument& source, std::string_view source_path,
    std::string_view child_name, std::size_t child_position) {
    if (child_name.empty() || child_name.find('/') != std::string_view::npos)
        return Result<std::size_t, Error>::failure(edit_error("New field name is empty or contains a path separator"));

    const auto parent_index = find_field(target, parent_path);
    const auto source_index = find_field(source, source_path);
    if (!parent_index || target.fields[*parent_index].kind != ValueKind::Object)
        return Result<std::size_t, Error>::failure(edit_error("Destination path is not an object", std::string{parent_path}));
    if (!source_index)
        return Result<std::size_t, Error>::failure(edit_error("Source field does not exist", std::string{source_path}));
    const auto& parent = target.fields[*parent_index];
    if (std::any_of(parent.children.begin(), parent.children.end(), [&](std::size_t child) {
            return child < target.fields.size() && target.fields[child].name == child_name;
        }))
        return Result<std::size_t, Error>::failure(edit_error("Destination object already has a child with that name",
                                                              std::string{parent_path} + "/" + std::string{child_name}));
    if (child_position > parent.children.size())
        return Result<std::size_t, Error>::failure(edit_error("Ordered child insertion position is outside the destination object",
                                                              std::string{parent_path}));

    std::vector<std::size_t> source_order;
    std::function<bool(std::size_t)> gather = [&](std::size_t index) {
        if (index >= source.fields.size()) return false;
        source_order.push_back(index);
        for (const auto child : source.fields[index].children)
            if (!gather(child)) return false;
        return true;
    };
    if (!gather(*source_index))
        return Result<std::size_t, Error>::failure(edit_error("Source object has an invalid child index", std::string{source_path}));
    std::sort(source_order.begin(), source_order.end());
    if (std::adjacent_find(source_order.begin(), source_order.end()) != source_order.end())
        return Result<std::size_t, Error>::failure(edit_error("Source subtree contains a repeated field", std::string{source_path}));

    const auto source_root_path = source.fields[*source_index].path;
    const auto new_root_path = target.fields[*parent_index].path + "/" + std::string{child_name};
    std::map<std::size_t, std::size_t> new_index;
    const auto insertion_index = child_position == parent.children.size()
        ? subtree_end(target, *parent_index) : parent.children[child_position];
    for (std::size_t offset = 0; offset < source_order.size(); ++offset)
        new_index.emplace(source_order[offset], insertion_index + offset);

    std::vector<DsonField> cloned;
    cloned.reserve(source_order.size());
    for (const auto old_index : source_order) {
        auto field = source.fields[old_index];
        // A built-in template can come from a different DSON file than the
        // destination. Raw value bytes are preserved, but their absolute source
        // offsets are used to recover alignment padding during serialization.
        // Rebase those offsets to the destination's Data section so preserved
        // unknown fields remain byte-identical without pointing outside it.
        const auto rebase_offset = [&](std::uint64_t offset) {
            if (offset < source.header.data_offset) return offset;
            return static_cast<std::uint64_t>(target.header.data_offset) +
                   (offset - source.header.data_offset);
        };
        field.source_name_offset = rebase_offset(field.source_name_offset);
        field.source_value_offset = rebase_offset(field.source_value_offset);
        field.embedded_document = field.embedded_document
            ? std::make_shared<DsonDocument>(deep_clone(*field.embedded_document))
            : nullptr;
        field.children.clear();
        if (old_index == *source_index) {
            field.name = std::string{child_name};
            field.parent_index = *parent_index;
            field.path = new_root_path;
        } else {
            const auto suffix_offset = source_root_path.size();
            if (!field.path.starts_with(source_root_path) || field.path.size() <= suffix_offset ||
                field.path[suffix_offset] != '/')
                return Result<std::size_t, Error>::failure(edit_error(
                    "Source subtree paths are not rooted at the requested field", field.path));
            field.path = new_root_path + field.path.substr(suffix_offset);
            if (field.parent_index == DsonField::no_index || !new_index.contains(field.parent_index))
                return Result<std::size_t, Error>::failure(edit_error("Source subtree has an external child parent", field.path));
            field.parent_index = new_index.at(field.parent_index);
        }
        const auto old_children = source.fields[old_index].children;
        for (const auto child : old_children) {
            if (!new_index.contains(child))
                return Result<std::size_t, Error>::failure(edit_error("Source subtree has a dangling child index", field.path));
            field.children.push_back(new_index.at(child));
        }
        cloned.push_back(std::move(field));
    }

    const auto count = cloned.size();
    for (auto& field : target.fields) {
        if (field.parent_index != DsonField::no_index && field.parent_index >= insertion_index)
            field.parent_index += count;
        for (auto& child : field.children)
            if (child >= insertion_index) child += count;
    }
    for (auto& root : target.root_fields)
        if (root >= insertion_index) root += count;
    auto inserted_parent_index = *parent_index;
    if (inserted_parent_index >= insertion_index) inserted_parent_index += count;
    cloned.front().parent_index = inserted_parent_index;
    target.fields.insert(target.fields.begin() + static_cast<std::ptrdiff_t>(insertion_index),
                         std::make_move_iterator(cloned.begin()), std::make_move_iterator(cloned.end()));
    auto& inserted_children = target.fields[inserted_parent_index].children;
    inserted_children.insert(inserted_children.begin() + static_cast<std::ptrdiff_t>(child_position), insertion_index);
    target.structural_dirty = true;
    return Result<std::size_t, Error>::success(insertion_index);
}

Result<std::size_t, Error> DsonDocumentEditor::append_object(
    DsonDocument& target, std::string_view parent_path, std::string_view child_name,
    const std::vector<std::pair<std::string, Value>>& primitive_fields) {
    if (child_name.empty() || child_name.find('/') != std::string_view::npos)
        return Result<std::size_t, Error>::failure(edit_error("New object name is empty or contains a path separator"));
    const auto parent_index = find_field(target, parent_path);
    if (!parent_index || target.fields[*parent_index].kind != ValueKind::Object)
        return Result<std::size_t, Error>::failure(edit_error("Destination path is not an object", std::string{parent_path}));
    const auto& parent = target.fields[*parent_index];
    if (std::any_of(parent.children.begin(), parent.children.end(), [&](std::size_t child) {
            return child < target.fields.size() && target.fields[child].name == child_name;
        }))
        return Result<std::size_t, Error>::failure(edit_error("Destination object already has a child with that name",
            std::string{parent_path} + "/" + std::string{child_name}));

    std::set<std::string, std::less<>> names;
    for (const auto& [name, value] : primitive_fields) {
        if (name.empty() || name.find('/') != std::string::npos || !names.insert(name).second ||
            value_kind(value) == ValueKind::Unknown)
            return Result<std::size_t, Error>::failure(edit_error("New object contains an invalid primitive field", name));
    }

    const auto insertion_index = subtree_end(target, *parent_index);
    const auto new_root_path = target.fields[*parent_index].path + "/" + std::string{child_name};
    std::vector<DsonField> appended;
    appended.reserve(primitive_fields.size() + 1);
    DsonField root;
    root.name = std::string{child_name};
    root.path = new_root_path;
    root.kind = ValueKind::Object;
    root.type_evidence = TypeEvidence::Structure;
    root.parent_index = *parent_index;
    for (std::size_t index = 0; index < primitive_fields.size(); ++index)
        root.children.push_back(insertion_index + 1 + index);
    appended.push_back(std::move(root));
    for (const auto& [name, value] : primitive_fields) {
        DsonField field;
        field.name = name;
        field.path = new_root_path + "/" + name;
        field.kind = value_kind(value);
        field.type_evidence = TypeEvidence::KnownPath;
        field.value = value;
        // These fields have no source offsets or raw bytes in the original
        // document. Force the writer to serialize their typed values directly.
        field.dirty = true;
        field.parent_index = insertion_index;
        appended.push_back(std::move(field));
    }

    const auto count = appended.size();
    for (auto& field : target.fields) {
        if (field.parent_index != DsonField::no_index && field.parent_index >= insertion_index)
            field.parent_index += count;
        for (auto& child : field.children)
            if (child >= insertion_index) child += count;
    }
    for (auto& root_index : target.root_fields)
        if (root_index >= insertion_index) root_index += count;
    auto adjusted_parent_index = *parent_index;
    if (adjusted_parent_index >= insertion_index) adjusted_parent_index += count;
    appended.front().parent_index = adjusted_parent_index;
    target.fields.insert(target.fields.begin() + static_cast<std::ptrdiff_t>(insertion_index),
                         std::make_move_iterator(appended.begin()), std::make_move_iterator(appended.end()));
    auto& children = target.fields[adjusted_parent_index].children;
    children.push_back(insertion_index);
    target.structural_dirty = true;
    return Result<std::size_t, Error>::success(insertion_index);
}

Result<std::size_t, Error> DsonDocumentEditor::append_value(
    DsonDocument& target, std::string_view parent_path, std::string_view child_name, Value value) {
    if (child_name.empty() || child_name.find('/') != std::string_view::npos)
        return Result<std::size_t, Error>::failure(edit_error("New field name is empty or contains a path separator"));
    const auto parent_index = find_field(target, parent_path);
    if (!parent_index || target.fields[*parent_index].kind != ValueKind::Object)
        return Result<std::size_t, Error>::failure(edit_error("Destination path is not an object", std::string{parent_path}));
    const auto& parent = target.fields[*parent_index];
    if (std::any_of(parent.children.begin(), parent.children.end(), [&](std::size_t child) {
            return child < target.fields.size() && target.fields[child].name == child_name;
        }))
        return Result<std::size_t, Error>::failure(edit_error("Destination object already has a child with that name",
            std::string{parent_path} + "/" + std::string{child_name}));
    const auto kind = value_kind(value);
    if (kind == ValueKind::Unknown || kind == ValueKind::Object || kind == ValueKind::EmbeddedDson)
        return Result<std::size_t, Error>::failure(edit_error("New field value is not a supported primitive", std::string{child_name}));

    const auto insertion_index = subtree_end(target, *parent_index);
    const auto path = target.fields[*parent_index].path + "/" + std::string{child_name};
    DsonField field;
    field.name = std::string{child_name};
    field.path = path;
    field.kind = kind;
    field.value = std::move(value);
    field.parent_index = *parent_index;
    field.dirty = true;

    for (auto& existing : target.fields) {
        if (existing.parent_index != DsonField::no_index && existing.parent_index >= insertion_index)
            ++existing.parent_index;
        for (auto& child : existing.children) if (child >= insertion_index) ++child;
    }
    for (auto& root : target.root_fields) if (root >= insertion_index) ++root;
    auto inserted_parent_index = *parent_index;
    if (inserted_parent_index >= insertion_index) ++inserted_parent_index;
    field.parent_index = inserted_parent_index;
    target.fields.insert(target.fields.begin() + static_cast<std::ptrdiff_t>(insertion_index), std::move(field));
    target.fields[inserted_parent_index].children.push_back(insertion_index);
    target.structural_dirty = true;
    return Result<std::size_t, Error>::success(insertion_index);
}

Result<void, Error> DsonDocumentEditor::rename(DsonDocument& document, std::string_view path,
                                                std::string_view new_name) {
    if (new_name.empty() || new_name.find('/') != std::string_view::npos)
        return Result<void, Error>::failure(edit_error("New field name is empty or contains a path separator"));
    const auto index = find_field(document, path);
    if (!index) return Result<void, Error>::failure(edit_error("Field to rename does not exist", std::string{path}));
    const auto parent_index = document.fields[*index].parent_index;
    if (parent_index == DsonField::no_index)
        return Result<void, Error>::failure(edit_error("The DSON root object cannot be renamed", std::string{path}));
    if (std::any_of(document.fields[parent_index].children.begin(), document.fields[parent_index].children.end(),
                    [&](std::size_t child) {
                        return child != *index && document.fields[child].name == new_name;
                    }))
        return Result<void, Error>::failure(edit_error("Sibling with the requested name already exists",
                                                       std::string{path}));

    const auto old_prefix = document.fields[*index].path;
    const auto new_prefix = document.fields[parent_index].path + "/" + std::string{new_name};
    const auto end = subtree_end(document, *index);
    document.fields[*index].name = std::string{new_name};
    for (std::size_t field = *index; field < end; ++field) {
        if (field == *index) document.fields[field].path = new_prefix;
        else if (document.fields[field].path.starts_with(old_prefix + "/"))
            document.fields[field].path = new_prefix + document.fields[field].path.substr(old_prefix.size());
        else return Result<void, Error>::failure(edit_error("Descendant field path is outside the renamed subtree",
                                                            document.fields[field].path));
    }
    document.structural_dirty = true;
    return Result<void, Error>::success();
}

Result<void, Error> DsonDocumentEditor::erase(DsonDocument& document, std::string_view path) {
    const auto index = find_field(document, path);
    if (!index) return Result<void, Error>::failure(edit_error("Field to erase does not exist", std::string{path}));
    const auto parent_index = document.fields[*index].parent_index;
    if (parent_index == DsonField::no_index)
        return Result<void, Error>::failure(edit_error("The DSON root object cannot be erased", std::string{path}));
    const auto end = subtree_end(document, *index);
    const auto count = end - *index;

    auto& siblings = document.fields[parent_index].children;
    siblings.erase(std::remove_if(siblings.begin(), siblings.end(), [&](std::size_t child) {
        return child >= *index && child < end;
    }), siblings.end());
    for (std::size_t field_index = 0; field_index < document.fields.size(); ++field_index) {
        if (field_index >= *index && field_index < end) continue;
        auto& field = document.fields[field_index];
        if (field.parent_index != DsonField::no_index && field.parent_index >= end) field.parent_index -= count;
        else if (field.parent_index != DsonField::no_index && field.parent_index >= *index)
            field.parent_index = DsonField::no_index;
        for (auto& child : field.children) {
            if (child >= end) child -= count;
            else if (child >= *index) child = DsonField::no_index;
        }
        field.children.erase(std::remove(field.children.begin(), field.children.end(), DsonField::no_index),
                             field.children.end());
    }
    document.root_fields.erase(std::remove_if(document.root_fields.begin(), document.root_fields.end(), [&](std::size_t root) {
        return root >= *index && root < end;
    }), document.root_fields.end());
    for (auto& root : document.root_fields)
        if (root >= end) root -= count;
    document.fields.erase(document.fields.begin() + static_cast<std::ptrdiff_t>(*index),
                          document.fields.begin() + static_cast<std::ptrdiff_t>(end));
    document.structural_dirty = true;
    return Result<void, Error>::success();
}

} // namespace ddse::core::dson
