#include <gtest/gtest.h>

#include <optional>
#include <string_view>
#include <vector>

#include "GatewayConfig.hpp"

namespace {

using domain::ErrorKind;
using gateway::parseGatewayArguments;

TEST(GatewayConfigTest, DefaultsWhenNoArguments) {
    const std::vector<std::string_view> arguments;
    const auto result = parseGatewayArguments(arguments);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().bindAddress, "0.0.0.0");
    EXPECT_EQ(result.value().port, 9500);
    EXPECT_FALSE(result.value().probeChannel.has_value());
    EXPECT_FALSE(result.value().showHelp);
}

TEST(GatewayConfigTest, ParsesEveryOption) {
    const std::vector<std::string_view> arguments{"--bind", "127.0.0.1",   "--port",          "9600",
                                                  "--log-level", "debug", "--probe-channel", "0",
                                                  "--probe-seconds", "30", "--exit-after", "45",
                                                  "--migrations-dir", "db/migrations", "--media-url", "http://127.0.0.1:8889",
                                                  "--publish-channels", "0,2,5"};
    const auto result = parseGatewayArguments(arguments);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().exitAfterSeconds, std::optional<int>(45));
    EXPECT_EQ(result.value().mediaServerUrl, std::optional<std::string>("http://127.0.0.1:8889"));
    EXPECT_EQ(result.value().publishChannels, (std::vector<int>{0, 2, 5}));
    EXPECT_EQ(result.value().migrationsDirectory, std::optional<std::string>("db/migrations"));
    EXPECT_EQ(result.value().bindAddress, "127.0.0.1");
    EXPECT_EQ(result.value().port, 9600);
    EXPECT_EQ(result.value().logLevel, telemetry::Level::Debug);
    EXPECT_EQ(result.value().probeChannel, std::optional<int>(0));
    EXPECT_EQ(result.value().probeSeconds, 30);
}

TEST(GatewayConfigTest, HelpFlagIsRecognised) {
    const std::vector<std::string_view> arguments{"--help"};
    const auto result = parseGatewayArguments(arguments);

    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().showHelp);
}

TEST(GatewayConfigTest, RejectsInvalidValues) {
    const std::vector<std::vector<std::string_view>> invalid{
        {"--port", "0"},          {"--port", "70000"},    {"--port", "abc"},         {"--port", "95x"},
        {"--log-level", "loud"},  {"--probe-channel", "-1"}, {"--probe-seconds", "0"}, {"--bind", ""},
        {"--unknown", "1"},       {"--port"},             {"--exit-after", "0"},     {"--migrations-dir", ""},
        {"--media-url", "ftp://media"},   {"--publish-channels", "0,,1"}, {"--publish-channels", "a"},
        {"--publish-channels", "-1"},
    };

    for (const auto& arguments : invalid) {
        const auto result = parseGatewayArguments(arguments);
        ASSERT_FALSE(result.hasValue()) << arguments.front();
        EXPECT_EQ(result.error().kind, ErrorKind::InvalidArgument);
    }
}

}
