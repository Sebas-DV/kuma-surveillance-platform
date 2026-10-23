#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dahua/Types.hpp"
#include "domain/Result.hpp"
#include "telemetry/Logger.hpp"

namespace gateway {

struct GatewayConfig {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t port{9500};
    telemetry::Level logLevel{telemetry::Level::Info};
    std::optional<int> probeChannel;
    int probeSeconds{10};
    std::optional<int> exitAfterSeconds;
    std::optional<std::string> migrationsDirectory;
    std::optional<std::string> mediaServerUrl;
    std::vector<int> publishChannels;
    bool showHelp{false};
};

[[nodiscard]] domain::Result<GatewayConfig> parseGatewayArguments(std::span<const std::string_view> arguments);

[[nodiscard]] std::optional<dahua::Credentials> credentialsFromEnvironment();

[[nodiscard]] std::string_view gatewayUsage() noexcept;

}
