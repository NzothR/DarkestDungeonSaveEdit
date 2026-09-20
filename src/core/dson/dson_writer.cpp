#include "ddse/core/dson/dson_writer.hpp"

#include "ddse/core/dson/dson_reader.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <type_traits>

namespace ddse::core::dson {
namespace {

Error encode_error(std::string_view message, const DsonDocument& document,
                   std::string_view path = {}, std::uint64_t offset = 0) {
    Error error{ErrorCode::DsonEncodeFailed, std::string{message}, "DsonWriter",
                {{"document", document.source_name}, {"offset", std::to_string(offset)}}};
    if (!path.empty()) error.context.emplace("path", path);
    return error;
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        output.push_back(static_cast<std::byte>((value >> (i * 8U)) & 0xffU));
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    append_u32(output, static_cast<std::uint32_t>(value));
    append_u32(output, static_cast<std::uint32_t>(value >> 32U));
}

void append_string(std::vector<std::byte>& output, std::string_view value) {
    for (const unsigned char byte : value) output.push_back(static_cast<std::byte>(byte));
}

void align_data(std::vector<std::byte>& output) {
    while ((output.size() & 3U) != 0) output.push_back(std::byte{0});
}

bool has_changes(const DsonDocument& document) {
    for (const auto& field : document.fields) {
        if (field.dirty || (field.embedded_document && has_changes(*field.embedded_document))) return true;
    }
    return false;
}

std::string dirty_field_path(const DsonDocument& document) {
    for (const auto& field : document.fields) {
        if (field.dirty) return field.path;
        if (field.embedded_document) {
            auto nested = dirty_field_path(*field.embedded_document);
            if (!nested.empty()) return nested;
        }
    }
    return {};
}

bool is_aligned_kind(ValueKind kind) {
    switch (kind) {
    case ValueKind::TwoBoolean:
    case ValueKind::String:
    case ValueKind::Integer:
    case ValueKind::Float:
    case ValueKind::IntegerVector:
    case ValueKind::StringVector:
    case ValueKind::FloatArray:
    case ValueKind::TwoInteger:
    case ValueKind::EmbeddedDson:
        return true;
    default:
        return false;
    }
}

std::uint64_t alignment_padding(std::uint64_t offset) {
    return (4U - (offset & 3U)) & 3U;
}

Result<void, Error> write_value(std::vector<std::byte>& output, const DsonField& field,
                                const DsonWriter& writer, const DsonDocument& document);

Result<void, Error> write_known_value(std::vector<std::byte>& output, const DsonField& field,
                                      const DsonDocument& document) {
    auto wrong_value = [&]() {
        return Result<void, Error>::failure(encode_error("Dirty value does not match its parsed DSON type or exceeds a format limit",
                                                         document, field.path, field.source_value_offset));
    };
    switch (field.kind) {
    case ValueKind::Boolean:
        if (const auto* value = std::get_if<bool>(&field.value)) {
            output.push_back(*value ? std::byte{1} : std::byte{0});
            return Result<void, Error>::success();
        }
        break;
    case ValueKind::Character:
        if (const auto* value = std::get_if<char>(&field.value)) {
            output.push_back(static_cast<std::byte>(static_cast<unsigned char>(*value)));
            return Result<void, Error>::success();
        }
        break;
    case ValueKind::TwoBoolean:
        if (const auto* value = std::get_if<std::array<bool, 2>>(&field.value)) {
            align_data(output);
            append_u32(output, (*value)[0] ? 1U : 0U);
            append_u32(output, (*value)[1] ? 1U : 0U);
            return Result<void, Error>::success();
        }
        break;
    case ValueKind::String:
        if (const auto* value = std::get_if<std::string>(&field.value)) {
            if (value->size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) break;
            align_data(output);
            append_u32(output, static_cast<std::uint32_t>(value->size() + 1));
            append_string(output, *value);
            output.push_back(std::byte{0});
            return Result<void, Error>::success();
        }
        break;
    case ValueKind::Integer:
        if (const auto* value = std::get_if<std::int32_t>(&field.value)) {
            align_data(output);
            append_u32(output, std::bit_cast<std::uint32_t>(*value));
            return Result<void, Error>::success();
        }
        break;
    case ValueKind::Float:
        if (const auto* value = std::get_if<float>(&field.value)) {
            align_data(output);
            append_u32(output, std::bit_cast<std::uint32_t>(*value));
            return Result<void, Error>::success();
        }
        break;
    case ValueKind::IntegerVector:
        if (const auto* value = std::get_if<std::vector<std::int32_t>>(&field.value)) {
            if (value->size() > std::numeric_limits<std::uint32_t>::max()) break;
            align_data(output);
            append_u32(output, static_cast<std::uint32_t>(value->size()));
            for (const auto item : *value) append_u32(output, std::bit_cast<std::uint32_t>(item));
            return Result<void, Error>::success();
        }
        break;
    case ValueKind::StringVector:
        if (const auto* value = std::get_if<std::vector<std::string>>(&field.value)) {
            if (value->size() > std::numeric_limits<std::uint32_t>::max()) break;
            if (std::any_of(value->begin(), value->end(), [](const std::string& item) {
                    return item.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
                })) break;
            align_data(output);
            append_u32(output, static_cast<std::uint32_t>(value->size()));
            for (const auto& item : *value) {
                const auto length = item.size() + 1;
                align_data(output);
                append_u32(output, static_cast<std::uint32_t>(length));
                append_string(output, item);
                output.push_back(std::byte{0});
            }
            return Result<void, Error>::success();
        }
        break;
    case ValueKind::FloatArray:
        if (const auto* value = std::get_if<std::vector<float>>(&field.value)) {
            align_data(output);
            for (const auto item : *value) append_u32(output, std::bit_cast<std::uint32_t>(item));
            return Result<void, Error>::success();
        }
        break;
    case ValueKind::TwoInteger:
        if (const auto* value = std::get_if<std::array<std::int32_t, 2>>(&field.value)) {
            align_data(output);
            append_u32(output, std::bit_cast<std::uint32_t>((*value)[0]));
            append_u32(output, std::bit_cast<std::uint32_t>((*value)[1]));
            return Result<void, Error>::success();
        }
        break;
    case ValueKind::Object:
    case ValueKind::EmbeddedDson:
    case ValueKind::Unknown:
        break;
    }
    return wrong_value();
}

Result<void, Error> write_value(std::vector<std::byte>& output, const DsonField& field,
                                const DsonWriter& writer, const DsonDocument& document) {
    if (field.kind == ValueKind::Object)
        return Result<void, Error>::failure(Error{ErrorCode::DsonEncodeFailed,
            "Object fields cannot be value-edited in Stage 2", "DsonWriter", {{"path", field.path}}});
    if (field.kind == ValueKind::Unknown)
        return Result<void, Error>::failure(Error{ErrorCode::DsonEncodeFailed,
            "Unknown values are read-only and remain raw-preserved", "DsonWriter", {{"path", field.path}}});
    if (field.kind == ValueKind::EmbeddedDson) {
        if (!field.embedded_document)
            return Result<void, Error>::failure(Error{ErrorCode::DsonEncodeFailed,
                "Embedded DSON field has no parsed child document", "DsonWriter", {{"path", field.path}}});
        auto embedded = writer.encode(*field.embedded_document);
        if (!embedded) return Result<void, Error>::failure(embedded.error());
        if (embedded.value().size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            return Result<void, Error>::failure(Error{ErrorCode::DsonEncodeFailed,
                "Embedded document exceeds the DSON string length limit", "DsonWriter", {{"path", field.path}}});
        align_data(output);
        append_u32(output, static_cast<std::uint32_t>(embedded.value().size()));
        output.insert(output.end(), embedded.value().begin(), embedded.value().end());
        return Result<void, Error>::success();
    }
    return write_known_value(output, field, document);
}

Result<void, Error> append_preserved_value(std::vector<std::byte>& data, const DsonField& field,
                                           const DsonDocument& document) {
    if (!is_aligned_kind(field.kind)) {
        const auto old_data_offset = field.source_value_offset - document.header.data_offset;
        const auto new_padding = alignment_padding(data.size());
        const auto old_padding = alignment_padding(old_data_offset);
        if (field.kind == ValueKind::Unknown && new_padding != old_padding) {
            return Result<void, Error>::failure(encode_error(
                "Changing a prior field shifts an Unknown value across an alignment boundary; the document is read-only",
                document, field.path, field.source_value_offset));
        }
        data.insert(data.end(), field.raw_data.begin(), field.raw_data.end());
        return Result<void, Error>::success();
    }

    const auto old_data_offset = field.source_value_offset - document.header.data_offset;
    const auto old_padding = static_cast<std::size_t>(alignment_padding(old_data_offset));
    if (field.raw_data.size() < old_padding)
        return Result<void, Error>::failure(encode_error("Raw field data is shorter than its alignment padding",
                                                         document, field.path, field.source_value_offset));
    const auto new_padding = static_cast<std::size_t>(alignment_padding(data.size()));
    if (new_padding == old_padding) {
        data.insert(data.end(), field.raw_data.begin(), field.raw_data.end());
    } else {
        data.insert(data.end(), new_padding, std::byte{0});
        data.insert(data.end(), field.raw_data.begin() + static_cast<std::ptrdiff_t>(old_padding), field.raw_data.end());
    }
    return Result<void, Error>::success();
}

} // namespace

Result<std::vector<std::byte>, Error> DsonWriter::encode(const DsonDocument& document) const {
    if (document.fields.size() != document.meta2.size() || document.fields.empty() ||
        document.meta1.size() != document.header.meta1_count || document.meta2.size() != document.header.meta2_count ||
        document.root_fields.size() != 1) {
        return Result<std::vector<std::byte>, Error>::failure(encode_error("Document AST counts are inconsistent", document));
    }
    if (!has_changes(document)) {
        if (document.original_bytes.empty())
            return Result<std::vector<std::byte>, Error>::failure(encode_error("Unmodified document has no original byte source", document));
        return Result<std::vector<std::byte>, Error>::success(document.original_bytes);
    }

    std::vector<bool> seen(document.meta2.size(), false);
    auto meta2 = document.meta2;
    std::vector<std::byte> data;
    for (std::size_t i = 0; i < document.fields.size(); ++i) {
        const auto& field = document.fields[i];
        if (field.meta2_entry_index >= meta2.size() || seen[field.meta2_entry_index])
            return Result<std::vector<std::byte>, Error>::failure(encode_error("AST field-to-Meta2 mapping is invalid", document, field.path));
        seen[field.meta2_entry_index] = true;
        const auto& source_meta = meta2[field.meta2_entry_index];
        const auto encoded_name_length = field.name.size() + 1;
        const auto declared_name_length = (field.field_info & 0x000007fcU) >> 2U;
        const bool is_object = (field.field_info & 1U) != 0;
        if (field.name_hash != source_meta.name_hash || field.field_info != source_meta.field_info ||
            string_hash(field.name) != field.name_hash || encoded_name_length != declared_name_length ||
            (is_object != (field.kind == ValueKind::Object))) {
            return Result<std::vector<std::byte>, Error>::failure(encode_error("Field identity or structural metadata changed; Stage 2 does not support structural edits",
                document, field.path, field.source_name_offset));
        }
        if (field.dirty && (field.kind == ValueKind::Object || field.kind == ValueKind::Unknown)) {
            return Result<std::vector<std::byte>, Error>::failure(encode_error(
                field.kind == ValueKind::Unknown ? "Unknown values are read-only" : "Object structure is read-only",
                document, field.path, field.source_value_offset));
        }
        meta2[field.meta2_entry_index].data_offset = static_cast<std::uint32_t>(data.size());
        append_string(data, field.name);
        data.push_back(std::byte{0});

        if (field.kind == ValueKind::Object) continue;
        const bool embedded_changed = field.embedded_document && has_changes(*field.embedded_document);
        if (field.dirty || embedded_changed) {
            auto result = write_value(data, field, *this, document);
            if (!result) return Result<std::vector<std::byte>, Error>::failure(result.error());
        } else {
            auto result = append_preserved_value(data, field, document);
            if (!result) return Result<std::vector<std::byte>, Error>::failure(result.error());
        }
    }
    if (std::find(seen.begin(), seen.end(), false) != seen.end())
        return Result<std::vector<std::byte>, Error>::failure(encode_error("One or more Meta2 fields have no AST field", document));
    if (data.size() > std::numeric_limits<std::uint32_t>::max())
        return Result<std::vector<std::byte>, Error>::failure(encode_error("Encoded Data section exceeds DSON size limit", document));

    std::vector<std::byte> output;
    output.reserve(static_cast<std::size_t>(document.header.data_offset) + data.size());
    const auto& h = document.header;
    output.insert(output.end(), h.magic.begin(), h.magic.end());
    output.insert(output.end(), h.revision.begin(), h.revision.end());
    append_u32(output, h.header_length);
    append_u32(output, h.reserved_1);
    append_u32(output, h.meta1_size);
    append_u32(output, h.meta1_count);
    append_u32(output, h.meta1_offset);
    append_u64(output, h.reserved_2);
    append_u64(output, h.reserved_3);
    append_u32(output, h.meta2_count);
    append_u32(output, h.meta2_offset);
    append_u32(output, h.reserved_4);
    append_u32(output, static_cast<std::uint32_t>(data.size()));
    append_u32(output, h.data_offset);
    for (const auto& entry : document.meta1) {
        append_u32(output, std::bit_cast<std::uint32_t>(entry.parent_index));
        append_u32(output, entry.meta2_entry_index);
        append_u32(output, entry.direct_children);
        append_u32(output, entry.all_descendants);
    }
    for (const auto& entry : meta2) {
        append_u32(output, entry.name_hash);
        append_u32(output, entry.data_offset);
        append_u32(output, entry.field_info);
    }
    output.insert(output.end(), data.begin(), data.end());

    DsonReader reader;
    auto validation = reader.parse(output, document.source_name);
    if (!validation) {
        auto error = encode_error("Encoded candidate failed DSON structural validation", document);
        error.cause = std::make_shared<Error>(validation.error());
        return Result<std::vector<std::byte>, Error>::failure(std::move(error));
    }
    return Result<std::vector<std::byte>, Error>::success(std::move(output));
}

BinaryDiffReport DsonWriter::compare_binary(const DsonDocument& original,
                                             const std::vector<std::byte>& encoded) const {
    BinaryDiffReport report;
    report.original_size = original.original_bytes.size();
    report.encoded_size = encoded.size();
    report.binary_identical = original.original_bytes == encoded;
    const auto extent = std::max(report.original_size, report.encoded_size);
    bool found = false;
    std::size_t first = 0;
    for (std::size_t i = 0; i < extent; ++i) {
        const bool differs = i >= report.original_size || i >= report.encoded_size ||
                             original.original_bytes[i] != encoded[i];
        if (differs) {
            ++report.differing_byte_count;
            if (!found) { first = i; found = true; }
        }
    }
    if (!found) return report;
    report.first_difference_offset = first;
    if (first >= original.header.data_offset) {
        for (std::size_t i = 0; i < original.fields.size(); ++i) {
            const auto& field = original.fields[i];
            const auto end = i + 1 < original.fields.size()
                                 ? original.fields[i + 1].source_name_offset
                                 : static_cast<std::uint64_t>(original.original_bytes.size());
            if (field.source_name_offset <= first && first < end) {
                report.field_path = field.path;
                break;
            }
        }
    } else if (first >= original.header.meta2_offset) {
        const auto index = (first - original.header.meta2_offset) / 12;
        for (const auto& field : original.fields)
            if (field.meta2_entry_index == index) { report.field_path = field.path; break; }
    }
    if (report.field_path.empty()) report.field_path = dirty_field_path(original);
    return report;
}

std::string BinaryDiffReport::to_string() const {
    if (binary_identical) return "binary-identical";
    std::string result = "bytes=" + std::to_string(original_size) + "->" + std::to_string(encoded_size) +
                         " differing=" + std::to_string(differing_byte_count) +
                         " first_offset=" + std::to_string(first_difference_offset);
    if (!field_path.empty()) result += " field=" + field_path;
    return result;
}

} // namespace ddse::core::dson
