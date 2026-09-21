#include "ddse/core/dson/dson_reader.hpp"
#include "ddse/core/dson/dson_document_editor.hpp"
#include "ddse/core/dson/dson_writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using namespace ddse::core::dson;

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<char> raw{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes;
    bytes.reserve(raw.size());
    for (const auto byte : raw) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
    return bytes;
}

std::vector<std::byte> unknown_document() {
    std::vector<std::byte> bytes(124, std::byte{0});
    auto put = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    };
    bytes[0] = std::byte{1}; bytes[1] = std::byte{0xb1};
    put(8, 64); put(16, 16); put(20, 1); put(24, 64); put(44, 2); put(48, 80); put(56, 20); put(60, 104);
    put(64, UINT32_MAX); put(68, 0); put(72, 1); put(76, 1);
    put(80, string_hash("base_root")); put(84, 0); put(88, (10U << 2U) | 1U);
    put(92, string_hash("future")); put(96, 10); put(100, 7U << 2U);
    const std::string root{"base_root\0", 10}, child{"future\0", 7};
    std::copy(root.begin(), root.end(), reinterpret_cast<char*>(bytes.data() + 104));
    std::copy(child.begin(), child.end(), reinterpret_cast<char*>(bytes.data() + 114));
    bytes[121] = std::byte{0xaa}; bytes[122] = std::byte{0xbb}; bytes[123] = std::byte{0xcc};
    return bytes;
}

} // namespace

TEST(DsonWriter, UnmodifiedProfileDocumentsAreByteIdentical) {
    const std::filesystem::path profile{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(profile)) GTEST_SKIP() << "Optional local save sample is not present";
    DsonReader reader;
    DsonWriter writer;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(profile)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        SCOPED_TRACE(entry.path().filename().string());
        auto source = read_bytes(entry.path());
        auto document = reader.parse(source, entry.path().filename().string());
        ASSERT_TRUE(document) << document.error().message;
        auto encoded = writer.encode(document.value());
        ASSERT_TRUE(encoded) << encoded.error().message;
        EXPECT_EQ(encoded.value(), source);
        EXPECT_TRUE(writer.compare_binary(document.value(), encoded.value()).binary_identical);
        ++count;
    }
    EXPECT_EQ(count, 16U);
}

TEST(DsonWriter, DirtyIntegerRoundTripsAndDiffIdentifiesField) {
    const std::filesystem::path sample = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR} / "persist.game.json";
    if (!std::filesystem::exists(sample)) GTEST_SKIP() << "Optional local save sample is not present";
    DsonReader reader;
    DsonWriter writer;
    const auto source = read_bytes(sample);
    auto document = reader.parse(source, sample.filename().string());
    ASSERT_TRUE(document) << document.error().message;
    auto& field = *std::find_if(document.value().fields.begin(), document.value().fields.end(),
                                 [](const DsonField& f) { return f.kind == ValueKind::Integer; });
    const auto changed_value = std::get<std::int32_t>(field.value) + 1;
    const auto expected_path = field.path;
    field.replace_value(changed_value);
    auto encoded = writer.encode(document.value());
    ASSERT_TRUE(encoded) << encoded.error().message;
    const auto report = writer.compare_binary(document.value(), encoded.value());
    EXPECT_FALSE(report.binary_identical);
    EXPECT_EQ(report.field_path, expected_path);
    EXPECT_EQ(report.encoded_size, source.size());
    EXPECT_EQ(report.differing_byte_count, 1U);
    auto reparsed = reader.parse(encoded.value(), "round-trip.dson");
    ASSERT_TRUE(reparsed) << reparsed.error().message;
    const auto found = std::find_if(reparsed.value().fields.begin(), reparsed.value().fields.end(),
                                    [&](const DsonField& f) { return f.path == expected_path; });
    ASSERT_NE(found, reparsed.value().fields.end());
    EXPECT_EQ(std::get<std::int32_t>(found->value), changed_value);
}

TEST(DsonWriter, VariableLengthStringRebuildsFollowingOffsets) {
    const std::filesystem::path sample = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR} / "persist.game.json";
    if (!std::filesystem::exists(sample)) GTEST_SKIP() << "Optional local save sample is not present";
    DsonReader reader;
    DsonWriter writer;
    auto document = reader.parse(read_bytes(sample), sample.filename().string());
    ASSERT_TRUE(document) << document.error().message;
    auto found = std::find_if(document.value().fields.begin(), document.value().fields.end(),
                              [](const DsonField& f) { return f.kind == ValueKind::String; });
    ASSERT_NE(found, document.value().fields.end());
    auto changed = std::get<std::string>(found->value) + "_stage2";
    const auto path = found->path;
    found->replace_value(changed);
    auto encoded = writer.encode(document.value());
    ASSERT_TRUE(encoded) << encoded.error().message;
    const auto report = writer.compare_binary(document.value(), encoded.value());
    EXPECT_EQ(report.field_path, path);
    auto reparsed = reader.parse(encoded.value(), "string-round-trip.dson");
    ASSERT_TRUE(reparsed) << reparsed.error().message;
    const auto output_field = std::find_if(reparsed.value().fields.begin(), reparsed.value().fields.end(),
                                          [&](const DsonField& f) { return f.path == path; });
    ASSERT_NE(output_field, reparsed.value().fields.end());
    EXPECT_EQ(std::get<std::string>(output_field->value), changed);
    EXPECT_NE(encoded.value().size(), read_bytes(sample).size());
}

