#include "ddse/infrastructure/native_file_system.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

namespace {
std::filesystem::path unique_temp_dir() {
    return std::filesystem::temp_directory_path() /
           ("ddse-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}
}

TEST(NativeFileSystem, RoundTripsBytesAndReturnsStructuredMissingFileError) {
    const auto root = unique_temp_dir();
    ddse::infrastructure::NativeFileSystem fs;
    ASSERT_TRUE(fs.create_directories(root / "nested"));
    const std::string payload{"first\0second", 12};
    ASSERT_TRUE(fs.write_file(root / "nested" / "data.bin", payload));
    const auto loaded = fs.read_file(root / "nested" / "data.bin");
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded.value(), payload);

    const auto missing = fs.read_file(root / "missing.bin");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ddse::core::ErrorCode::FileNotFound);
    EXPECT_EQ(missing.error().module, "NativeFileSystem");
    std::filesystem::remove_all(root);
}
