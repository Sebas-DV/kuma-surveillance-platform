#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "dahua/Interfaces.hpp"
#include "persistence/DeviceRepository.hpp"
#include "telemetry/Logger.hpp"

namespace gateway {

class DbCredentialProvider final : public dahua::ICredentialProvider {
public:
    explicit DbCredentialProvider(std::shared_ptr<persistence::DeviceRepository> devices,
                                  const telemetry::Logger& logger = telemetry::Logger::global());

    [[nodiscard]] std::optional<dahua::Credentials> find(const dahua::DeviceId& id) const override;

    [[nodiscard]] bool changedSinceLastUse(const dahua::DeviceId& id) const;

private:
    [[nodiscard]] static std::string fingerprint(const domain::Result<std::optional<persistence::DeviceCredentials>>& stored);
    void remember(const dahua::DeviceId& id, std::string value) const;

    std::shared_ptr<persistence::DeviceRepository> devices_;
    const telemetry::Logger& logger_;
    mutable std::mutex mutex_;
    mutable std::map<std::string, std::string> served_;
};

}
