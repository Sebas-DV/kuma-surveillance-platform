#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "domain/Result.hpp"
#include "persistence/Database.hpp"
#include "persistence/Models.hpp"
#include "security/SecretBox.hpp"

namespace persistence {

class DeviceRepository {
public:
    DeviceRepository(std::shared_ptr<Database> database, security::SecretBox secretBox);

    [[nodiscard]] domain::Result<Device> create(const NewDevice& request);
    [[nodiscard]] domain::Result<Device> get(std::string_view id);
    [[nodiscard]] domain::Result<Device> getByRegistrationId(std::string_view registrationId);
    [[nodiscard]] domain::Result<std::vector<Device>> list();
    [[nodiscard]] domain::Result<Device> update(std::string_view id, const DevicePatch& patch);
    [[nodiscard]] domain::Result<void> replaceChannels(std::string_view id, const std::vector<Channel>& channels);
    [[nodiscard]] domain::Result<void> remove(std::string_view id);
    [[nodiscard]] domain::Result<std::optional<DeviceCredentials>> findCredentials(std::string_view registrationId);
    [[nodiscard]] domain::Result<void> recordSeen(std::string_view registrationId, std::string_view serialNumber,
                                                  int channelCount);

private:
    std::shared_ptr<Database> database_;
    security::SecretBox secretBox_;
};

}
