#include "persistence/DeviceRepository.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "detail/Sql.hpp"

namespace persistence {

namespace {

using domain::Error;
using domain::ErrorKind;

constexpr std::size_t kMaxNameLength = 120;
constexpr std::size_t kMaxRegistrationIdLength = 64;
constexpr std::size_t kMaxUsernameLength = 64;
constexpr std::size_t kMaxPasswordLength = 128;

const std::string& deviceColumns() {
    static const std::string columns =
        "id::text, name, registration_id, dvr_username, serial_number, channel_count, enabled, " +
        detail::epochMillis("last_seen_at") + ", " + detail::epochMillis("created_at") + ", " +
        detail::epochMillis("updated_at");
    return columns;
}

Device deviceFromRow(const Rows& rows, const std::size_t row) {
    Device device;
    device.id = rows.text(row, 0);
    device.name = rows.text(row, 1);
    device.registrationId = rows.text(row, 2);
    device.username = rows.text(row, 3);
    device.serialNumber = rows.optionalText(row, 4);
    device.channelCount = static_cast<int>(rows.integer(row, 5));
    device.enabled = rows.boolean(row, 6);
    device.lastSeenAt = rows.optionalTime(row, 7);
    device.createdAt = rows.time(row, 8);
    device.updatedAt = rows.time(row, 9);
    return device;
}

domain::Result<void> loadChannels(const Session& session, std::vector<Device>& devices) {
    if (devices.empty()) {
        return domain::Result<void>::success();
    }
    std::string ids = "{";
    for (const auto& device : devices) {
        if (ids.size() > 1) {
            ids += ',';
        }
        ids += device.id;
    }
    ids += '}';

    const auto rows = session.query(
        "SELECT device_id::text, channel_index, name, enabled FROM device_channels "
        "WHERE device_id = ANY($1::uuid[]) ORDER BY device_id, channel_index",
        {ids});
    if (!rows) {
        return domain::Result<void>::failure(rows.error());
    }
    std::map<std::string, Device*> byId;
    for (auto& device : devices) {
        byId.emplace(device.id, &device);
    }
    for (std::size_t row = 0; row < rows.value().size(); ++row) {
        const auto found = byId.find(rows.value().text(row, 0));
        if (found == byId.end()) {
            continue;
        }
        found->second->channels.push_back(Channel{static_cast<int>(rows.value().integer(row, 1)),
                                                  rows.value().text(row, 2), rows.value().boolean(row, 3)});
    }
    return domain::Result<void>::success();
}

domain::Result<Device> fetchDevice(const Session& session, const std::string_view condition, const Param& value) {
    const auto rows = session.query("SELECT " + deviceColumns() + " FROM devices WHERE " + std::string(condition),
                                    {value});
    if (!rows) {
        return domain::Result<Device>::failure(rows.error());
    }
    if (rows.value().size() == 0) {
        return domain::Result<Device>::failure(detail::notFound("device not found"));
    }
    std::vector<Device> devices;
    devices.push_back(deviceFromRow(rows.value(), 0));
    if (const auto loaded = loadChannels(session, devices); !loaded) {
        return domain::Result<Device>::failure(loaded.error());
    }
    return domain::Result<Device>::success(std::move(devices.front()));
}

Error translateWriteError(const Error& error) {
    if (detail::mentions(error, "devices_registration_id_key")) {
        return Error{ErrorKind::AlreadyExists, "registration id is already assigned to another device", 0, false};
    }
    if (detail::mentions(error, "devices_name_key")) {
        return Error{ErrorKind::AlreadyExists, "device name is already in use", 0, false};
    }
    return error;
}

domain::Result<void> validateText(const std::string_view value, const std::size_t limit, const std::string_view field) {
    if (detail::isBlank(value) || value.size() > limit) {
        return domain::Result<void>::failure(
            detail::invalid(std::string(field) + " must be between 1 and " + std::to_string(limit) + " characters"));
    }
    return domain::Result<void>::success();
}

domain::Result<void> validateChannels(const std::vector<Channel>& channels) {
    std::set<int> seen;
    for (const auto& channel : channels) {
        if (channel.index < 0 || channel.index >= kMaxChannels) {
            return domain::Result<void>::failure(detail::invalid("channel index out of range"));
        }
        if (!seen.insert(channel.index).second) {
            return domain::Result<void>::failure(detail::invalid("duplicated channel index"));
        }
        if (const auto name = validateText(channel.name, kMaxNameLength, "channel name"); !name) {
            return name;
        }
    }
    return domain::Result<void>::success();
}

domain::Result<void> insertChannels(const Session& session, const std::string& deviceId,
                                    const std::vector<Channel>& channels) {
    for (const auto& channel : channels) {
        const auto inserted =
            session.query("INSERT INTO device_channels (device_id, channel_index, name, enabled) "
                          "VALUES ($1::uuid, $2::int, $3, $4::boolean)",
                          {deviceId, std::to_string(channel.index), channel.name, channel.enabled ? "true" : "false"});
        if (!inserted) {
            return domain::Result<void>::failure(inserted.error());
        }
    }
    return domain::Result<void>::success();
}

std::vector<Channel> defaultChannels(const int count) {
    std::vector<Channel> channels;
    channels.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        channels.push_back(Channel{index, "Canal " + std::to_string(index + 1), true});
    }
    return channels;
}

int requiredChannelCount(const std::vector<Channel>& channels) {
    int required = 0;
    for (const auto& channel : channels) {
        required = std::max(required, channel.index + 1);
    }
    return required;
}

}

