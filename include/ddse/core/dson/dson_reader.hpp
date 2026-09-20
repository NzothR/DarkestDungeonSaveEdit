#pragma once

#include "ddse/core/dson/dson_document.hpp"
#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace ddse::core::dson {

struct ReaderOptions {
    std::size_t max_embedded_depth{32};
};

class DsonReader {
public:
    explicit DsonReader(ReaderOptions options = {}) : options_(options) {}

    [[nodiscard]] Result<DsonDocument, Error> parse(std::span<const std::byte> bytes,
                                                     std::string_view source_name = "<memory>") const;

private:
    [[nodiscard]] Result<DsonDocument, Error> parse_at_depth(std::span<const std::byte> bytes,
                                                               std::string_view source_name,
                                                               std::size_t depth) const;
    ReaderOptions options_;
};

} // namespace ddse::core::dson
