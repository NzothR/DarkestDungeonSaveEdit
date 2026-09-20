#include "ddse/application/logger.hpp"
#include "ddse/infrastructure/console_logger.hpp"

#include <gtest/gtest.h>

#include <sstream>

TEST(ConsoleLogger, EmitsLevelMessageAndStructuredFields) {
    std::ostringstream output;
    ddse::infrastructure::ConsoleLogger logger{output};
    logger.log(ddse::application::LogLevel::Info, "ready",
               {{"module", "test"}, {"operation", "startup"}});
    EXPECT_NE(output.str().find("INFO"), std::string::npos);
    EXPECT_NE(output.str().find("module=\"test\""), std::string::npos);
    EXPECT_NE(output.str().find("operation=\"startup\""), std::string::npos);
}
