#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "dahua/NetSdkRuntime.hpp"
#include "dahua/Types.hpp"
#include "domain/Result.hpp"

namespace dahua {

struct RegistrationServerConfig {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t port{9500};
    std::size_t queueCapacity{1024};
};

class RegistrationServer {
public:
    using Handler = std::function<void(const RegistrationEvent&)>;

    RegistrationServer(std::shared_ptr<NetSdkRuntime> runtime, RegistrationServerConfig config, Handler handler);
    RegistrationServer(const RegistrationServer&) = delete;
    RegistrationServer& operator=(const RegistrationServer&) = delete;
    RegistrationServer(RegistrationServer&&) = delete;
    RegistrationServer& operator=(RegistrationServer&&) = delete;
    ~RegistrationServer();

    [[nodiscard]] domain::Result<void> start();
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint64_t droppedEvents() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
