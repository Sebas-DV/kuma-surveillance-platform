#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "domain/Result.hpp"

namespace dahua {

struct NetSdkConfig {
    std::chrono::milliseconds connectTimeout{5000};
    int connectAttempts{1};
};

struct DisconnectNotice {
    std::uint64_t connectionToken{0};
    std::string address;
    int port{0};
};

class NetSdkRuntime {
    struct PassKey {
        explicit PassKey() = default;
    };

public:
    using DisconnectHandler = std::function<void(const DisconnectNotice&)>;

    explicit NetSdkRuntime(PassKey key);
    NetSdkRuntime(const NetSdkRuntime&) = delete;
    NetSdkRuntime& operator=(const NetSdkRuntime&) = delete;
    NetSdkRuntime(NetSdkRuntime&&) = delete;
    NetSdkRuntime& operator=(NetSdkRuntime&&) = delete;
    ~NetSdkRuntime();

    [[nodiscard]] static domain::Result<std::shared_ptr<NetSdkRuntime>> acquire(const NetSdkConfig& config = {});

    void setDisconnectHandler(DisconnectHandler handler);

    [[nodiscard]] std::uint32_t sdkVersion() const noexcept;

    void dispatchDisconnect(const DisconnectNotice& notice) const;

private:
    [[nodiscard]] domain::Result<void> initialize(const NetSdkConfig& config);

    mutable std::mutex mutex_;
    DisconnectHandler handler_;
    bool initialized_{false};
};

}
