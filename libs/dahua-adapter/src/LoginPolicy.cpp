#include "dahua/LoginPolicy.hpp"

#include <algorithm>
#include <cmath>

namespace dahua {

namespace {

std::chrono::milliseconds transientDelay(const LoginPolicyConfig& config, const unsigned failures) {
    const auto exponent = static_cast<double>(failures > 0 ? failures - 1 : 0);
    const double raw = static_cast<double>(config.initialDelay.count()) * std::pow(config.multiplier, exponent);
    const double capped = std::min(raw, static_cast<double>(config.maxDelay.count()));
    return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(capped)};
}

}

LoginPolicy::LoginPolicy(const LoginPolicyConfig config) : config_(config) {}

bool LoginPolicy::canAttempt(const Clock::time_point now) const noexcept {
    if (blocked_) {
        return false;
    }
    return !nextAttempt_.has_value() || now >= *nextAttempt_;
}

std::optional<LoginPolicy::Clock::time_point> LoginPolicy::nextAttemptAt() const noexcept {
    if (blocked_) {
        return std::nullopt;
    }
    return nextAttempt_;
}

void LoginPolicy::recordSuccess() noexcept {
    failures_ = 0;
    blocked_ = false;
    nextAttempt_.reset();
}

void LoginPolicy::recordFailure(const domain::Error& error, const Clock::time_point now) noexcept {
    ++failures_;
    switch (error.kind) {
        case domain::ErrorKind::Unauthenticated:
            blocked_ = true;
            nextAttempt_.reset();
            break;
        case domain::ErrorKind::AccountLocked:
            nextAttempt_ = now + config_.lockedDelay;
            break;
        case domain::ErrorKind::Forbidden:
        case domain::ErrorKind::Unsupported:
        case domain::ErrorKind::InvalidArgument:
            nextAttempt_ = now + config_.rejectedDelay;
            break;
        default:
            nextAttempt_ = now + transientDelay(config_, failures_);
            break;
    }
}

void LoginPolicy::resetCredentials() noexcept {
    failures_ = 0;
    blocked_ = false;
    nextAttempt_.reset();
}

}
