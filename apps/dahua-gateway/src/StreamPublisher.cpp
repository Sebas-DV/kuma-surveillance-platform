#include "StreamPublisher.hpp"

#include <algorithm>

namespace gateway {

namespace {

constexpr std::chrono::milliseconds kInitialBackoff{500};
constexpr std::chrono::milliseconds kMaxBackoff{10000};
constexpr std::chrono::milliseconds kPollInterval{250};

std::string joinUrl(const std::string& base, const std::string& registrationId, const int channel) {
    std::string url = base;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url + "/" + registrationId + "/" + std::to_string(channel) + "/whip";
}

}

ChannelPublisher::ChannelPublisher(std::shared_ptr<dahua::IDeviceSession> session, std::string whipUrl,
                                   const int channel, const telemetry::Logger& logger)
    : session_(std::move(session)), whipUrl_(std::move(whipUrl)), channel_(channel), logger_(logger) {}

ChannelPublisher::~ChannelPublisher() { stop(); }

domain::Result<void> ChannelPublisher::start() {
    auto opened = session_->openLiveStream(dahua::LiveStreamRequest{channel_, dahua::StreamProfile::Main},
                                           [this](const dahua::MediaPacket& packet) { onPacket(packet); });
    if (!opened) {
        return domain::Result<void>::failure(opened.error());
    }
    stream_ = std::move(opened).value();
    worker_ = std::thread(&ChannelPublisher::supervise, this);
    return domain::Result<void>::success();
}

void ChannelPublisher::stop() {
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    publisher_.store(nullptr);
    if (stream_) {
        stream_->stop();
        stream_.reset();
    }
}

void ChannelPublisher::onPacket(const dahua::MediaPacket& packet) {
    if (packet.kind == dahua::MediaKind::Video && packet.video.codec != dahua::VideoCodec::Unknown) {
        codec_.store(packet.video.codec);
    }
    if (const auto publisher = publisher_.load()) {
        publisher->push(packet);
    }
}

void ChannelPublisher::requestKeyFrame() const {
    if (stream_) {
        if (const auto requested = stream_->requestKeyFrame(); !requested) {
            logger_.debug("publish.keyframe_failed",
                          {{"channel", std::to_string(channel_)}, {"reason", requested.error().message}});
        }
    }
}

void ChannelPublisher::superviseOnce(std::chrono::milliseconds& backoff) {
    const auto publisher = publisher_.load();
    if (publisher && publisher->connected()) {
        backoff = kInitialBackoff;
        return;
    }
    publisher_.store(nullptr);

    const dahua::VideoCodec codec = codec_.load();
    if (codec == dahua::VideoCodec::Unknown) {
        return;
    }
    whip::PublisherConfig config;
    config.endpoint = whipUrl_;
    config.video = codec;
    config.onKeyFrameRequest = [this] { requestKeyFrame(); };
    auto connected = whip::Publisher::connect(std::move(config));
    if (!connected) {
        logger_.warn("publish.connect_failed", {{"url", whipUrl_},
                                                {"channel", std::to_string(channel_)},
                                                {"reason", connected.error().message},
                                                {"retry_ms", std::to_string(backoff.count())}});
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, backoff, [this] { return stopping_; });
        backoff = std::min(backoff * 2, kMaxBackoff);
        return;
    }
    publisher_.store(std::shared_ptr<whip::Publisher>(std::move(connected).value()));
    requestKeyFrame();
    logger_.info("publish.started", {{"url", whipUrl_}, {"channel", std::to_string(channel_)}});
}

void ChannelPublisher::supervise() noexcept {
    try {
        std::chrono::milliseconds backoff = kInitialBackoff;
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            lock.unlock();
            superviseOnce(backoff);
            lock.lock();
            wake_.wait_for(lock, kPollInterval, [this] { return stopping_; });
        }
    } catch (const std::exception&) {
        logger_.error("publish.supervisor_failed", {{"channel", std::to_string(channel_)}});
    }
}

StreamPublisher::StreamPublisher(std::shared_ptr<dahua::DeviceSessionManager> manager, StreamPublisherConfig config,
                                 const telemetry::Logger& logger)
    : manager_(std::move(manager)), config_(std::move(config)), logger_(logger) {}

std::vector<int> StreamPublisher::selectChannels(const dahua::DeviceSnapshot& snapshot) const {
    std::vector<int> selected;
    if (config_.enabledChannels) {
        selected = config_.enabledChannels(snapshot.id.value);
    } else if (snapshot.info.has_value()) {
        for (int channel = 0; channel < snapshot.info->channelCount; ++channel) {
            selected.push_back(channel);
        }
    }
    if (!config_.channels.empty()) {
        std::erase_if(selected, [this](const int channel) {
            return std::ranges::find(config_.channels, channel) == config_.channels.end();
        });
    }
    if (snapshot.info.has_value()) {
        std::erase_if(selected, [&snapshot](const int channel) { return channel >= snapshot.info->channelCount; });
    }
    return selected;
}

void StreamPublisher::onSnapshot(const dahua::DeviceSnapshot& snapshot) {
    if (snapshot.state == dahua::SessionState::Authenticating) {
        return;
    }
    if (snapshot.state != dahua::SessionState::Online || !snapshot.info.has_value()) {
        std::unique_ptr<Group> retired;
        {
            const std::scoped_lock lock(mutex_);
            if (const auto found = groups_.find(snapshot.id.value); found != groups_.end()) {
                retired = std::make_unique<Group>(std::move(found->second));
                groups_.erase(found);
            }
        }
        return;
    }

    const auto session = manager_->session(snapshot.id);
    if (!session) {
        return;
    }
    {
        const std::scoped_lock lock(mutex_);
        const auto found = groups_.find(snapshot.id.value);
        if (found != groups_.end() && found->second.connectionToken == session->connectionToken()) {
            return;
        }
    }

    Group group;
    group.connectionToken = session->connectionToken();
    for (const int channel : selectChannels(snapshot)) {
        auto publisher = std::make_unique<ChannelPublisher>(
            session, joinUrl(config_.mediaServerUrl, snapshot.id.value, channel), channel, logger_);
        if (const auto started = publisher->start(); !started) {
            logger_.warn("publish.open_failed", {{"device_id", snapshot.id.value},
                                                 {"channel", std::to_string(channel)},
                                                 {"reason", started.error().message}});
            continue;
        }
        group.channels.push_back(std::move(publisher));
    }
    std::unique_ptr<Group> replaced;
    {
        const std::scoped_lock lock(mutex_);
        if (const auto found = groups_.find(snapshot.id.value); found != groups_.end()) {
            replaced = std::make_unique<Group>(std::move(found->second));
        }
        groups_[snapshot.id.value] = std::move(group);
    }
}

void StreamPublisher::stopAll() {
    std::map<std::string, Group> retired;
    {
        const std::scoped_lock lock(mutex_);
        retired.swap(groups_);
    }
}

}
