#pragma once

#include <memory>

#include "dahua/Interfaces.hpp"
#include "dahua/NetSdkRuntime.hpp"

namespace dahua {

class SdkSessionFactory final : public ISessionFactory {
public:
    explicit SdkSessionFactory(std::shared_ptr<NetSdkRuntime> runtime);

    [[nodiscard]] domain::Result<std::unique_ptr<IDeviceSession>> connect(
        const RegistrationEvent& event, const Credentials& credentials) override;

private:
    std::shared_ptr<NetSdkRuntime> runtime_;
};

class StaticCredentialProvider final : public ICredentialProvider {
public:
    explicit StaticCredentialProvider(Credentials credentials);

    [[nodiscard]] std::optional<Credentials> find(const DeviceId& id) const override;

private:
    Credentials credentials_;
};

}
