#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dahua/DeviceSessionManager.hpp"
#include "telemetry/Logger.hpp"
#include "whip/Publisher.hpp"

namespace gateway {

class ChannelPublisher {
public:
    ChannelPublisher(std::shared_ptr<dahua::IDeviceSession> session, std::string whipUrl, int channel,
                     const telemetry::Logger& logger);
    ChannelPublisher(const ChannelPublisher&) = delete;
    ChannelPublisher& operator=(const ChannelPublisher&) = delete;
    ChannelPublisher(ChannelPublisher&&) = delete;
    ChannelPublisher& operator=(ChannelPublisher&&) = delete;
    ~ChannelPublisher();

    [[nodiscard]] domain::Result<void> start();
    void stop();

    [[nodiscard]] std::uint64_t connectionToken() const noexcept { return session_->connectionToken(); }

private:
    void onPacket(const dahua::MediaPacket& packet);
    void supervise() noexcept;
    void superviseOnce(std::chrono::milliseconds& backoff);
    void requestKeyFrame() const;

    std::shared_ptr<dahua::IDeviceSession> session_;
    std::string whipUrl_;
    int channel_;
    const telemetry::Logger& logger_;
    std::shared_ptr<dahua::ILiveStream> stream_;
    std::atomic<std::shared_ptr<whip::Publisher>> publisher_;
    std::atomic<dahua::VideoCodec> codec_{dahua::VideoCodec::Unknown};
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    std::thread worker_;
};

struct StreamPublisherConfig {
    std::string mediaServerUrl;
    std::vector<int> channels;
    std::function<std::vector<int>(const std::string& registrationId)> enabledChannels;
};

class StreamPublisher {
public:
    StreamPublisher(std::shared_ptr<dahua::DeviceSessionManager> manager, StreamPublisherConfig config,
                    const telemetry::Logger& logger = telemetry::Logger::global());

    void onSnapshot(const dahua::DeviceSnapshot& snapshot);
    void stopAll();

private:
    struct Group {
        std::uint64_t connectionToken{0};
        std::vector<std::unique_ptr<ChannelPublisher>> channels;
    };

    [[nodiscard]] std::vector<int> selectChannels(const dahua::DeviceSnapshot& snapshot) const;

    std::shared_ptr<dahua::DeviceSessionManager> manager_;
    StreamPublisherConfig config_;
    const telemetry::Logger& logger_;
    std::mutex mutex_;
    std::map<std::string, Group> groups_;
};

}
