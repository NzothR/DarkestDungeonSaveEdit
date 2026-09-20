#include "ddse/core/dson/dson_document.hpp"

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>

namespace ddse::core::dson {

std::string_view to_string(ValueKind kind) noexcept {
    switch (kind) {
    case ValueKind::Object: return "Object";
    case ValueKind::Boolean: return "Boolean";
    case ValueKind::Character: return "Character";
    case ValueKind::TwoBoolean: return "TwoBoolean";
    case ValueKind::String: return "String";
    case ValueKind::Integer: return "Integer";
    case ValueKind::Float: return "Float";
    case ValueKind::IntegerVector: return "IntegerVector";
    case ValueKind::StringVector: return "StringVector";
    case ValueKind::FloatArray: return "FloatArray";
    case ValueKind::TwoInteger: return "TwoInteger";
    case ValueKind::EmbeddedDson: return "EmbeddedDson";
    case ValueKind::Unknown: return "Unknown";
    }
    return "Invalid";
}

std::string_view to_string(TypeEvidence evidence) noexcept {
    switch (evidence) {
    case TypeEvidence::Structure: return "Structure";
    case TypeEvidence::KnownPath: return "KnownPath";
    case TypeEvidence::ShapeHeuristic: return "ShapeHeuristic";
    case TypeEvidence::Unknown: return "Unknown";
    }
    return "Invalid";
}

std::uint32_t string_hash(std::string_view utf8) noexcept {
    std::uint32_t hash = 0;
    for (const auto byte : utf8) hash = hash * 53U + static_cast<unsigned char>(byte);
    return hash;
}

namespace {
void mix(std::uint64_t& hash, std::uint8_t byte) {
    hash ^= byte;
    hash *= 1099511628211ULL;
}

void mix_string(std::uint64_t& hash, std::string_view value) {
    for (const unsigned char byte : value) mix(hash, byte);
    mix(hash, 0xffU);
}

void summarize_document(const DsonDocument& doc, StructureSummary& summary,
                        std::uint64_t& fingerprint, std::size_t depth) {
    ++summary.embedded_documents;
    summary.max_depth = std::max(summary.max_depth, depth);
    std::vector<std::set<std::string>> names(doc.fields.size() + 1);
    for (std::size_t index = 0; index < doc.fields.size(); ++index) {
        const auto& field = doc.fields[index];
        ++summary.fields;
        if (field.kind == ValueKind::Object) ++summary.objects;
        if (field.kind == ValueKind::Unknown) ++summary.unknown_values;
        const auto parent_slot = field.parent_index == DsonField::no_index ? doc.fields.size() : field.parent_index;
        if (!names[parent_slot].insert(field.name).second) ++summary.duplicate_names;

        mix_string(fingerprint, field.name);
        mix_string(fingerprint, field.path);
        mix(fingerprint, static_cast<std::uint8_t>(field.kind));
        for (unsigned shift = 0; shift < 32; shift += 8) {
            mix(fingerprint, static_cast<std::uint8_t>((field.field_info >> shift) & 0xffU));
            mix(fingerprint, static_cast<std::uint8_t>((field.name_hash >> shift) & 0xffU));
        }
        for (const auto byte : field.raw_data) mix(fingerprint, std::to_integer<std::uint8_t>(byte));
        mix(fingerprint, 0xfeU);
        if (field.embedded_document)
            summarize_document(*field.embedded_document, summary, fingerprint, depth + 1);
    }
}
} // namespace

StructureSummary summarize(const DsonDocument& document) {
    StructureSummary result;
    std::uint64_t fingerprint = 14695981039346656037ULL;
    summarize_document(document, result, fingerprint, 0);
    result.fingerprint = fingerprint;
    if (result.embedded_documents > 0) --result.embedded_documents;
    return result;
}

std::string StructureSummary::to_string() const {
    std::ostringstream out;
    out << "fields=" << fields << " objects=" << objects << " duplicates=" << duplicate_names
        << " unknown=" << unknown_values << " embedded=" << embedded_documents
        << " depth=" << max_depth << " fingerprint=" << std::hex << std::setw(16)
        << std::setfill('0') << fingerprint;
    return out.str();
}

} // namespace ddse::core::dson
