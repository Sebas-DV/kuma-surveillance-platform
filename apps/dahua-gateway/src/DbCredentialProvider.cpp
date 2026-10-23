#include "DbCredentialProvider.hpp"

#include "security/Crypto.hpp"

namespace gateway {

namespace {

constexpr std::string_view kNone = "none";
constexpr std::string_view kUnavailable = "unavailable";

}

DbCredentialProvider::DbCredentialProvider(std::shared_ptr<persistence::DeviceRepository> devices,
                                           const telemetry::Logger& logger)
    : devices_(std::move(devices)), logger_(logger) {}

std::string DbCredentialProvider::fingerprint(
    const domain::Result<std::optional<persistence::DeviceCredentials>>& stored) {
    if (!stored) {
        return std::string(kUnavailable);
    }
    const auto& found = stored.value();
    if (!found.has_value()) {
        return std::string(kNone);
    }
    std::string material = found->username;
    material.push_back('\0');
    material += found->password;
    std::string digest = security::toHex(security::sha256(material));
    security::wipe(material);
    return digest;
}

void DbCredentialProvider::remember(const dahua::DeviceId& id, std::string value) const {
    const std::scoped_lock lock(mutex_);
    served_[id.value] = std::move(value);
}

std::optional<dahua::Credentials> DbCredentialProvider::find(const dahua::DeviceId& id) const {
    auto stored = devices_->findCredentials(id.value);
    remember(id, fingerprint(stored));
    if (!stored) {
        logger_.error("credentials.lookup_failed", {{"device_id", id.value}, {"reason", stored.error().message}});
        return std::nullopt;
    }
    auto& found = stored.value();
    if (!found.has_value()) {
        logger_.warn("credentials.unknown_device", {{"device_id", id.value}});
        return std::nullopt;
    }
    auto& credentials = *found;
    dahua::Credentials result(credentials.username, credentials.password);
    security::wipe(credentials.password);
    return result;
}

bool DbCredentialProvider::changedSinceLastUse(const dahua::DeviceId& id) const {
    std::string previous;
    {
        const std::scoped_lock lock(mutex_);
        const auto found = served_.find(id.value);
        if (found == served_.end()) {
            return false;
        }
        previous = found->second;
    }
    const std::string current = fingerprint(devices_->findCredentials(id.value));
    return current != previous && current != kUnavailable;
}

}
