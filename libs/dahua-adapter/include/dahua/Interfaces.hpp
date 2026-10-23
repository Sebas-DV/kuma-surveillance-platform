#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

#include "dahua/MediaPacket.hpp"
#include "dahua/Types.hpp"
#include "domain/Result.hpp"

namespace dahua {

using PacketHandler = std::function<void(const MediaPacket&)>;

class ILiveStream {
public:
    virtual ~ILiveStream() = default;
    virtual void stop() = 0;
    [[nodiscard]] virtual std::uint64_t packetCount() const noexcept = 0;
};

class ITalkSession {
public:
    virtual ~ITalkSession() = default;
    [[nodiscard]] virtual domain::Result<void> sendAudio(std::span<const std::byte> pcm16) = 0;
    virtual void stop() = 0;
};

class IDeviceSession {
public:
    virtual ~IDeviceSession() = default;
    [[nodiscard]] virtual const DeviceId& id() const noexcept = 0;
    [[nodiscard]] virtual const DeviceInfo& info() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t connectionToken() const noexcept = 0;
    [[nodiscard]] virtual domain::Result<std::shared_ptr<ILiveStream>> openLiveStream(
        const LiveStreamRequest& request, PacketHandler handler) = 0;
    [[nodiscard]] virtual domain::Result<std::shared_ptr<ITalkSession>> openTalk(const TalkRequest& request) = 0;
};

class ISessionFactory {
public:
    virtual ~ISessionFactory() = default;
    [[nodiscard]] virtual domain::Result<std::unique_ptr<IDeviceSession>> connect(
        const RegistrationEvent& event, const Credentials& credentials) = 0;
};

class ICredentialProvider {
public:
    virtual ~ICredentialProvider() = default;
    [[nodiscard]] virtual std::optional<Credentials> find(const DeviceId& id) const = 0;
};

}
