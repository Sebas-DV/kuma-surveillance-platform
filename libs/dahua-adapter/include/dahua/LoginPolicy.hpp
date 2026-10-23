#pragma once

#include <chrono>
#include <optional>

#include "domain/Result.hpp"

namespace dahua {

struct LoginPolicyConfig {
    std::chrono::milliseconds initialDelay{2000};
    std::chrono::milliseconds maxDelay{300000};
    double multiplier{2.0};
    std::chrono::milliseconds lockedDelay{std::chrono::minutes{35}};
    std::chrono::milliseconds rejectedDelay{std::chrono::minutes{10}};
};

class LoginPolicy {
public:
    using Clock = std::chrono::steady_clock;

    explicit LoginPolicy(LoginPolicyConfig config = {});

    [[nodiscard]] bool canAttempt(Clock::time_point now) const noexcept;
    [[nodiscard]] std::optional<Clock::time_point> nextAttemptAt() const noexcept;
    [[nodiscard]] bool blocked() const noexcept { return blocked_; }
    [[nodiscard]] unsigned consecutiveFailures() const noexcept { return failures_; }

    void recordSuccess() noexcept;
    void recordFailure(const domain::Error& error, Clock::time_point now) noexcept;
    void resetCredentials() noexcept;

private:
    LoginPolicyConfig config_;
    unsigned failures_{0};
    bool blocked_{false};
    std::optional<Clock::time_point> nextAttempt_;
};

}
