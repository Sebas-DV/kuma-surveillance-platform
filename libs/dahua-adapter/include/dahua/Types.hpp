#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <string>

namespace dahua {

struct DeviceId {
    std::string value;

    [[nodiscard]] auto operator<=>(const DeviceId&) const = default;
};

class Credentials {
public:
    Credentials() = default;
    Credentials(std::string username, std::string password);
    Credentials(const Credentials&) = default;
    Credentials(Credentials&&) noexcept = default;
    Credentials& operator=(const Credentials&) = default;
    Credentials& operator=(Credentials&&) noexcept = default;
    ~Credentials();

    [[nodiscard]] const std::string& username() const noexcept { return username_; }
    [[nodiscard]] const std::string& password() const noexcept { return password_; }

private:
    std::string username_;
    std::string password_;
};

struct DeviceInfo {
    std::string serialNumber;
    int channelCount{0};
    int alarmInputCount{0};
    int alarmOutputCount{0};
    int diskCount{0};
    int deviceType{0};
};

enum class RegistrationEventKind { Registered, Disconnected };

struct RegistrationEvent {
    RegistrationEventKind kind{RegistrationEventKind::Registered};
    DeviceId deviceId;
    std::string remoteAddress;
    std::uint16_t remotePort{0};
    bool redirectionSupported{false};
    std::chrono::system_clock::time_point receivedAt;
};

enum class SessionState { Registered, Authenticating, Online, Offline, Backoff, Blocked };

enum class StreamProfile { Main, Sub1, Sub2, Sub3 };

struct LiveStreamRequest {
    int channel{0};
    StreamProfile profile{StreamProfile::Main};
};

enum class TalkCodec { Pcm16, G711A, G711U };

struct TalkRequest {
    int channel{0};
    TalkCodec codec{TalkCodec::Pcm16};
    bool transfer{true};
};

[[nodiscard]] const char* toString(SessionState state) noexcept;

}
