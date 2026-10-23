#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dahua/Interfaces.hpp"
#include "dahua/LoginPolicy.hpp"
#include "dahua/Types.hpp"
#include "telemetry/Logger.hpp"

namespace dahua {

struct DeviceSnapshot {
    DeviceId id;
    SessionState state{SessionState::Registered};
    std::string remoteAddress;
    std::uint16_t remotePort{0};
    std::optional<DeviceInfo> info;
    unsigned failedAttempts{0};
    std::optional<domain::Error> lastError;
    std::chrono::system_clock::time_point lastSeen;
};

struct DeviceSessionManagerConfig {
    LoginPolicyConfig loginPolicy;
};

class DeviceSessionManager {
public:
    using Observer = std::function<void(const DeviceSnapshot&)>;

    DeviceSessionManager(std::shared_ptr<ISessionFactory> factory,
                         std::shared_ptr<const ICredentialProvider> credentials,
                         DeviceSessionManagerConfig config = {},
                         const telemetry::Logger& logger = telemetry::Logger::global());
    DeviceSessionManager(const DeviceSessionManager&) = delete;
    DeviceSessionManager& operator=(const DeviceSessionManager&) = delete;
    DeviceSessionManager(DeviceSessionManager&&) = delete;
    DeviceSessionManager& operator=(DeviceSessionManager&&) = delete;
    ~DeviceSessionManager();

    void setObserver(Observer observer);

    void start();
    void stop();

    void handleRegistration(const RegistrationEvent& event);
    void handleDisconnect(std::uint64_t connectionToken);
    void resetCredentials(const DeviceId& id);

    [[nodiscard]] std::vector<DeviceSnapshot> snapshot() const;
    [[nodiscard]] std::optional<DeviceSnapshot> find(const DeviceId& id) const;
    [[nodiscard]] std::shared_ptr<IDeviceSession> session(const DeviceId& id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
