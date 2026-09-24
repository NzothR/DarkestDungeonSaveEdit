#pragma once

#include "ddse/core/dson/dson_document.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ddse::core::dson {

class DsonDocumentEditor {
public:
    // Adds a deep copy of source_path below parent_path. The cloned root receives child_name.
    [[nodiscard]] static Result<std::size_t, Error>
    append_clone(DsonDocument& target, std::string_view parent_path,
                 const DsonDocument& source, std::string_view source_path,
                 std::string_view child_name);

    // Adds a deep copy at the requested ordered-child position. Positions at
    // children.size() append to the object.
    [[nodiscard]] static Result<std::size_t, Error>
    insert_clone_at(DsonDocument& target, std::string_view parent_path,
                    const DsonDocument& source, std::string_view source_path,
                    std::string_view child_name, std::size_t child_position);

    // Adds a new object with primitive child fields while preserving the
    // destination's ordered-field layout. The writer rebuilds DSON metadata.
    [[nodiscard]] static Result<std::size_t, Error>
    append_object(DsonDocument& target, std::string_view parent_path,
                  std::string_view child_name,
                  const std::vector<std::pair<std::string, Value>>& primitive_fields);

    // Adds a typed primitive field to an object. Used for definition-driven
    // keyed collections such as a new hero's selected skill maps.
    [[nodiscard]] static Result<std::size_t, Error>
    append_value(DsonDocument& target, std::string_view parent_path,
                 std::string_view child_name, Value value);

    // Renames a field and all paths below it, preserving its value and children.
    [[nodiscard]] static Result<void, Error>
    rename(DsonDocument& document, std::string_view path, std::string_view new_name);

    // Removes a field and its descendants. The root object cannot be removed.
    [[nodiscard]] static Result<void, Error>
    erase(DsonDocument& document, std::string_view path);
};

} // namespace ddse::core::dson
