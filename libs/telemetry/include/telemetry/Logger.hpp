#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>

namespace telemetry {

enum class Level { Trace, Debug, Info, Warn, Error };

struct Field {
    std::string key;
    std::string value;
};

class Logger {
public:
    using Sink = std::function<void(const std::string&)>;

    Logger();
    explicit Logger(Sink sink);

    void setLevel(Level level) noexcept;
    void setSink(Sink sink);

    void log(Level level, std::string_view event, std::initializer_list<Field> fields = {}) const noexcept;

    void debug(std::string_view event, std::initializer_list<Field> fields = {}) const noexcept;
    void info(std::string_view event, std::initializer_list<Field> fields = {}) const noexcept;
    void warn(std::string_view event, std::initializer_list<Field> fields = {}) const noexcept;
    void error(std::string_view event, std::initializer_list<Field> fields = {}) const noexcept;

    [[nodiscard]] std::uint64_t droppedRecords() const noexcept { return dropped_.load(); }

    [[nodiscard]] static Logger& global();

private:
    mutable std::mutex mutex_;
    Sink sink_;
    std::atomic<Level> level_{Level::Info};
    mutable std::atomic<std::uint64_t> dropped_{0};
};

}
