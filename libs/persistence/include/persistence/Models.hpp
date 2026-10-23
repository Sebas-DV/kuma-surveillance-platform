#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace persistence {

using TimePoint = std::chrono::system_clock::time_point;

inline constexpr int kMaxChannels = 512;

enum class UserRole { Viewer, Operator, Admin };

enum class ApiKeyKind { Session, Service };

[[nodiscard]] std::string_view toString(UserRole role) noexcept;
[[nodiscard]] std::optional<UserRole> parseUserRole(std::string_view text) noexcept;
[[nodiscard]] bool atLeast(UserRole actual, UserRole required) noexcept;

[[nodiscard]] std::string_view toString(ApiKeyKind kind) noexcept;
[[nodiscard]] std::optional<ApiKeyKind> parseApiKeyKind(std::string_view text) noexcept;

struct Channel {
    int index{0};
    std::string name;
    bool enabled{true};
};

struct Device {
    std::string id;
    std::string name;
    std::string registrationId;
    std::string username;
    std::optional<std::string> serialNumber;
    int channelCount{0};
    bool enabled{true};
    std::optional<TimePoint> lastSeenAt;
    TimePoint createdAt;
    TimePoint updatedAt;
    std::vector<Channel> channels;
};

struct NewDevice {
    std::string name;
    std::string registrationId;
    std::string username;
    std::string password;
    int channelCount{0};
    std::vector<Channel> channels;
    bool enabled{true};
};

struct DevicePatch {
    std::optional<std::string> name;
    std::optional<std::string> registrationId;
    std::optional<std::string> username;
    std::optional<std::string> password;
    std::optional<bool> enabled;
};

struct DeviceCredentials {
    std::string deviceId;
    std::string registrationId;
    std::string username;
    std::string password;
};

struct User {
    std::string id;
    std::string username;
    UserRole role{UserRole::Viewer};
    bool disabled{false};
    std::optional<TimePoint> lockedUntil;
    TimePoint createdAt;
    TimePoint updatedAt;
};

struct UserPatch {
    std::optional<UserRole> role;
    std::optional<bool> disabled;
    std::optional<std::string> password;
};

struct ApiKey {
    std::string id;
    std::string userId;
    std::string name;
    ApiKeyKind kind{ApiKeyKind::Service};
    std::optional<TimePoint> expiresAt;
    std::optional<TimePoint> revokedAt;
    std::optional<TimePoint> lastUsedAt;
    TimePoint createdAt;
};

struct IssuedApiKey {
    ApiKey key;
    std::string token;
};

struct Principal {
    std::string userId;
    std::string username;
    UserRole role{UserRole::Viewer};
    std::string apiKeyId;
};

struct VideoTokenRequest {
    std::string userId;
    std::string deviceId;
    int channel{0};
    int profile{0};
    std::chrono::seconds ttl{60};
};

struct IssuedVideoToken {
    std::string id;
    std::string token;
    TimePoint expiresAt;
};

struct VideoGrant {
    std::string deviceId;
    std::string registrationId;
    int channel{0};
    int profile{0};
    std::string userId;
    TimePoint sessionEndsAt;
};

}
