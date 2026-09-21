#pragma once

#include "ddse/core/dson/dson_document.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstddef>
#include <string_view>

namespace ddse::core::dson {

class DsonDocumentEditor {
public:
    // Adds a deep copy of source_path below parent_path. The cloned root receives child_name.
    [[nodiscard]] static Result<std::size_t, Error>
    append_clone(DsonDocument& target, std::string_view parent_path,
                 const DsonDocument& source, std::string_view source_path,
                 std::string_view child_name);

    // Renames a field and all paths below it, preserving its value and children.
    [[nodiscard]] static Result<void, Error>
    rename(DsonDocument& document, std::string_view path, std::string_view new_name);

    // Removes a field and its descendants. The root object cannot be removed.
    [[nodiscard]] static Result<void, Error>
    erase(DsonDocument& document, std::string_view path);
};

} // namespace ddse::core::dson
