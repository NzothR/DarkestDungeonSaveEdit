#include "ddse/infrastructure/platform_info.hpp"
#include <gtest/gtest.h>

TEST(PlatformInfo, IsNotEmpty) {
    EXPECT_FALSE(ddse::infrastructure::platform_name().empty());
}

TEST(PlatformInfo, IsKnownPlatformName) {
    const auto platform = ddse::infrastructure::platform_name();
    EXPECT_TRUE(platform == "Windows" || platform == "Linux" || platform == "Other");
}
