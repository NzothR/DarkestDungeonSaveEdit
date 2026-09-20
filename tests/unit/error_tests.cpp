#include "ddse/core/error.hpp"
#include "ddse/core/result.hpp"

#include <gtest/gtest.h>

TEST(Error, ContextAdditionReturnsIndependentCopy) {
    const ddse::core::Error original{ddse::core::ErrorCode::IoError, "failed", "test"};
    const auto enriched = original.with_context("path", "save.json");
    EXPECT_TRUE(original.context.empty());
    EXPECT_EQ(enriched.context.at("path"), "save.json");
}

TEST(Result, CarriesValueOrStructuredError) {
    const auto success = ddse::core::Result<int, ddse::core::Error>::success(42);
    EXPECT_TRUE(success);
    EXPECT_EQ(success.value(), 42);

    const auto failure = ddse::core::Result<void, ddse::core::Error>::failure(
        {ddse::core::ErrorCode::InvalidConfiguration, "missing root", "config"});
    EXPECT_FALSE(failure);
    EXPECT_EQ(failure.error().code, ddse::core::ErrorCode::InvalidConfiguration);
}
