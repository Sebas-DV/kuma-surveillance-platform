#include <gtest/gtest.h>

#include <chrono>
#include <optional>

#include "dahua/LoginPolicy.hpp"

namespace {

using namespace std::chrono_literals;
using dahua::LoginPolicy;
using dahua::LoginPolicyConfig;
using domain::Error;
using domain::ErrorKind;

Error errorOf(const ErrorKind kind) { return Error{kind, "x", 0, false}; }

LoginPolicyConfig testConfig() {
    LoginPolicyConfig config;
    config.initialDelay = 1000ms;
    config.maxDelay = 8000ms;
    config.multiplier = 2.0;
    config.lockedDelay = 30min;
    config.rejectedDelay = 10min;
    return config;
}

TEST(LoginPolicyTest, FirstAttemptIsAlwaysAllowed) {
    const LoginPolicy policy(testConfig());
    EXPECT_TRUE(policy.canAttempt(LoginPolicy::Clock::now()));
    EXPECT_FALSE(policy.blocked());
    EXPECT_FALSE(policy.nextAttemptAt().has_value());
}

TEST(LoginPolicyTest, TransientFailuresBackOffExponentiallyAndCap) {
    LoginPolicy policy(testConfig());
    const auto start = LoginPolicy::Clock::now();

    policy.recordFailure(errorOf(ErrorKind::Unreachable), start);
    EXPECT_FALSE(policy.canAttempt(start + 999ms));
    EXPECT_TRUE(policy.canAttempt(start + 1000ms));

    policy.recordFailure(errorOf(ErrorKind::Timeout), start);
    EXPECT_EQ(policy.nextAttemptAt(), std::optional(start + 2000ms));

    policy.recordFailure(errorOf(ErrorKind::Busy), start);
    EXPECT_EQ(policy.nextAttemptAt(), std::optional(start + 4000ms));

    policy.recordFailure(errorOf(ErrorKind::Unreachable), start);
    EXPECT_EQ(policy.nextAttemptAt(), std::optional(start + 8000ms));

    policy.recordFailure(errorOf(ErrorKind::Unreachable), start);
    EXPECT_EQ(policy.nextAttemptAt(), std::optional(start + 8000ms));
    EXPECT_EQ(policy.consecutiveFailures(), 5U);
}

TEST(LoginPolicyTest, WrongCredentialsBlockUntilReset) {
    LoginPolicy policy(testConfig());
    const auto now = LoginPolicy::Clock::now();

    policy.recordFailure(errorOf(ErrorKind::Unauthenticated), now);
    EXPECT_TRUE(policy.blocked());
    EXPECT_FALSE(policy.canAttempt(now + 24h));
    EXPECT_FALSE(policy.nextAttemptAt().has_value());

    policy.resetCredentials();
    EXPECT_FALSE(policy.blocked());
    EXPECT_TRUE(policy.canAttempt(now));
    EXPECT_EQ(policy.consecutiveFailures(), 0U);
}

TEST(LoginPolicyTest, LockedAccountWaitsForTheLockoutToExpire) {
    LoginPolicy policy(testConfig());
    const auto now = LoginPolicy::Clock::now();

    policy.recordFailure(errorOf(ErrorKind::AccountLocked), now);
    EXPECT_FALSE(policy.blocked());
    EXPECT_FALSE(policy.canAttempt(now + 29min));
    EXPECT_TRUE(policy.canAttempt(now + 30min));
}

TEST(LoginPolicyTest, DeviceRejectionsUseTheLongerRetryDelay) {
    for (const auto kind : {ErrorKind::Forbidden, ErrorKind::Unsupported}) {
        LoginPolicy policy(testConfig());
        const auto now = LoginPolicy::Clock::now();
        policy.recordFailure(errorOf(kind), now);
        EXPECT_FALSE(policy.blocked());
        EXPECT_FALSE(policy.canAttempt(now + 9min));
        EXPECT_TRUE(policy.canAttempt(now + 10min));
    }
}

TEST(LoginPolicyTest, SuccessClearsAllState) {
    LoginPolicy policy(testConfig());
    const auto now = LoginPolicy::Clock::now();
    policy.recordFailure(errorOf(ErrorKind::Unreachable), now);
    policy.recordFailure(errorOf(ErrorKind::Unreachable), now);

    policy.recordSuccess();
    EXPECT_EQ(policy.consecutiveFailures(), 0U);
    EXPECT_TRUE(policy.canAttempt(now));
    EXPECT_FALSE(policy.nextAttemptAt().has_value());
}

TEST(LoginPolicyTest, SuccessAfterBackoffRestartsTheSequence) {
    LoginPolicy policy(testConfig());
    const auto now = LoginPolicy::Clock::now();
    policy.recordFailure(errorOf(ErrorKind::Unreachable), now);
    policy.recordFailure(errorOf(ErrorKind::Unreachable), now);
    policy.recordSuccess();
    policy.recordFailure(errorOf(ErrorKind::Unreachable), now);
    EXPECT_EQ(policy.nextAttemptAt(), std::optional(now + 1000ms));
}

}