TEST(DsonWriter, EmbeddedDocumentCanBeEditedAndDecodedAgain) {
    const std::filesystem::path sample = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR} / "persist.roster.json";
    if (!std::filesystem::exists(sample)) GTEST_SKIP() << "Optional local save sample is not present";
    DsonReader reader;
    DsonWriter writer;
    auto document = reader.parse(read_bytes(sample), sample.filename().string());
    ASSERT_TRUE(document) << document.error().message;
    auto embedded = std::find_if(document.value().fields.begin(), document.value().fields.end(),
                                 [](const DsonField& f) { return f.embedded_document != nullptr; });
    ASSERT_NE(embedded, document.value().fields.end());
    auto nested = std::find_if(embedded->embedded_document->fields.begin(), embedded->embedded_document->fields.end(),
                               [](const DsonField& f) { return f.kind == ValueKind::Integer; });
    ASSERT_NE(nested, embedded->embedded_document->fields.end());
    const auto nested_path = nested->path;
    const auto changed = std::get<std::int32_t>(nested->value) + 1;
    nested->replace_value(changed);
    auto encoded = writer.encode(document.value());
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto reparsed = reader.parse(encoded.value(), "embedded-round-trip.dson");
    ASSERT_TRUE(reparsed) << reparsed.error().message;
    auto output_embedded = std::find_if(reparsed.value().fields.begin(), reparsed.value().fields.end(),
                                        [&](const DsonField& f) { return f.path == embedded->path; });
    ASSERT_NE(output_embedded, reparsed.value().fields.end());
    ASSERT_TRUE(output_embedded->embedded_document);
    auto output_nested = std::find_if(output_embedded->embedded_document->fields.begin(),
                                      output_embedded->embedded_document->fields.end(),
                                      [&](const DsonField& f) { return f.path == nested_path; });
    ASSERT_NE(output_nested, output_embedded->embedded_document->fields.end());
    EXPECT_EQ(std::get<std::int32_t>(output_nested->value), changed);
}

TEST(DsonWriter, UnknownValueRemainsRawAndDirtyUnknownFailsClosed) {
    const auto source = unknown_document();
    DsonReader reader;
    DsonWriter writer;
    auto parsed = reader.parse(source, "unknown-fixture.dson");
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto unchanged = writer.encode(parsed.value());
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged.value(), source);

    auto& unknown = parsed.value().fields[1];
    unknown.replace_value(std::int32_t{42});
    const auto failed = writer.encode(parsed.value());
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ddse::core::ErrorCode::DsonEncodeFailed);
    EXPECT_EQ(failed.error().context.at("path"), "base_root/future");
    EXPECT_EQ(source, unknown_document()); // encoding is in-memory and failure cannot alter its source
}

TEST(DsonWriter, DirtyValueTypeMismatchFailsBeforeProducingBytes) {
    const auto bytes = unknown_document();
    DsonReader reader;
    DsonWriter writer;
    auto document = reader.parse(bytes, "fixture.dson");
    ASSERT_TRUE(document);
    auto& field = document.value().fields[1];
    // Promote the fixture field to a typed value solely to exercise the type guard.
    field.kind = ValueKind::Integer;
    field.replace_value(std::string{"wrong type"});
    const auto failed = writer.encode(document.value());
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ddse::core::ErrorCode::DsonEncodeFailed);
}

