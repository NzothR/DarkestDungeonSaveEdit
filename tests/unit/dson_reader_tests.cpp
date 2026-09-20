#include "ddse/core/dson/dson_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<char> raw{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes;
    bytes.reserve(raw.size());
    for (const auto byte : raw) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
    return bytes;
}

std::string summary_text(const ddse::core::dson::DsonDocument& document) {
    return ddse::core::dson::summarize(document).to_string();
}

void collect_kind_counts(const ddse::core::dson::DsonDocument& document, std::array<std::size_t, 13>& counts) {
    for (const auto& field : document.fields) {
        ++counts[static_cast<std::size_t>(field.kind)];
        if (field.embedded_document) collect_kind_counts(*field.embedded_document, counts);
    }
}

std::vector<std::byte> make_unknown_primitive_document() {
    using ddse::core::dson::string_hash;
    std::vector<std::byte> bytes(124, std::byte{0});
    auto put_u32 = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    };
    const std::array<std::byte, 4> magic{std::byte{1}, std::byte{0xb1}, std::byte{0}, std::byte{0}};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    put_u32(8, 64);
    put_u32(16, 16); // one Meta1 entry
    put_u32(20, 1);
    put_u32(24, 64);
    put_u32(44, 2); // root object and one primitive field
    put_u32(48, 80);
    put_u32(56, 20);
    put_u32(60, 104);

    put_u32(64, UINT32_MAX); // root parent = -1
    put_u32(68, 0);          // root Meta2 index
    put_u32(72, 1);          // direct child count
    put_u32(76, 1);          // all descendants

    put_u32(80, string_hash("base_root"));
    put_u32(84, 0);
    put_u32(88, (10U << 2U) | 1U); // 10-byte name, root object
    put_u32(92, string_hash("future"));
    put_u32(96, 10);
    put_u32(100, 7U << 2U); // 7-byte name, unrecognized primitive

    const std::string root_name{"base_root\0", 10};
    const std::string child_name{"future\0", 7};
    std::copy(root_name.begin(), root_name.end(), reinterpret_cast<char*>(bytes.data() + 104));
    std::copy(child_name.begin(), child_name.end(), reinterpret_cast<char*>(bytes.data() + 114));
    bytes[121] = std::byte{0xaa};
    bytes[122] = std::byte{0xbb};
    bytes[123] = std::byte{0xcc};
    return bytes;
}

std::vector<std::byte> make_known_array_pair_document() {
    using ddse::core::dson::string_hash;
    std::vector<std::byte> bytes(196, std::byte{0});
    auto put_u32 = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    };
    const std::array<std::byte, 4> magic{std::byte{1}, std::byte{0xb1}, std::byte{0}, std::byte{0}};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    put_u32(8, 64);
    put_u32(16, 32); // two Meta1 objects
    put_u32(20, 2);
    put_u32(24, 64);
    put_u32(44, 4); // root, map object, bounds and killRange
    put_u32(48, 96);
    put_u32(56, 52);
    put_u32(60, 144);

    put_u32(64, UINT32_MAX); put_u32(68, 0); put_u32(72, 2); put_u32(76, 3);
    put_u32(80, 0);          put_u32(84, 1); put_u32(88, 1); put_u32(92, 1);

    put_u32(96, string_hash("base_root")); put_u32(100, 0);  put_u32(104, (10U << 2U) | 1U);
    put_u32(108, string_hash("map"));       put_u32(112, 10); put_u32(116, (4U << 2U) | (1U << 11U) | 1U);
    put_u32(120, string_hash("bounds"));    put_u32(124, 14); put_u32(128, 7U << 2U);
    put_u32(132, string_hash("killRange")); put_u32(136, 32); put_u32(140, 10U << 2U);

    const std::string root_name{"base_root\0", 10};
    const std::string map_name{"map\0", 4};
    const std::string bounds_name{"bounds\0", 7};
    const std::string pair_name{"killRange\0", 10};
    auto copy_name = [&](std::size_t offset, const std::string& name) {
        std::copy(name.begin(), name.end(), reinterpret_cast<char*>(bytes.data() + offset));
    };
    copy_name(144, root_name);
    copy_name(154, map_name);
    copy_name(158, bounds_name);
    put_u32(168, std::bit_cast<std::uint32_t>(1.25F));
    put_u32(172, std::bit_cast<std::uint32_t>(-2.5F));
    copy_name(176, pair_name);
    put_u32(188, 4);
    put_u32(192, 9);
    return bytes;
}

} // namespace

