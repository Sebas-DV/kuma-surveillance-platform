#include "Serialization.hpp"

#include <chrono>
#include <format>

namespace api {

namespace {

using persistence::TimePoint;

Json::Value optionalTime(const std::optional<TimePoint>& time) {
    return time ? Json::Value(toIso(*time)) : Json::Value(Json::nullValue);
}

}

std::string toIso(const TimePoint time) {
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::seconds>(time));
}

Json::Value toJson(const persistence::Channel& channel) {
    Json::Value json(Json::objectValue);
    json["index"] = channel.index;
    json["name"] = channel.name;
    json["enabled"] = channel.enabled;
    return json;
}

Json::Value toJson(const persistence::Device& device) {
    Json::Value json(Json::objectValue);
    json["id"] = device.id;
    json["name"] = device.name;
    json["registration_id"] = device.registrationId;
    json["username"] = device.username;
    json["serial_number"] = device.serialNumber ? Json::Value(*device.serialNumber) : Json::Value(Json::nullValue);
    json["channel_count"] = device.channelCount;
    json["enabled"] = device.enabled;
    json["last_seen_at"] = optionalTime(device.lastSeenAt);
    json["created_at"] = toIso(device.createdAt);
    json["updated_at"] = toIso(device.updatedAt);
    Json::Value channels(Json::arrayValue);
    for (const auto& channel : device.channels) {
        channels.append(toJson(channel));
    }
    json["channels"] = std::move(channels);
    return json;
}

Json::Value toJson(const persistence::User& user) {
    Json::Value json(Json::objectValue);
    json["id"] = user.id;
    json["username"] = user.username;
    json["role"] = std::string(persistence::toString(user.role));
    json["disabled"] = user.disabled;
    json["locked_until"] = optionalTime(user.lockedUntil);
    json["created_at"] = toIso(user.createdAt);
    json["updated_at"] = toIso(user.updatedAt);
    return json;
}

Json::Value toJson(const persistence::ApiKey& key) {
    Json::Value json(Json::objectValue);
    json["id"] = key.id;
    json["user_id"] = key.userId;
    json["name"] = key.name;
    json["kind"] = std::string(persistence::toString(key.kind));
    json["expires_at"] = optionalTime(key.expiresAt);
    json["revoked_at"] = optionalTime(key.revokedAt);
    json["last_used_at"] = optionalTime(key.lastUsedAt);
    json["created_at"] = toIso(key.createdAt);
    return json;
}

}
