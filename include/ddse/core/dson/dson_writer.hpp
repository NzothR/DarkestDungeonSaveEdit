#pragma once

#include "ddse/core/dson/dson_document.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ddse::core::dson {

struct BinaryDiffReport {
    bool binary_identical{};
    std::size_t original_size{};
    std::size_t encoded_size{};
    std::size_t differing_byte_count{};
    std::size_t first_difference_offset{};
    std::string field_path;

    [[nodiscard]] std::string to_string() const;
};

class DsonWriter {
public:
    [[nodiscard]] Result<std::vector<std::byte>, Error> encode(const DsonDocument& document) const;
    [[nodiscard]] BinaryDiffReport compare_binary(const DsonDocument& original,
                                                   const std::vector<std::byte>& encoded) const;
};

} // namespace ddse::core::dson
