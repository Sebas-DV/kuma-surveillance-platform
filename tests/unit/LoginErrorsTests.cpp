#include <gtest/gtest.h>

#include <array>

#include "dahua/LoginErrors.hpp"

namespace {

using dahua::loginErrorFromNative;
using domain::ErrorKind;

struct Expectation {
    int code;
    ErrorKind kind;
    bool retryable;
};

TEST(LoginErrorsTest, MapsDocumentedCodes) {
    const std::array<Expectation, 14> expectations{{
        {1, ErrorKind::Unauthenticated, false},
        {2, ErrorKind::Unauthenticated, false},
        {3, ErrorKind::Timeout, true},
        {4, ErrorKind::Busy, true},
        {5, ErrorKind::AccountLocked, true},
        {6, ErrorKind::Forbidden, false},
        {7, ErrorKind::Busy, true},
        {8, ErrorKind::Unreachable, true},
        {9, ErrorKind::Unreachable, true},
        {10, ErrorKind::Busy, true},
        {11, ErrorKind::Unsupported, false},
        {12, ErrorKind::Forbidden, false},
        {13, ErrorKind::Forbidden, false},
        {18, ErrorKind::Unsupported, false},
    }};

    for (const auto& expectation : expectations) {
        const auto error = loginErrorFromNative(expectation.code);
        EXPECT_EQ(error.kind, expectation.kind) << "code " << expectation.code;
        EXPECT_EQ(error.retryable, expectation.retryable) << "code " << expectation.code;
        EXPECT_EQ(error.nativeCode, expectation.code);
        EXPECT_FALSE(error.message.empty());
    }
}

TEST(LoginErrorsTest, ZeroCodeIsAProtocolFailure) {
    const auto error = loginErrorFromNative(0);
    EXPECT_EQ(error.kind, ErrorKind::Protocol);
    EXPECT_TRUE(error.retryable);
}

TEST(LoginErrorsTest, UnknownCodesAreRetryableInternalErrors) {
    const auto error = loginErrorFromNative(9999);
    EXPECT_EQ(error.kind, ErrorKind::Internal);
    EXPECT_TRUE(error.retryable);
    EXPECT_EQ(error.nativeCode, 9999);
}

}
