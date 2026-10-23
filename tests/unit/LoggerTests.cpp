#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "telemetry/Logger.hpp"

namespace {

using telemetry::Level;
using telemetry::Logger;

struct Capture {
    std::vector<std::string> lines;

    Logger::Sink sink() {
        return [this](const std::string& line) { lines.push_back(line); };
    }
};

TEST(LoggerTest, WritesStructuredJsonLine) {
    Capture capture;
    const Logger logger(capture.sink());
    logger.info("device.online", {{"device_id", "abc"}});

    ASSERT_EQ(capture.lines.size(), 1U);
    const std::string& line = capture.lines.front();
    EXPECT_NE(line.find("\"level\":\"info\""), std::string::npos);
    EXPECT_NE(line.find("\"event\":\"device.online\""), std::string::npos);
    EXPECT_NE(line.find("\"device_id\":\"abc\""), std::string::npos);
    EXPECT_EQ(line.front(), '{');
    EXPECT_EQ(line.back(), '}');
}

TEST(LoggerTest, RedactsSensitiveFields) {
    Capture capture;
    const Logger logger(capture.sink());
    logger.info("login", {{"user", "admin"}, {"password", "hunter2"}, {"Auth_Token", "xyz"}});

    ASSERT_EQ(capture.lines.size(), 1U);
    const std::string& line = capture.lines.front();
    EXPECT_EQ(line.find("hunter2"), std::string::npos);
    EXPECT_EQ(line.find("xyz"), std::string::npos);
    EXPECT_NE(line.find("\"user\":\"admin\""), std::string::npos);
    EXPECT_NE(line.find("\"password\":\"***\""), std::string::npos);
}

TEST(LoggerTest, EscapesControlCharactersAndQuotes) {
    Capture capture;
    const Logger logger(capture.sink());
    logger.info("evt", {{"note", "line1\n\"quoted\"\\"}});

    ASSERT_EQ(capture.lines.size(), 1U);
    EXPECT_NE(capture.lines.front().find(R"("note":"line1\n\"quoted\"\\")"), std::string::npos);
}

TEST(LoggerTest, FiltersBelowConfiguredLevel) {
    Capture capture;
    Logger logger(capture.sink());
    logger.setLevel(Level::Warn);
    logger.info("ignored");
    logger.debug("ignored");
    logger.warn("kept");
    logger.error("kept");

    EXPECT_EQ(capture.lines.size(), 2U);
}

}
