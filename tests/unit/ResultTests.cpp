#include <gtest/gtest.h>

#include <memory>

#include "domain/Result.hpp"

namespace {

using domain::Error;
using domain::ErrorKind;
using domain::Result;

TEST(ResultTest, SuccessCarriesValue) {
    const auto result = Result<int>::success(42);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(static_cast<bool>(result));
    EXPECT_EQ(result.value(), 42);
}

TEST(ResultTest, FailureCarriesError) {
    const auto result = Result<int>::failure(Error{ErrorKind::Timeout, "slow", 7, true});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_EQ(result.error().message, "slow");
    EXPECT_EQ(result.error().nativeCode, 7);
    EXPECT_TRUE(result.error().retryable);
}

TEST(ResultTest, SupportsMoveOnlyValues) {
    auto result = Result<std::unique_ptr<int>>::success(std::make_unique<int>(5));
    ASSERT_TRUE(result.hasValue());
    const auto taken = std::move(result).value();
    ASSERT_NE(taken, nullptr);
    EXPECT_EQ(*taken, 5);
}

TEST(ResultTest, VoidSpecializationReportsState) {
    EXPECT_TRUE(Result<void>::success().hasValue());
    const auto failed = Result<void>::failure(Error{ErrorKind::Busy, "busy", 0, true});
    ASSERT_FALSE(failed.hasValue());
    EXPECT_EQ(failed.error().kind, ErrorKind::Busy);
}

TEST(ResultTest, ErrorKindNamesAreStable) {
    EXPECT_EQ(domain::toString(ErrorKind::AccountLocked), "account_locked");
    EXPECT_EQ(domain::toString(ErrorKind::Unauthenticated), "unauthenticated");
    EXPECT_EQ(domain::toString(ErrorKind::Internal), "internal");
}

}
