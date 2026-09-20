#include "ddse/core/dson/dson_reader.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace ddse::core::dson {
namespace {

constexpr std::array<std::byte, 4> magic{
    std::byte{0x01}, std::byte{0xb1}, std::byte{0x00}, std::byte{0x00}};
constexpr std::uint32_t object_bit = 1U;
constexpr std::uint32_t name_length_mask = 0x000007fcU;
constexpr std::uint32_t object_index_mask = 0x7ffff800U;

Error malformed(std::string_view message, std::string_view document,
                std::string_view section, std::uint64_t offset) {
    return {ErrorCode::DsonMalformed, std::string{message}, "DsonReader",
            {{"document", std::string{document}}, {"section", std::string{section}},
             {"offset", std::to_string(offset)}}};
}

bool fits(std::size_t size, std::uint64_t offset, std::uint64_t length) {
    return offset <= size && length <= static_cast<std::uint64_t>(size) - offset;
}

std::uint32_t u32(std::span<const std::byte> bytes, std::size_t offset) {
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::uint64_t u64(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint64_t>(u32(bytes, offset)) |
           (static_cast<std::uint64_t>(u32(bytes, offset + 4)) << 32U);
}

bool valid_utf8(std::span<const std::byte> bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto c = std::to_integer<unsigned char>(bytes[i]);
        if (c <= 0x7f) { ++i; continue; }
        std::uint32_t codepoint{};
        std::size_t length{};
        if (c >= 0xc2 && c <= 0xdf) { codepoint = c & 0x1fU; length = 2; }
        else if (c >= 0xe0 && c <= 0xef) { codepoint = c & 0x0fU; length = 3; }
        else if (c >= 0xf0 && c <= 0xf4) { codepoint = c & 0x07U; length = 4; }
        else return false;
        if (length > bytes.size() - i) return false;
        for (std::size_t j = 1; j < length; ++j) {
            const auto next = std::to_integer<unsigned char>(bytes[i + j]);
            if ((next & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if ((length == 3 && codepoint < 0x800U) ||
            (length == 4 && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) || codepoint > 0x10ffffU)
            return false;
        i += length;
    }
    return true;
}

std::optional<std::vector<std::string>> path_parts(std::string_view path) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        result.emplace_back(path.substr(start, end == std::string_view::npos ? path.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

bool matches(std::string_view path, std::initializer_list<std::string_view> reverse_pattern) {
    const auto parts = path_parts(path);
    if (parts->size() < reverse_pattern.size()) return false;
    std::size_t index = parts->size();
    for (const auto expected : reverse_pattern) {
        const auto& actual = (*parts)[--index];
        if (expected != "*" && actual != expected) return false;
    }
    return true;
}

bool special_type(ValueKind kind, std::string_view path) {
    switch (kind) {
    case ValueKind::Character:
        return matches(path, {"requirement_code"});
    case ValueKind::Float:
        return matches(path, {"current_hp"}) || matches(path, {"m_Stress"}) ||
               matches(path, {"amount", "buff_group", "*", "actor"}) ||
               matches(path, {"percent", "*", "chapters"}) ||
               matches(path, {"chance", "non_rolled_additional_chances"});
    case ValueKind::IntegerVector:
        return matches(path, {"read_page_indexes"}) || matches(path, {"raid_read_page_indexes"}) ||
               matches(path, {"raid_unread_page_indexes"}) || matches(path, {"dungeons_unlocked"}) ||
               matches(path, {"played_video_list"}) || matches(path, {"trinket_retention_ids"}) ||
               matches(path, {"last_party_guids"}) || matches(path, {"dungeon_history"}) ||
               matches(path, {"buff_group_guids"}) || matches(path, {"result_event_history"}) ||
               matches(path, {"dead_hero_entries"}) ||
               matches(path, {"additional_mash_disabled_infestation_monster_class_ids"}) ||
               matches(path, {"valid_additional_mash_entry_indexes", "mash"}) ||
               matches(path, {"heroes", "party"}) || matches(path, {"skill_cooldown_keys"}) ||
               matches(path, {"skill_cooldown_values"}) || matches(path, {"bufferedSpawningSlotsAvailable"}) ||
               matches(path, {"curios", "*", "curioGroups"}) ||
               matches(path, {"curio_table_entries", "*", "curioGroups"}) ||
               matches(path, {"raid_finish_quirk_monster_class_ids"}) ||
               matches(path, {"narration_audio_event_queue_tags"}) || matches(path, {"dispatched_events"}) ||
               matches(path, {"combat_skills", "*", "backer_heroes"}) ||
               matches(path, {"camping_skills", "*", "backer_heroes"}) ||
               matches(path, {"quirks", "*", "backer_heroes"});
    case ValueKind::StringVector:
        return matches(path, {"goal_ids"}) || matches(path, {"roaming_dungeon_2_ids", "*", "s"}) ||
               matches(path, {"quirk_group"}) || matches(path, {"backgroundNames"}) ||
               matches(path, {"backgrounds", "*", "backgroundGroups"}) ||
               matches(path, {"background_table_entries", "*", "backgroundGroups"});
    case ValueKind::FloatArray:
        return matches(path, {"bounds", "map"}) || matches(path, {"bounds", "areas", "*"}) ||
               matches(path, {"mappos", "tiles", "*", "areas", "*"}) ||
               matches(path, {"sidepos", "tiles", "*", "areas", "*"});
    case ValueKind::TwoInteger:
        return matches(path, {"killRange"});
    default:
        return false;
    }
}

std::uint64_t aligned_start(std::uint64_t data_offset) {
    return data_offset + ((4U - (data_offset & 3U)) & 3U);
}

struct FieldSource {
    std::uint32_t meta2_index{};
    std::uint32_t name_length{};
    std::uint64_t offset{};
    std::string name;
};

std::optional<std::int32_t> signed_count(std::span<const std::byte> bytes, std::size_t offset) {
    if (bytes.size() < 4) return std::nullopt;
    return std::bit_cast<std::int32_t>(u32(bytes, offset));
}

template <typename T>
std::vector<T> copy_values(std::span<const std::byte> bytes) {
    std::vector<T> result;
    result.reserve(bytes.size() / 4);
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        if constexpr (std::is_same_v<T, float>) result.push_back(std::bit_cast<float>(u32(bytes, i)));
        else result.push_back(std::bit_cast<std::int32_t>(u32(bytes, i)));
    }
    return result;
}

} // namespace

Result<DsonDocument, Error> DsonReader::parse(std::span<const std::byte> bytes,
                                               std::string_view source_name) const {
    return parse_at_depth(bytes, source_name, 0);
}

Result<DsonDocument, Error> DsonReader::parse_at_depth(std::span<const std::byte> bytes,
                                                         std::string_view source_name,
                                                         std::size_t depth) const {
    if (depth > options_.max_embedded_depth)
        return Result<DsonDocument, Error>::failure(malformed("Embedded DSON nesting limit exceeded", source_name, "embedded", 0));
    if (bytes.size() < 64)
        return Result<DsonDocument, Error>::failure(malformed("File is shorter than the 64-byte header", source_name, "header", bytes.size()));

    DsonDocument document;
    document.source_name = std::string{source_name};
    document.original_bytes.assign(bytes.begin(), bytes.end());
    auto& h = document.header;
    std::copy_n(bytes.begin(), 4, h.magic.begin());
    std::copy_n(bytes.begin() + 4, 4, h.revision.begin());
    h.header_length = u32(bytes, 8);
    h.reserved_1 = u32(bytes, 12);
    h.meta1_size = u32(bytes, 16);
    h.meta1_count = u32(bytes, 20);
    h.meta1_offset = u32(bytes, 24);
    h.reserved_2 = u64(bytes, 28);
    h.reserved_3 = u64(bytes, 36);
    h.meta2_count = u32(bytes, 44);
    h.meta2_offset = u32(bytes, 48);
    h.reserved_4 = u32(bytes, 52);
    h.data_length = u32(bytes, 56);
    h.data_offset = u32(bytes, 60);

    if (h.magic != magic)
        return Result<DsonDocument, Error>::failure(malformed("Invalid DSON magic bytes", source_name, "header", 0));
    if (h.header_length != 64)
        return Result<DsonDocument, Error>::failure(malformed("Unsupported header length", source_name, "header", 8));
    if (h.meta1_offset != h.header_length)
        return Result<DsonDocument, Error>::failure(malformed("Meta1 does not start at the end of the header", source_name, "header", 24));

    const std::uint64_t expected_meta1_size = static_cast<std::uint64_t>(h.meta1_count) * 16U;
    const std::uint64_t expected_meta2_size = static_cast<std::uint64_t>(h.meta2_count) * 12U;
    const std::uint64_t expected_meta2_offset = static_cast<std::uint64_t>(h.meta1_offset) + expected_meta1_size;
    const std::uint64_t expected_data_offset = static_cast<std::uint64_t>(h.meta2_offset) + expected_meta2_size;
    const std::uint64_t expected_file_size = static_cast<std::uint64_t>(h.data_offset) + h.data_length;
    if (expected_meta1_size != h.meta1_size)
        return Result<DsonDocument, Error>::failure(malformed("Meta1 byte size does not match its entry count", source_name, "header", 16));
    if (expected_meta2_offset != h.meta2_offset)
        return Result<DsonDocument, Error>::failure(malformed("Meta2 offset does not follow Meta1", source_name, "header", 48));
    if (expected_data_offset != h.data_offset)
        return Result<DsonDocument, Error>::failure(malformed("Data offset does not follow Meta2", source_name, "header", 60));
    if (expected_file_size != bytes.size())
        return Result<DsonDocument, Error>::failure(malformed("Data length does not match total file size", source_name, "header", 56));

    if (!fits(bytes.size(), h.meta1_offset, expected_meta1_size))
        return Result<DsonDocument, Error>::failure(malformed("Meta1 section exceeds file bounds", source_name, "meta1", h.meta1_offset));
    if (!fits(bytes.size(), h.meta2_offset, expected_meta2_size))
        return Result<DsonDocument, Error>::failure(malformed("Meta2 section exceeds file bounds", source_name, "meta2", h.meta2_offset));
    if (!fits(bytes.size(), h.data_offset, h.data_length))
        return Result<DsonDocument, Error>::failure(malformed("Data section exceeds file bounds", source_name, "data", h.data_offset));
    if (h.meta1_count == 0 || h.meta2_count == 0)
        return Result<DsonDocument, Error>::failure(malformed("DSON document has no root/object fields", source_name, "header", 20));

    document.meta1.reserve(h.meta1_count);
    for (std::uint32_t i = 0; i < h.meta1_count; ++i) {
        const auto at = static_cast<std::size_t>(h.meta1_offset) + static_cast<std::size_t>(i) * 16;
        const auto parent = std::bit_cast<std::int32_t>(u32(bytes, at));
        const auto meta2_index = u32(bytes, at + 4);
        const auto direct = u32(bytes, at + 8);
        const auto all = u32(bytes, at + 12);
        if (parent < -1 || (parent >= 0 && static_cast<std::uint32_t>(parent) >= h.meta1_count) ||
            meta2_index >= h.meta2_count || direct > h.meta2_count || all > h.meta2_count) {
            return Result<DsonDocument, Error>::failure(malformed("Meta1 entry contains an invalid index or child count",
                source_name, "meta1", at));
        }
        document.meta1.push_back({parent, meta2_index, direct, all});
    }

    document.meta2.reserve(h.meta2_count);
    std::vector<FieldSource> sources;
    sources.reserve(h.meta2_count);
    const auto data = bytes.subspan(h.data_offset, h.data_length);
    for (std::uint32_t i = 0; i < h.meta2_count; ++i) {
        const auto at = static_cast<std::size_t>(h.meta2_offset) + static_cast<std::size_t>(i) * 12;
        Meta2Entry entry{u32(bytes, at), u32(bytes, at + 4), u32(bytes, at + 8)};
        const auto name_length = (entry.field_info & name_length_mask) >> 2U;
        if (name_length == 0 || !fits(data.size(), entry.data_offset, name_length))
            return Result<DsonDocument, Error>::failure(malformed("Meta2 field name is empty or outside the Data section",
                source_name, "meta2", at));
        const auto raw_name = data.subspan(entry.data_offset, name_length);
        if (raw_name.back() != std::byte{0} ||
            std::find(raw_name.begin(), raw_name.end() - 1, std::byte{0}) != raw_name.end() - 1 ||
            !valid_utf8(raw_name.first(name_length - 1))) {
            return Result<DsonDocument, Error>::failure(malformed("Field name is not a valid null-terminated UTF-8 string",
                source_name, "data", static_cast<std::uint64_t>(h.data_offset) + entry.data_offset));
        }
        const std::string name{reinterpret_cast<const char*>(raw_name.data()), name_length - 1};
        if (string_hash(name) != entry.name_hash)
            return Result<DsonDocument, Error>::failure(malformed("Field name hash does not match its UTF-8 name",
                source_name, "meta2", at));
        std::uint32_t meta1_index = UINT32_MAX;
        if ((entry.field_info & object_bit) != 0) {
            meta1_index = (entry.field_info & object_index_mask) >> 11U;
            if (meta1_index >= h.meta1_count)
                return Result<DsonDocument, Error>::failure(malformed("Object field points outside Meta1",
                    source_name, "meta2", at + 8));
        }
        document.meta2.push_back(entry);
        sources.push_back({i, name_length, entry.data_offset, name});
    }

    std::sort(sources.begin(), sources.end(), [](const FieldSource& a, const FieldSource& b) {
        return a.offset < b.offset;
    });
    if (sources.front().offset != 0)
        return Result<DsonDocument, Error>::failure(malformed("First field does not start at Data offset zero", source_name, "data", h.data_offset));
    for (std::size_t i = 1; i < sources.size(); ++i) {
        const auto previous_end = sources[i - 1].offset + sources[i - 1].name_length;
        if (sources[i].offset < previous_end)
            return Result<DsonDocument, Error>::failure(malformed("Field names overlap or repeat a Data offset",
                source_name, "data", static_cast<std::uint64_t>(h.data_offset) + sources[i].offset));
    }

    struct Frame { std::uint32_t meta1_index; std::size_t field_index; std::uint32_t remaining; };
    std::vector<Frame> stack;
    std::vector<bool> object_seen(h.meta1_count, false);
    std::vector<std::uint32_t> actual_descendants(h.meta1_count, 0);
    document.fields.reserve(h.meta2_count);

    for (std::size_t ordered_index = 0; ordered_index < sources.size(); ++ordered_index) {
        const auto& source = sources[ordered_index];
        const auto& meta = document.meta2[source.meta2_index];
        const bool is_object = (meta.field_info & object_bit) != 0;
        while (!stack.empty() && stack.back().remaining == 0) stack.pop_back();
        if (!stack.empty() && stack.back().remaining == 0) stack.pop_back();
        const auto parent_field = stack.empty() ? DsonField::no_index : stack.back().field_index;
        const auto parent_meta = stack.empty() ? -1 : static_cast<std::int32_t>(stack.back().meta1_index);
        if (ordered_index == 0 && (!is_object || parent_meta != -1))
            return Result<DsonDocument, Error>::failure(malformed("First Data field is not the root object",
                source_name, "data", h.data_offset + source.offset));
        if (ordered_index > 0 && stack.empty())
            return Result<DsonDocument, Error>::failure(malformed("Field appears outside the root object",
                source_name, "data", h.data_offset + source.offset));
        if (!stack.empty()) {
            if (stack.back().remaining == 0)
                return Result<DsonDocument, Error>::failure(malformed("Object contains more fields than Meta1 declares",
                    source_name, "meta1", h.meta1_offset + stack.back().meta1_index * 16U));
            --stack.back().remaining;
            for (const auto& ancestor : stack) ++actual_descendants[ancestor.meta1_index];
        }

        DsonField field;
        field.name = source.name;
        field.parent_index = parent_field;
        field.meta2_entry_index = source.meta2_index;
        field.name_hash = meta.name_hash;
        field.field_info = meta.field_info;
        field.source_name_offset = static_cast<std::uint64_t>(h.data_offset) + source.offset;
        field.source_value_offset = field.source_name_offset + source.name_length;
        field.path = parent_field == DsonField::no_index
                         ? field.name
                         : document.fields[parent_field].path + "/" + field.name;

        if (is_object) {
            field.kind = ValueKind::Object;
            field.type_evidence = TypeEvidence::Structure;
            field.meta1_entry_index = (meta.field_info & object_index_mask) >> 11U;
            const auto& object_meta = document.meta1[field.meta1_entry_index];
            if (object_seen[field.meta1_entry_index] || object_meta.meta2_entry_index != source.meta2_index ||
                object_meta.parent_index != parent_meta) {
                return Result<DsonDocument, Error>::failure(malformed("Object Meta1 parent/index relation is inconsistent",
                    source_name, "meta1", h.meta1_offset + field.meta1_entry_index * 16U));
            }
            object_seen[field.meta1_entry_index] = true;
        }

        const auto field_index = document.fields.size();
        document.fields.push_back(std::move(field));
        if (parent_field == DsonField::no_index) document.root_fields.push_back(field_index);
        else document.fields[parent_field].children.push_back(field_index);

        if (is_object) {
            const auto meta1_index = document.fields[field_index].meta1_entry_index;
            const auto child_count = document.meta1[meta1_index].direct_children;
            if (child_count > 0) stack.push_back({meta1_index, field_index, child_count});
        } else {
            const auto value_start = source.offset + source.name_length;
            const auto field_end = ordered_index + 1 < sources.size()
                                       ? sources[ordered_index + 1].offset
                                       : h.data_length;
            if (field_end < value_start || field_end > h.data_length)
                return Result<DsonDocument, Error>::failure(malformed("Primitive field has an invalid Data span",
                    source_name, "data", h.data_offset + value_start));
            const auto span = data.subspan(static_cast<std::size_t>(value_start),
                                           static_cast<std::size_t>(field_end - value_start));
            auto& stored = document.fields[field_index];
            stored.raw_data.assign(span.begin(), span.end());
            const auto aligned = aligned_start(value_start);
            const auto padding = static_cast<std::size_t>(aligned - value_start);
            const auto payload = padding <= span.size() ? span.subspan(padding) : std::span<const std::byte>{};

            auto mark_known_path = [&](ValueKind kind) {
                stored.kind = kind;
                stored.type_evidence = TypeEvidence::KnownPath;
            };
            auto mark_shape = [&](ValueKind kind) {
                stored.kind = kind;
                stored.type_evidence = TypeEvidence::ShapeHeuristic;
            };

            bool parsed = false;
            // Char and bool are the only primitive encodings that are not aligned.
            // Check the raw field span before applying the four-byte alignment rule.
            if (span.size() == 1) {
                const auto byte = std::to_integer<unsigned char>(span[0]);
                if (special_type(ValueKind::Character, stored.path) || (byte >= 0x20 && byte <= 0x7e)) {
                    stored.value = static_cast<char>(byte);
                    if (special_type(ValueKind::Character, stored.path)) mark_known_path(ValueKind::Character);
                    else mark_shape(ValueKind::Character);
                } else {
                    stored.value = span[0] != std::byte{0};
                    mark_shape(ValueKind::Boolean);
                }
                parsed = true;
            }
            for (const auto candidate : {ValueKind::FloatArray, ValueKind::IntegerVector,
                                         ValueKind::StringVector, ValueKind::Float, ValueKind::TwoInteger,
                                         ValueKind::Character}) {
                if (parsed) break;
                if (!special_type(candidate, stored.path)) continue;
                if (candidate == ValueKind::FloatArray && payload.size() % 4 == 0) {
                    stored.value = copy_values<float>(payload); mark_known_path(candidate); parsed = true;
                } else if (candidate == ValueKind::IntegerVector && payload.size() >= 4) {
                    const auto count = signed_count(payload, 0);
                    if (count && *count >= 0 && static_cast<std::uint64_t>(*count) * 4U + 4U == payload.size()) {
                        auto values = copy_values<std::int32_t>(payload.subspan(4));
                        stored.value = std::move(values); mark_known_path(candidate); parsed = true;
                    }
                } else if (candidate == ValueKind::StringVector && payload.size() >= 4) {
                    const auto count = signed_count(payload, 0);
                    if (count && *count >= 0) {
                        std::vector<std::string> values;
                        std::size_t cursor = 4;
                        bool valid = true;
                        for (std::int32_t element = 0; element < *count; ++element) {
                            if (cursor > payload.size() || payload.size() - cursor < 4) { valid = false; break; }
                            const auto length = signed_count(payload.subspan(cursor), 0);
                            cursor += 4;
                            if (!length || *length < 1 || static_cast<std::uint64_t>(*length) > payload.size() - cursor) { valid = false; break; }
                            const auto value_bytes = payload.subspan(cursor, static_cast<std::size_t>(*length));
                            if (value_bytes.back() != std::byte{0} || !valid_utf8(value_bytes.first(value_bytes.size() - 1))) { valid = false; break; }
                            values.emplace_back(reinterpret_cast<const char*>(value_bytes.data()), value_bytes.size() - 1);
                            cursor += static_cast<std::size_t>(*length);
                            if (element + 1 < *count) cursor += static_cast<std::size_t>((4U - ((aligned + cursor) & 3U)) & 3U);
                        }
                        if (valid && cursor == payload.size()) {
                            stored.value = std::move(values); mark_known_path(candidate); parsed = true;
                        }
                    }
                } else if (candidate == ValueKind::Float && payload.size() == 4) {
                    stored.value = std::bit_cast<float>(u32(payload, 0)); mark_known_path(candidate); parsed = true;
                } else if (candidate == ValueKind::TwoInteger && payload.size() == 8) {
                    stored.value = std::array<std::int32_t, 2>{std::bit_cast<std::int32_t>(u32(payload, 0)),
                                                               std::bit_cast<std::int32_t>(u32(payload, 4))};
                    mark_known_path(candidate); parsed = true;
                } else if (candidate == ValueKind::Character && payload.size() == 1) {
                    stored.value = static_cast<char>(std::to_integer<unsigned char>(payload[0]));
                    mark_known_path(candidate); parsed = true;
                }
                if (parsed) break;
            }

            if (!parsed && payload.size() == 1) {
                stored.value = payload[0] != std::byte{0};
                mark_shape(ValueKind::Boolean);
                parsed = true;
            }
            if (!parsed && payload.size() == 8 &&
                (payload[0] == std::byte{0} || payload[0] == std::byte{1}) &&
                (payload[4] == std::byte{0} || payload[4] == std::byte{1})) {
                stored.value = std::array<bool, 2>{payload[0] != std::byte{0}, payload[4] != std::byte{0}};
                mark_shape(ValueKind::TwoBoolean);
                parsed = true;
            }
            if (!parsed && payload.size() == 4) {
                stored.value = std::bit_cast<std::int32_t>(u32(payload, 0));
                mark_shape(ValueKind::Integer);
                parsed = true;
            }
            if (!parsed && payload.size() >= 5) {
                const auto length = signed_count(payload, 0);
                if (length && *length >= 64 && static_cast<std::uint64_t>(*length) + 4U <= payload.size() &&
                    std::equal(magic.begin(), magic.end(), payload.begin() + 4)) {
                    const auto embedded_bytes = payload.subspan(4, static_cast<std::size_t>(*length));
                    auto embedded = parse_at_depth(embedded_bytes, stored.path, depth + 1);
                    if (!embedded) {
                        auto error = malformed("Embedded DSON failed structural validation", source_name,
                                               "embedded", stored.source_value_offset);
                        error.context.insert_or_assign("path", stored.path);
                        error.cause = std::make_shared<Error>(embedded.error());
                        return Result<DsonDocument, Error>::failure(std::move(error));
                    }
                    stored.kind = ValueKind::EmbeddedDson;
                    stored.type_evidence = TypeEvidence::Structure;
                    stored.embedded_document = std::make_shared<DsonDocument>(std::move(embedded).value());
                    parsed = true;
                }
                if (!parsed && static_cast<std::uint64_t>(*length) + 4U == payload.size() && *length >= 1 &&
                    payload.back() == std::byte{0} && valid_utf8(payload.subspan(4, payload.size() - 5))) {
                    stored.value = std::string{reinterpret_cast<const char*>(payload.data() + 4), payload.size() - 5};
                    mark_shape(ValueKind::String);
                    parsed = true;
                }
            }
            if (!parsed) {
                stored.kind = ValueKind::Unknown;
                stored.type_evidence = TypeEvidence::Unknown;
            }
        }
    }

    while (!stack.empty() && stack.back().remaining == 0) stack.pop_back();
    if (!stack.empty())
        return Result<DsonDocument, Error>::failure(malformed("Object declares children that are missing from Data",
            source_name, "meta1", h.meta1_offset + stack.back().meta1_index * 16U));
    if (document.root_fields.size() != 1)
        return Result<DsonDocument, Error>::failure(malformed("DSON document must have exactly one root object",
            source_name, "data", h.data_offset));
    for (std::size_t i = 0; i < object_seen.size(); ++i) {
        if (!object_seen[i])
            return Result<DsonDocument, Error>::failure(malformed("Meta1 object has no matching object field",
                source_name, "meta1", h.meta1_offset + i * 16U));
        if (actual_descendants[i] != document.meta1[i].all_descendants)
            return Result<DsonDocument, Error>::failure(malformed("Meta1 descendant count does not match reconstructed tree",
                source_name, "meta1", h.meta1_offset + i * 16U + 12));
    }
    return Result<DsonDocument, Error>::success(std::move(document));
}

} // namespace ddse::core::dson
