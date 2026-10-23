#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace security {

class LoginThrottle {
public:
    using Clock = std::chrono::steady_clock;

    LoginThrottle(std::size_t maxFailures, std::chrono::seconds window, std::size_t maxTrackedKeys = 10000);

    [[nodiscard]] bool blocked(const std::string& key, Clock::time_point now = Clock::now()) const;
    void recordFailure(const std::string& key, Clock::time_point now = Clock::now());
    void recordSuccess(const std::string& key);

private:
    void prune(Clock::time_point now);

    std::size_t maxFailures_;
    std::chrono::seconds window_;
    std::size_t maxTrackedKeys_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<Clock::time_point>> failures_;
};

}