DeviceRepository::DeviceRepository(std::shared_ptr<Database> database, security::SecretBox secretBox)
    : database_(std::move(database)), secretBox_(std::move(secretBox)) {}

domain::Result<Device> DeviceRepository::create(const NewDevice& request) {
    using Outcome = domain::Result<Device>;

    for (const auto& check : {validateText(request.name, kMaxNameLength, "name"),
                              validateText(request.registrationId, kMaxRegistrationIdLength, "registration id"),
                              validateText(request.username, kMaxUsernameLength, "dvr username"),
                              validateText(request.password, kMaxPasswordLength, "dvr password")}) {
        if (!check) {
            return Outcome::failure(check.error());
        }
    }
    if (request.channelCount < 0 || request.channelCount > kMaxChannels) {
        return Outcome::failure(detail::invalid("channel count out of range"));
    }
    if (const auto channels = validateChannels(request.channels); !channels) {
        return Outcome::failure(channels.error());
    }

    const int channelCount = std::max(request.channelCount, requiredChannelCount(request.channels));
    const std::vector<Channel> channels = request.channels.empty() ? defaultChannels(channelCount) : request.channels;

    const auto sealed = secretBox_.seal(request.password);
    if (!sealed) {
        return Outcome::failure(sealed.error());
    }
    const std::string encodedPassword = "\\x" + security::toHex(sealed.value());

    auto created = database_->transact([&](const Session& session) -> Outcome {
        const auto inserted = session.query(
            "INSERT INTO devices (name, registration_id, dvr_username, dvr_password_encrypted, channel_count, enabled) "
            "VALUES ($1, $2, $3, $4::bytea, $5::int, $6::boolean) ON CONFLICT DO NOTHING RETURNING id::text",
            {request.name, request.registrationId, request.username, encodedPassword, std::to_string(channelCount),
             request.enabled ? "true" : "false"});
        if (!inserted) {
            return Outcome::failure(translateWriteError(inserted.error()));
        }
        if (inserted.value().size() == 0) {
            const auto clash = session.query("SELECT registration_id = $1 FROM devices "
                                             "WHERE registration_id = $1 OR lower(name) = lower($2) LIMIT 1",
                                             {request.registrationId, request.name});
            if (!clash) {
                return Outcome::failure(clash.error());
            }
            const bool byRegistration = clash.value().size() > 0 && clash.value().boolean(0, 0);
            return Outcome::failure(Error{ErrorKind::AlreadyExists,
                                          byRegistration ? "registration id is already assigned to another device"
                                                         : "device name is already in use",
                                          0, false});
        }
        const std::string id = inserted.value().text(0, 0);
        if (const auto stored = insertChannels(session, id, channels); !stored) {
            return Outcome::failure(stored.error());
        }
        return fetchDevice(session, "id = $1::uuid", id);
    });
    return created;
}