TEST(DsonWriter, ClonesAndRenamesAnObjectSubtreeThenRoundTripsIt) {
    const std::filesystem::path sample = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR} / "persist.estate.json";
    if (!std::filesystem::exists(sample)) GTEST_SKIP() << "Optional local save sample is not present";
    DsonReader reader;
    DsonWriter writer;
    const auto source_bytes = read_bytes(sample);
    auto document = reader.parse(source_bytes, sample.filename().string());
    ASSERT_TRUE(document) << document.error().message;
    const auto original_field_count = document.value().fields.size();
    const auto appended = DsonDocumentEditor::append_clone(document.value(), "base_root/trinkets/items",
        document.value(), "base_root/trinkets/items/0", "127");
    ASSERT_TRUE(appended) << appended.error().message;

    const auto cloned_id = std::find_if(document.value().fields.begin(), document.value().fields.end(), [](const auto& field) {
        return field.path == "base_root/trinkets/items/127/id";
    });
    ASSERT_NE(cloned_id, document.value().fields.end());
    cloned_id->replace_value(std::string{"ddse_structure_test_trinket"});
    const auto cloned_amount = std::find_if(document.value().fields.begin(), document.value().fields.end(), [](const auto& field) {
        return field.path == "base_root/trinkets/items/127/amount";
    });
    ASSERT_NE(cloned_amount, document.value().fields.end());
    cloned_amount->replace_value(std::int32_t{1});
    EXPECT_EQ(source_bytes, read_bytes(sample));
    const auto added_field_count = document.value().fields.size() - original_field_count;

    auto encoded = writer.encode(document.value());
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto reparsed = reader.parse(encoded.value(), "structural-round-trip.dson");
    ASSERT_TRUE(reparsed) << reparsed.error().message;
    EXPECT_EQ(reparsed.value().fields.size(), original_field_count + added_field_count);
    const auto id = std::find_if(reparsed.value().fields.begin(), reparsed.value().fields.end(), [](const auto& field) {
        return field.path == "base_root/trinkets/items/127/id";
    });
    ASSERT_NE(id, reparsed.value().fields.end());
    EXPECT_EQ(std::get<std::string>(id->value), "ddse_structure_test_trinket");
    EXPECT_NE(encoded.value(), source_bytes);
}

TEST(DsonWriter, ClonesRosterHeroWithIndependentEmbeddedDocument) {
    const std::filesystem::path sample = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR} / "persist.roster.json";
    if (!std::filesystem::exists(sample)) GTEST_SKIP() << "Optional local save sample is not present";
    DsonReader reader;
    DsonWriter writer;
    auto document = reader.parse(read_bytes(sample), sample.filename().string());
    ASSERT_TRUE(document) << document.error().message;
    const auto source_hero = std::find_if(document.value().fields.begin(), document.value().fields.end(), [](const auto& field) {
        return field.path.starts_with("base_root/heroes/") && field.kind == ValueKind::Object &&
               field.path.find('/', std::string_view{"base_root/heroes/"}.size()) == std::string::npos;
    });
    ASSERT_NE(source_hero, document.value().fields.end());
    const auto source_hero_path = source_hero->path;
    const auto source_embedded = std::find_if(document.value().fields.begin(), document.value().fields.end(), [&](const auto& field) {
        return field.path == source_hero_path + "/hero_file_data/raw_data" && field.embedded_document;
    });
    ASSERT_NE(source_embedded, document.value().fields.end());
    const auto original_name = std::find_if(source_embedded->embedded_document->fields.begin(),
        source_embedded->embedded_document->fields.end(), [](const auto& field) {
            return field.path == "base_root/actor/name";
        });
    ASSERT_NE(original_name, source_embedded->embedded_document->fields.end());
    const auto original_name_value = std::get<std::string>(original_name->value);

    const auto appended = DsonDocumentEditor::append_clone(document.value(), "base_root/heroes",
        document.value(), source_hero_path, "999999");
    ASSERT_TRUE(appended) << appended.error().message;
    const auto copied_embedded = std::find_if(document.value().fields.begin(), document.value().fields.end(), [](const auto& field) {
        return field.path == "base_root/heroes/999999/hero_file_data/raw_data" && field.embedded_document;
    });
    ASSERT_NE(copied_embedded, document.value().fields.end());
    auto copied_name = std::find_if(copied_embedded->embedded_document->fields.begin(),
        copied_embedded->embedded_document->fields.end(), [](const auto& field) {
            return field.path == "base_root/actor/name";
        });
    ASSERT_NE(copied_name, copied_embedded->embedded_document->fields.end());
    copied_name->replace_value(std::string{"DDSE Stage10 Hero"});
    EXPECT_EQ(std::get<std::string>(original_name->value), original_name_value);

    auto encoded = writer.encode(document.value());
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto reparsed = reader.parse(encoded.value(), "hero-clone-round-trip.dson");
    ASSERT_TRUE(reparsed) << reparsed.error().message;
    const auto parsed_copy = std::find_if(reparsed.value().fields.begin(), reparsed.value().fields.end(), [](const auto& field) {
        return field.path == "base_root/heroes/999999/hero_file_data/raw_data" && field.embedded_document;
    });
    ASSERT_NE(parsed_copy, reparsed.value().fields.end());
    const auto parsed_name = std::find_if(parsed_copy->embedded_document->fields.begin(),
        parsed_copy->embedded_document->fields.end(), [](const auto& field) {
            return field.path == "base_root/actor/name";
        });
    ASSERT_NE(parsed_name, parsed_copy->embedded_document->fields.end());
    EXPECT_EQ(std::get<std::string>(parsed_name->value), "DDSE Stage10 Hero");
}
