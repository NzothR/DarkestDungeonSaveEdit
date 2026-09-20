#include "ddse/application/application_info.hpp"
#include "ddse/core/version.hpp"
#include <gtest/gtest.h>
#include <string>

TEST(ApplicationInfo, UsesCoreVersion) {
    EXPECT_EQ(ddse::application::description(),
              "DDSE " + std::string{ddse::core::version()});
}