domain::Result<Device> DeviceRepository::get(const std::string_view id) {
    if (!detail::isUuid(id)) {
        return domain::Result<Device>::failure(detail::invalid("device id must be a uuid"));
    }
    auto acquired = database_->session();
    if (!acquired) {
        return domain::Result<Device>::failure(acquired.error());
    }
    return fetchDevice(acquired.value(), "id = $1::uuid", std::string(id));
}

domain::Result<Device> DeviceRepository::getByRegistrationId(const std::string_view registrationId) {
    auto acquired = database_->session();
    if (!acquired) {
        return domain::Result<Device>::failure(acquired.error());
    }
    return fetchDevice(acquired.value(), "registration_id = $1", std::string(registrationId));
}

domain::Result<std::vector<Device>> DeviceRepository::list() {
    using Outcome = domain::Result<std::vector<Device>>;
    auto acquired = database_->session();
    if (!acquired) {
        return Outcome::failure(acquired.error());
    }
    auto& session = acquired.value();
    const auto rows = session.query("SELECT " + deviceColumns() + " FROM devices ORDER BY lower(name)");
    if (!rows) {
        return Outcome::failure(rows.error());
    }
    std::vector<Device> devices;
    devices.reserve(rows.value().size());
    for (std::size_t row = 0; row < rows.value().size(); ++row) {
        devices.push_back(deviceFromRow(rows.value(), row));
    }
    if (const auto loaded = loadChannels(session, devices); !loaded) {
        return Outcome::failure(loaded.error());
    }
    return Outcome::success(std::move(devices));
}

domain::Result<Device> DeviceRepository::update(const std::string_view id, const DevicePatch& patch) {
    using Outcome = domain::Result<Device>;
    if (!detail::isUuid(id)) {
        return Outcome::failure(detail::invalid("device id must be a uuid"));
    }
    if (patch.name) {
        if (const auto check = validateText(*patch.name, kMaxNameLength, "name"); !check) {
            return Outcome::failure(check.error());
        }
    }
    if (patch.registrationId) {
        if (const auto check = validateText(*patch.registrationId, kMaxRegistrationIdLength, "registration id");
            !check) {
            return Outcome::failure(check.error());
        }
    }
    if (patch.username) {
        if (const auto check = validateText(*patch.username, kMaxUsernameLength, "dvr username"); !check) {
            return Outcome::failure(check.error());
        }
    }

    Param encodedPassword;
    if (patch.password) {
        if (const auto check = validateText(*patch.password, kMaxPasswordLength, "dvr password"); !check) {
            return Outcome::failure(check.error());
        }
        const auto sealed = secretBox_.seal(*patch.password);
        if (!sealed) {
            return Outcome::failure(sealed.error());
        }
        encodedPassword = "\\x" + security::toHex(sealed.value());
    }
    Param enabled;
    if (patch.enabled) {
        enabled = *patch.enabled ? "true" : "false";
    }

    auto acquired = database_->session();
    if (!acquired) {
        return Outcome::failure(acquired.error());
    }
    auto& session = acquired.value();
    const auto updated = session.query(
        "UPDATE devices SET name = COALESCE($2, name), registration_id = COALESCE($3, registration_id), "
        "dvr_username = COALESCE($4, dvr_username), "
        "dvr_password_encrypted = COALESCE($5::bytea, dvr_password_encrypted), "
        "enabled = COALESCE($6::boolean, enabled), updated_at = now() WHERE id = $1::uuid RETURNING id::text",
        {std::string(id), patch.name, patch.registrationId, patch.username, encodedPassword, enabled});
    if (!updated) {
        return Outcome::failure(translateWriteError(updated.error()));
    }
    if (updated.value().size() == 0) {
        return Outcome::failure(detail::notFound("device not found"));
    }
    return fetchDevice(session, "id = $1::uuid", std::string(id));
}

