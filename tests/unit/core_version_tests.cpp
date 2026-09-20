#include "ddse/core/version.hpp"
#include <gtest/gtest.h>

TEST(CoreVersion, ReportsExpectedVersion) {
    EXPECT_EQ(ddse::core::version(), "0.1.0");
}

TEST(CoreVersion, IsNotEmpty) {
    EXPECT_FALSE(ddse::core::version().empty());
}
