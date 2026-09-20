#include "fixture_paths.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <string_view>

TEST(FixtureLayout, RequiredFoldersExist) {
    const auto root = ddse::test::fixture_root();
    for (const std::string_view folder : {"dson", "saves", "content", "environments"}) {
        EXPECT_TRUE(std::filesystem::is_directory(root / folder)) << folder;
    }
}
