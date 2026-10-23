#include "GatewayConfig.hpp"

#include <charconv>
#include <algorithm>
#include <limits>

#include "domain/Environment.hpp"

namespace gateway {

namespace {

using domain::Error;
using domain::ErrorKind;
using Output = domain::Result<GatewayConfig>;

std::optional<int> parseInteger(const std::string_view text, const int minimum, const int maximum) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < minimum || value > maximum) {
        return std::nullopt;
    }
    return value;
}

std::optional<telemetry::Level> parseLevel(const std::string_view text) {
    if (text == "debug") return telemetry::Level::Debug;
    if (text == "info") return telemetry::Level::Info;
    if (text == "warn") return telemetry::Level::Warn;
    if (text == "error") return telemetry::Level::Error;
    return std::nullopt;
}

std::optional<std::vector<int>> parseChannelList(const std::string_view text) {
    std::vector<int> channels;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(',', start), text.size());
        const auto channel = parseInteger(text.substr(start, end - start), 0, 4095);
        if (!channel) {
            return std::nullopt;
        }
        channels.push_back(*channel);
        start = end + 1;
    }
    return channels;
}

Output invalid(std::string message) {
    return Output::failure(Error{ErrorKind::InvalidArgument, std::move(message), 0, false});
}

}

domain::Result<GatewayConfig> parseGatewayArguments(const std::span<const std::string_view> arguments) {
    GatewayConfig config;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];

        if (argument == "--help" || argument == "-h") {
            config.showHelp = true;
            continue;
        }

        if (index + 1 >= arguments.size()) {
            return invalid("missing value for " + std::string(argument));
        }
        const std::string_view value = arguments[++index];

        if (argument == "--bind") {
            if (value.empty()) {
                return invalid("--bind requires an address");
            }
            config.bindAddress = std::string(value);
        } else if (argument == "--port") {
            const auto port = parseInteger(value, 1, std::numeric_limits<std::uint16_t>::max());
            if (!port) {
                return invalid("--port must be between 1 and 65535");
            }
            config.port = static_cast<std::uint16_t>(*port);
        } else if (argument == "--log-level") {
            const auto level = parseLevel(value);
            if (!level) {
                return invalid("--log-level must be debug, info, warn or error");
            }
            config.logLevel = *level;
        } else if (argument == "--probe-channel") {
            const auto channel = parseInteger(value, 0, 4095);
            if (!channel) {
                return invalid("--probe-channel must be a non-negative channel index");
            }
            config.probeChannel = *channel;
        } else if (argument == "--probe-seconds") {
            const auto seconds = parseInteger(value, 1, 3600);
            if (!seconds) {
                return invalid("--probe-seconds must be between 1 and 3600");
            }
            config.probeSeconds = *seconds;
        } else if (argument == "--exit-after") {
            const auto seconds = parseInteger(value, 1, 86400);
            if (!seconds) {
                return invalid("--exit-after must be between 1 and 86400");
            }
            config.exitAfterSeconds = *seconds;
        } else if (argument == "--media-url") {
            if (!value.starts_with("http://") && !value.starts_with("https://")) {
                return invalid("--media-url must be an http or https URL");
            }
            config.mediaServerUrl = std::string(value);
        } else if (argument == "--publish-channels") {
            auto channels = parseChannelList(value);
            if (!channels) {
                return invalid("--publish-channels must be a comma separated list of channel indexes");
            }
            config.publishChannels = std::move(*channels);
        } else if (argument == "--migrations-dir") {
            if (value.empty()) {
                return invalid("--migrations-dir requires a directory");
            }
            config.migrationsDirectory = std::string(value);
        } else {
            return invalid("unknown argument " + std::string(argument));
        }
    }

    return Output::success(std::move(config));
}

std::optional<dahua::Credentials> credentialsFromEnvironment() {
    auto user = domain::readEnvironment("SURVEILLANCE_DAHUA_USER");
    auto password = domain::readEnvironment("SURVEILLANCE_DAHUA_PASSWORD");
    if (!user || !password || user->empty()) {
        return std::nullopt;
    }
    return dahua::Credentials(std::move(*user), std::move(*password));
}

std::string_view gatewayUsage() noexcept {
    return "dahua-gateway [--bind ADDRESS] [--port PORT] [--log-level debug|info|warn|error]\n"
           "              [--probe-channel INDEX] [--probe-seconds SECONDS] [--exit-after SECONDS]\n"
           "              [--migrations-dir DIRECTORY] [--media-url URL] [--publish-channels 0,1,2]\n"
           "\n"
           "With SURVEILLANCE_DATABASE_URL and SURVEILLANCE_SECRET_KEY set, devices and their credentials are read from PostgreSQL.\n"
           "--migrations-dir applies the SQL migrations in that directory before starting.\n"
           "--media-url publishes every online channel to that WHIP media server, for example http://127.0.0.1:8889.\n"
           "--publish-channels limits which channels are published.\n"
           "Without a database, credentials are read from SURVEILLANCE_DAHUA_USER and SURVEILLANCE_DAHUA_PASSWORD.\n"
           "--probe-channel opens that channel (0-based) on every device that comes online and reports what arrives.\n";
}

}
