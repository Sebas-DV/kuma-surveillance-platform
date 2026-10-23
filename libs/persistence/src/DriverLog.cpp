#include "persistence/DriverLog.hpp"

#include <trantor/utils/Logger.h>

#include <cstdio>
#include <string_view>

namespace persistence {

std::string withoutRowDetails(std::string message) {
    constexpr std::string_view kMarker = "\nDETAIL:";
    constexpr std::string_view kSourceTag = "\n - ";
    const auto start = message.find(kMarker);
    if (start == std::string::npos) {
        return message;
    }
    const auto end = message.find(kSourceTag, start + kMarker.size());
    message.erase(start, end == std::string::npos ? std::string::npos : end - start);
    return message;
}

void sanitizeDriverLogs() {
    trantor::Logger::setOutputFunction(
        [](const char* message, const uint64_t length) {
            const std::string cleaned = withoutRowDetails(std::string(message, static_cast<std::size_t>(length)));
            static_cast<void>(std::fwrite(cleaned.data(), 1, cleaned.size(), stderr));
        },
        [] { static_cast<void>(std::fflush(stderr)); });
}

}
