#include "security/LoginThrottle.hpp"

#include <algorithm>

namespace security {

LoginThrottle::LoginThrottle(const std::size_t maxFailures, const std::chrono::seconds window,
                             const std::size_t maxTrackedKeys)
    : maxFailures_(maxFailures), window_(window), maxTrackedKeys_(maxTrackedKeys) {}

bool LoginThrottle::blocked(const std::string& key, const Clock::time_point now) const {
    const std::scoped_lock lock(mutex_);
    const auto found = failures_.find(key);
    if (found == failures_.end()) {
        return false;
    }
    std::size_t recent = 0;
    for (const auto& at : found->second) {
        recent += now - at < window_ ? 1U : 0U;
    }
    return recent >= maxFailures_;
}

void LoginThrottle::recordFailure(const std::string& key, const Clock::time_point now) {
    const std::scoped_lock lock(mutex_);
    if (failures_.size() >= maxTrackedKeys_ && !failures_.contains(key)) {
        prune(now);
        if (failures_.size() >= maxTrackedKeys_) {
            failures_.clear();
        }
    }
    auto& entries = failures_[key];
    std::erase_if(entries, [&](const Clock::time_point& at) { return now - at >= window_; });
    entries.push_back(now);
}

void LoginThrottle::recordSuccess(const std::string& key) {
    const std::scoped_lock lock(mutex_);
    failures_.erase(key);
}

void LoginThrottle::prune(const Clock::time_point now) {
    for (auto entry = failures_.begin(); entry != failures_.end();) {
        std::erase_if(entry->second, [&](const Clock::time_point& at) { return now - at >= window_; });
        entry = entry->second.empty() ? failures_.erase(entry) : std::next(entry);
    }
}

}