domain::Result<void> DeviceRepository::replaceChannels(const std::string_view id,
                                                       const std::vector<Channel>& channels) {
    using Outcome = domain::Result<void>;
    if (!detail::isUuid(id)) {
        return Outcome::failure(detail::invalid("device id must be a uuid"));
    }
    if (const auto check = validateChannels(channels); !check) {
        return check;
    }
    const std::string deviceId(id);
    const int required = requiredChannelCount(channels);

    return database_->transact([&](const Session& session) -> Outcome {
        const auto locked = session.query("SELECT 1 FROM devices WHERE id = $1::uuid FOR UPDATE", {deviceId});
        if (!locked) {
            return Outcome::failure(locked.error());
        }
        if (locked.value().size() == 0) {
            return Outcome::failure(detail::notFound("device not found"));
        }
        if (const auto cleared = session.query("DELETE FROM device_channels WHERE device_id = $1::uuid", {deviceId});
            !cleared) {
            return Outcome::failure(cleared.error());
        }
        if (const auto stored = insertChannels(session, deviceId, channels); !stored) {
            return stored;
        }
        const auto touched = session.query(
            "UPDATE devices SET channel_count = GREATEST(channel_count, $2::int), updated_at = now() "
            "WHERE id = $1::uuid",
            {deviceId, std::to_string(required)});
        if (!touched) {
            return Outcome::failure(touched.error());
        }
        return Outcome::success();
    });
}

domain::Result<void> DeviceRepository::remove(const std::string_view id) {
    if (!detail::isUuid(id)) {
        return domain::Result<void>::failure(detail::invalid("device id must be a uuid"));
    }
    const auto removed = database_->query("DELETE FROM devices WHERE id = $1::uuid", {std::string(id)});
    if (!removed) {
        return domain::Result<void>::failure(removed.error());
    }
    if (removed.value().affected() == 0) {
        return domain::Result<void>::failure(detail::notFound("device not found"));
    }
    return domain::Result<void>::success();
}

domain::Result<std::optional<DeviceCredentials>> DeviceRepository::findCredentials(
    const std::string_view registrationId) {
    using Outcome = domain::Result<std::optional<DeviceCredentials>>;
    const auto rows = database_->query(
        "SELECT id::text, registration_id, dvr_username, dvr_password_encrypted FROM devices "
        "WHERE registration_id = $1 AND enabled",
        {std::string(registrationId)});
    if (!rows) {
        return Outcome::failure(rows.error());
    }
    if (rows.value().size() == 0) {
        return Outcome::success(std::nullopt);
    }
    auto password = secretBox_.open(rows.value().bytes(0, 3));
    if (!password) {
        return Outcome::failure(password.error());
    }
    DeviceCredentials credentials;
    credentials.deviceId = rows.value().text(0, 0);
    credentials.registrationId = rows.value().text(0, 1);
    credentials.username = rows.value().text(0, 2);
    credentials.password = std::move(password).value();
    return Outcome::success(std::move(credentials));
}

domain::Result<void> DeviceRepository::recordSeen(const std::string_view registrationId,
                                                  const std::string_view serialNumber, const int channelCount) {
    using Outcome = domain::Result<void>;
    if (channelCount < 0 || channelCount > kMaxChannels) {
        return Outcome::failure(detail::invalid("channel count out of range"));
    }
    return database_->transact([&](const Session& session) -> Outcome {
        const auto updated = session.query(
            "UPDATE devices SET last_seen_at = now(), serial_number = COALESCE(NULLIF($2, ''), serial_number), "
            "channel_count = CASE WHEN $3::int > 0 THEN $3::int ELSE channel_count END "
            "WHERE registration_id = $1 RETURNING id::text",
            {std::string(registrationId), std::string(serialNumber), std::to_string(channelCount)});
        if (!updated) {
            return Outcome::failure(updated.error());
        }
        if (updated.value().size() == 0) {
            return Outcome::failure(detail::notFound("device not found"));
        }
        if (channelCount == 0) {
            return Outcome::success();
        }
        const auto channels = session.query(
            "INSERT INTO device_channels (device_id, channel_index, name) "
            "SELECT $1::uuid, g, 'Canal ' || (g + 1) FROM generate_series(0, $2::int - 1) AS g "
            "ON CONFLICT DO NOTHING",
            {updated.value().text(0, 0), std::to_string(channelCount)});
        if (!channels) {
            return Outcome::failure(channels.error());
        }
        return Outcome::success();
    });
}

}
