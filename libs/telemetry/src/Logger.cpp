#include "telemetry/Logger.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <format>
#include <utility>

namespace telemetry {

namespace {

constexpr std::array<std::string_view, 5> kSensitiveKeys{
    "password", "secret", "token", "authorization", "credential"};

bool isSensitive(const std::string_view key) {
    std::string lowered(key);
    std::ranges::transform(lowered, lowered.begin(),
                           [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return std::ranges::any_of(kSensitiveKeys, [&lowered](const std::string_view sensitive) {
        return lowered.find(sensitive) != std::string::npos;
    });
}

void appendEscaped(std::string& out, const std::string_view text) {
    for (const char character : text) {
        switch (character) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(character)));
                } else {
                    out += character;
                }
        }
    }
}

void appendPair(std::string& out, const std::string_view key, const std::string_view value) {
    out += ",\"";
    appendEscaped(out, key);
    out += "\":\"";
    appendEscaped(out, isSensitive(key) ? std::string_view{"***"} : value);
    out += '"';
}

std::string_view levelName(const Level level) {
    switch (level) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warn: return "warn";
        case Level::Error: return "error";
    }
    return "info";
}

void writeToStandardError(const std::string& line) {
    static_cast<void>(std::fputs(line.c_str(), stderr));
    static_cast<void>(std::fputc('\n', stderr));
    static_cast<void>(std::fflush(stderr));
}

}

Logger::Logger() : sink_(writeToStandardError) {}

Logger::Logger(Sink sink) : sink_(std::move(sink)) {}

void Logger::setLevel(const Level level) noexcept { level_.store(level); }

void Logger::setSink(Sink sink) {
    const std::scoped_lock lock(mutex_);
    sink_ = std::move(sink);
}

void Logger::log(const Level level, const std::string_view event,
                 const std::initializer_list<Field> fields) const noexcept {
    if (level < level_.load()) {
        return;
    }

    try {
        const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
        std::string line = std::format(R"({{"ts":"{:%Y-%m-%dT%H:%M:%S}Z","level":"{}")", now, levelName(level));
        appendPair(line, "event", event);
        for (const auto& field : fields) {
            appendPair(line, field.key, field.value);
        }
        line += '}';

        const std::scoped_lock lock(mutex_);
        if (sink_) {
            sink_(line);
        }
    } catch (...) {
        dropped_.fetch_add(1);
    }
}

void Logger::debug(const std::string_view event, const std::initializer_list<Field> fields) const noexcept {
    log(Level::Debug, event, fields);
}

void Logger::info(const std::string_view event, const std::initializer_list<Field> fields) const noexcept {
    log(Level::Info, event, fields);
}

void Logger::warn(const std::string_view event, const std::initializer_list<Field> fields) const noexcept {
    log(Level::Warn, event, fields);
}

void Logger::error(const std::string_view event, const std::initializer_list<Field> fields) const noexcept {
    log(Level::Error, event, fields);
}

Logger& Logger::global() {
    static Logger instance;
    return instance;
}

}
