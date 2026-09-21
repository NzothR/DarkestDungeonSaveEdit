#include "ddse/application/application_status.hpp"

#include "ddse/core/version.hpp"

#include <gtest/gtest.h>

TEST(ApplicationStatus, ReportsGatewayContractAndCurrentVersion) {
    const ddse::application::ApplicationStatusService service;
    const auto status = service.get_status();

    EXPECT_EQ(status.api_version, 1U);
    EXPECT_EQ(status.application_name, "Darkest Dungeon Save Editor");
    EXPECT_EQ(status.application_version, ddse::core::version());
    EXPECT_EQ(status.state, "ready");
}
