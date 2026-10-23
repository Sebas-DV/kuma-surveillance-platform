#include <gtest/gtest.h>

#include "security/LoginThrottle.hpp"

namespace {

using security::LoginThrottle;
using namespace std::chrono_literals;

const LoginThrottle::Clock::time_point gStart = LoginThrottle::Clock::time_point{} + 1000s;

TEST(LoginThrottleTest, BlocksAfterTheConfiguredNumberOfFailures) {
    LoginThrottle throttle(3, 60s);
    EXPECT_FALSE(throttle.blocked("10.0.0.1", gStart));
    throttle.recordFailure("10.0.0.1", gStart);
    throttle.recordFailure("10.0.0.1", gStart + 1s);
    EXPECT_FALSE(throttle.blocked("10.0.0.1", gStart + 2s));
    throttle.recordFailure("10.0.0.1", gStart + 2s);
    EXPECT_TRUE(throttle.blocked("10.0.0.1", gStart + 3s));
}

TEST(LoginThrottleTest, KeysAreIndependent) {
    LoginThrottle throttle(1, 60s);
    throttle.recordFailure("10.0.0.1", gStart);
    EXPECT_TRUE(throttle.blocked("10.0.0.1", gStart));
    EXPECT_FALSE(throttle.blocked("10.0.0.2", gStart));
}

TEST(LoginThrottleTest, FailuresExpireWithTheWindow) {
    LoginThrottle throttle(2, 60s);
    throttle.recordFailure("a", gStart);
    throttle.recordFailure("a", gStart + 10s);
    EXPECT_TRUE(throttle.blocked("a", gStart + 20s));
    EXPECT_TRUE(throttle.blocked("a", gStart + 65s) == false);
}

TEST(LoginThrottleTest, SuccessClearsTheHistory) {
    LoginThrottle throttle(2, 60s);
    throttle.recordFailure("a", gStart);
    throttle.recordFailure("a", gStart);
    ASSERT_TRUE(throttle.blocked("a", gStart));
    throttle.recordSuccess("a");
    EXPECT_FALSE(throttle.blocked("a", gStart));
}

TEST(LoginThrottleTest, TrackedKeysAreBounded) {
    LoginThrottle throttle(1, 60s, 4);
    for (int index = 0; index < 50; ++index) {
        throttle.recordFailure("client-" + std::to_string(index), gStart);
    }
    throttle.recordFailure("latest", gStart);
    EXPECT_TRUE(throttle.blocked("latest", gStart));
}

}
