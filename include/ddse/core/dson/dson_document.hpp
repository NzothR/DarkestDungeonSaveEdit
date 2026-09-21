#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <utility>

namespace ddse::core::dson {

enum class ValueKind {
    Object,
    Boolean,
    Character,
    TwoBoolean,
    String,
    Integer,
    Float,
    IntegerVector,
    StringVector,
    FloatArray,
    TwoInteger,
    EmbeddedDson,
    Unknown,
};

enum class TypeEvidence { Structure, KnownPath, ShapeHeuristic, Unknown };

struct DsonDocument;
using Value = std::variant<std::monostate, bool, char, std::array<bool, 2>, std::string,
                           std::int32_t, float, std::vector<std::int32_t>,
                           std::vector<std::string>, std::vector<float>,
                           std::array<std::int32_t, 2>>;

struct Header {
    std::array<std::byte, 4> magic{};
    std::array<std::byte, 4> revision{};
    std::uint32_t header_length{};
    std::uint32_t reserved_1{};
    std::uint32_t meta1_size{};
    std::uint32_t meta1_count{};
    std::uint32_t meta1_offset{};
    std::uint64_t reserved_2{};
    std::uint64_t reserved_3{};
    std::uint32_t meta2_count{};
    std::uint32_t meta2_offset{};
    std::uint32_t reserved_4{};
    std::uint32_t data_length{};
    std::uint32_t data_offset{};
};

struct Meta1Entry {
    std::int32_t parent_index{};
    std::uint32_t meta2_entry_index{};
    std::uint32_t direct_children{};
    std::uint32_t all_descendants{};
};

struct Meta2Entry {
    std::uint32_t name_hash{};
    std::uint32_t data_offset{};
    std::uint32_t field_info{};
};

struct DsonField {
    static constexpr std::size_t no_index = static_cast<std::size_t>(-1);

    std::string name;
    std::string path;
    ValueKind kind{ValueKind::Unknown};
    TypeEvidence type_evidence{TypeEvidence::Unknown};
    Value value;
    std::vector<std::byte> raw_data;
    std::vector<std::size_t> children;
    std::shared_ptr<DsonDocument> embedded_document;
    std::size_t parent_index{no_index};
    std::uint32_t meta2_entry_index{};
    std::uint32_t meta1_entry_index{UINT32_MAX};
    std::uint32_t name_hash{};
    std::uint32_t field_info{};
    std::uint64_t source_name_offset{};
    std::uint64_t source_value_offset{};
    bool dirty{};

    void replace_value(Value replacement) {
        value = std::move(replacement);
        dirty = true;
    }
};

struct DsonDocument {
    Header header;
    std::vector<Meta1Entry> meta1;
    std::vector<Meta2Entry> meta2;
    std::vector<DsonField> fields; // physical Data-section order; duplicates are retained
    std::vector<std::byte> original_bytes;
    std::string source_name;
    std::vector<std::size_t> root_fields;
    // Set by DsonDocumentEditor when fields or object keys are added, renamed, or removed.
    bool structural_dirty{};
};

struct StructureSummary {
    std::size_t fields{};
    std::size_t objects{};
    std::size_t duplicate_names{};
    std::size_t unknown_values{};
    std::size_t embedded_documents{};
    std::size_t max_depth{};
    std::uint64_t fingerprint{};

    [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] std::string_view to_string(ValueKind kind) noexcept;
[[nodiscard]] std::string_view to_string(TypeEvidence evidence) noexcept;
[[nodiscard]] std::uint32_t string_hash(std::string_view utf8) noexcept;
[[nodiscard]] StructureSummary summarize(const DsonDocument& document);

} // namespace ddse::core::dson