TEST(DsonReader, ParsesEveryProfileDocumentAndProducesStableSummaries) {
    const std::filesystem::path profile{DDSE_TEST_SAVE_PROFILE_DIR};
    if (!std::filesystem::exists(profile))
        GTEST_SKIP() << "Optional local save sample is not present: " << profile.string();

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(profile))
        if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    ASSERT_EQ(files.size(), 16U);

    ddse::core::dson::DsonReader reader;
    std::size_t total_fields = 0;
    std::size_t total_unknown = 0;
    std::size_t total_embedded = 0;
    std::size_t total_duplicates = 0;
    std::array<std::size_t, 13> kind_counts{};
    for (const auto& file : files) {
        SCOPED_TRACE(file.filename().string());
        const auto bytes = read_bytes(file);
        ASSERT_FALSE(bytes.empty());
        const auto first = reader.parse(bytes, file.filename().string());
        ASSERT_TRUE(first) << first.error().message;
        const auto second = reader.parse(bytes, file.filename().string());
        ASSERT_TRUE(second) << second.error().message;
        EXPECT_EQ(summary_text(first.value()), summary_text(second.value()));

        const auto summary = ddse::core::dson::summarize(first.value());
        total_fields += summary.fields;
        total_unknown += summary.unknown_values;
        total_embedded += summary.embedded_documents;
        total_duplicates += summary.duplicate_names;
        collect_kind_counts(first.value(), kind_counts);
        std::cout << "DSON " << file.filename().string() << ": " << summary.to_string() << '\n';
    }
    std::cout << "DSON PROFILE TOTAL: files=" << files.size() << " fields=" << total_fields
              << " unknown=" << total_unknown << " duplicates=" << total_duplicates
              << " embedded=" << total_embedded << '\n';
    for (std::size_t i = 0; i < kind_counts.size(); ++i)
        std::cout << "  kind " << ddse::core::dson::to_string(static_cast<ddse::core::dson::ValueKind>(i))
                  << '=' << kind_counts[i] << '\n';
    EXPECT_GT(total_fields, 0U);
    EXPECT_EQ(total_unknown, 0U);
    EXPECT_GT(total_embedded, 0U);
    EXPECT_GT(total_duplicates, 0U); // duplicated fields are retained, not silently discarded
    EXPECT_GT(kind_counts[static_cast<std::size_t>(ddse::core::dson::ValueKind::IntegerVector)], 0U);
    EXPECT_GT(kind_counts[static_cast<std::size_t>(ddse::core::dson::ValueKind::StringVector)], 0U);

    const std::filesystem::path backup{DDSE_TEST_SAVE_BACKUP_DIR};
    ASSERT_TRUE(std::filesystem::exists(backup));
    for (const auto& file : files) {
        const auto backup_file = backup / file.filename();
        ASSERT_TRUE(std::filesystem::exists(backup_file)) << backup_file.string();
        EXPECT_EQ(read_bytes(file), read_bytes(backup_file)) << file.filename().string();
    }
}

TEST(DsonReader, MalformedHeaderReportsDocumentSectionAndOffset) {
    const std::vector<std::byte> truncated(12, std::byte{0});
    ddse::core::dson::DsonReader reader;
    const auto result = reader.parse(truncated, "malformed-fixture.dson");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ddse::core::ErrorCode::DsonMalformed);
    EXPECT_EQ(result.error().module, "DsonReader");
    EXPECT_EQ(result.error().context.at("document"), "malformed-fixture.dson");
    EXPECT_EQ(result.error().context.at("section"), "header");
    EXPECT_EQ(result.error().context.at("offset"), "12");
}

TEST(DsonReader, CorruptSectionOffsetsFailBeforeReadingMetadata) {
    const std::filesystem::path sample = std::filesystem::path{DDSE_TEST_SAVE_PROFILE_DIR} / "persist.game.json";
    if (!std::filesystem::exists(sample)) GTEST_SKIP() << "Optional local save sample is not present";
    auto bytes = read_bytes(sample);
    ASSERT_GT(bytes.size(), 64U);
    bytes[48] = std::byte{0xff}; // meta2_offset low byte; makes section arithmetic inconsistent
    ddse::core::dson::DsonReader reader;
    const auto result = reader.parse(bytes, "corrupt-offset-fixture.dson");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ddse::core::ErrorCode::DsonMalformed);
    EXPECT_EQ(result.error().context.at("section"), "header");
    EXPECT_EQ(result.error().context.at("offset"), "48");
}

TEST(DsonReader, UnknownPrimitiveBytesArePreservedAndTruncationIsRejected) {
    const auto bytes = make_unknown_primitive_document();
    ddse::core::dson::DsonReader reader;
    const auto parsed = reader.parse(bytes, "unknown-type-fixture.dson");
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().fields.size(), 2U);
    const auto& unknown = parsed.value().fields[1];
    EXPECT_EQ(unknown.path, "base_root/future");
    EXPECT_EQ(unknown.kind, ddse::core::dson::ValueKind::Unknown);
    EXPECT_EQ(unknown.raw_data, (std::vector<std::byte>{std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}}));

    for (std::size_t length = 0; length < bytes.size(); ++length) {
        const auto truncated = reader.parse(std::span<const std::byte>{bytes.data(), length}, "truncated-fixture.dson");
        EXPECT_FALSE(truncated) << "unexpectedly accepted prefix length " << length;
    }
}

TEST(DsonReader, PathTaggedFloatArrayAndPairDecodeToTypedValues) {
    const auto bytes = make_known_array_pair_document();
    ddse::core::dson::DsonReader reader;
    const auto parsed = reader.parse(bytes, "typed-values-fixture.dson");
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().fields.size(), 4U);

    const auto& bounds = parsed.value().fields[2];
    EXPECT_EQ(bounds.path, "base_root/map/bounds");
    EXPECT_EQ(bounds.kind, ddse::core::dson::ValueKind::FloatArray);
    const auto& floats = std::get<std::vector<float>>(bounds.value);
    ASSERT_EQ(floats.size(), 2U);
    EXPECT_FLOAT_EQ(floats[0], 1.25F);
    EXPECT_FLOAT_EQ(floats[1], -2.5F);

    const auto& range = parsed.value().fields[3];
    EXPECT_EQ(range.kind, ddse::core::dson::ValueKind::TwoInteger);
    EXPECT_EQ((std::get<std::array<std::int32_t, 2>>(range.value)), (std::array<std::int32_t, 2>{4, 9}));
}
