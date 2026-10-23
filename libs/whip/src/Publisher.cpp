#include "whip/Publisher.hpp"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <rtc/rtc.hpp>
#include <trantor/net/EventLoopThread.h>

#include <future>
#include <mutex>
#include <random>

namespace whip {

namespace {

using domain::Error;
using domain::ErrorKind;

constexpr std::uint8_t kVideoPayloadType = 96;
constexpr std::uint8_t kAudioPayloadType = 8;
constexpr std::uint32_t kVideoClockRate = 90000;
constexpr std::uint32_t kAudioClockRate = 8000;
constexpr double kMillisecondsPerSecond = 1000.0;

struct HttpEndpoint {
    std::string origin;
    std::string path;
};

std::optional<HttpEndpoint> splitEndpoint(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) {
        return std::nullopt;
    }
    const auto pathStart = url.find('/', scheme + 3);
    if (pathStart == std::string::npos) {
        return HttpEndpoint{url, "/"};
    }
    return HttpEndpoint{url.substr(0, pathStart), url.substr(pathStart)};
}

trantor::EventLoop* httpLoop() {
    static trantor::EventLoopThread thread("whip-http");
    static const bool started = [] {
        thread.run();
        return true;
    }();
    static_cast<void>(started);
    return thread.getLoop();
}

std::uint32_t randomSsrc() {
    static std::mutex mutex;
    static std::mt19937 engine{std::random_device{}()};
    const std::scoped_lock lock(mutex);
    return std::uniform_int_distribution<std::uint32_t>(1, 0xFFFFFFFFU)(engine);
}

Error failure(const ErrorKind kind, std::string message, const bool retryable) {
    return Error{kind, std::move(message), 0, retryable};
}

}

struct Publisher::Impl {
    PublisherConfig config;
    HttpEndpoint endpoint;
    std::shared_ptr<rtc::PeerConnection> connection;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<rtc::Track> audioTrack;
    std::string sessionPath;

    std::atomic<bool> ready{false};
    std::atomic<bool> closed{false};
    std::atomic<bool> waitingForKeyFrame{true};
    std::atomic<std::uint64_t> videoFrames{0};
    std::atomic<std::uint64_t> audioFrames{0};
    std::atomic<std::uint64_t> droppedFrames{0};
    std::int64_t baseTimestampMs{-1};

    void deleteSession() const {
        if (sessionPath.empty()) {
            return;
        }
        try {
            const auto client = drogon::HttpClient::newHttpClient(endpoint.origin, httpLoop());
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setMethod(drogon::Delete);
            request->setPath(sessionPath);
            if (!config.bearerToken.empty()) {
                request->addHeader("Authorization", "Bearer " + config.bearerToken);
            }
            static_cast<void>(client->sendRequest(request, 2.0));
        } catch (const std::exception&) {
            return;
        }
    }
};

Publisher::Publisher(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Publisher::~Publisher() {
    impl_->closed = true;
    impl_->ready = false;
    impl_->deleteSession();
    if (impl_->connection) {
        impl_->connection->close();
    }
}

domain::Result<std::unique_ptr<Publisher>> Publisher::connect(PublisherConfig config) {
    using Outcome = domain::Result<std::unique_ptr<Publisher>>;
    static const bool loggerReady = [] {
        rtc::InitLogger(rtc::LogLevel::Warning);
        return true;
    }();
    static_cast<void>(loggerReady);

    const auto endpoint = splitEndpoint(config.endpoint);
    if (!endpoint) {
        return Outcome::failure(failure(ErrorKind::InvalidArgument, "endpoint must be an http(s) URL", false));
    }
    if (config.video != dahua::VideoCodec::H264 && config.video != dahua::VideoCodec::H265) {
        return Outcome::failure(failure(ErrorKind::Unsupported, "video codec is not supported by WebRTC", false));
    }

    auto impl = std::make_unique<Impl>();
    impl->config = std::move(config);
    impl->endpoint = *endpoint;
    Impl& state = *impl;

    try {
        rtc::Configuration rtcConfig;
        state.connection = std::make_shared<rtc::PeerConnection>(rtcConfig);

        const std::uint32_t videoSsrc = randomSsrc();
        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        if (state.config.video == dahua::VideoCodec::H265) {
            video.addH265Codec(kVideoPayloadType);
        } else {
            video.addH264Codec(kVideoPayloadType);
        }
        video.addSSRC(videoSsrc, "video");
        state.videoTrack = state.connection->addTrack(video);

        auto videoRtp = std::make_shared<rtc::RtpPacketizationConfig>(videoSsrc, "video", kVideoPayloadType,
                                                                    kVideoClockRate);
        std::shared_ptr<rtc::RtpPacketizer> videoPacketizer;
        if (state.config.video == dahua::VideoCodec::H265) {
            videoPacketizer =
                std::make_shared<rtc::H265RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, videoRtp);
        } else {
            videoPacketizer =
                std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, videoRtp);
        }
        videoPacketizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(videoRtp));
        videoPacketizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        videoPacketizer->addToChain(std::make_shared<rtc::PliHandler>([&state] {
            if (state.config.onKeyFrameRequest) {
                state.config.onKeyFrameRequest();
            }
        }));
        state.videoTrack->setMediaHandler(videoPacketizer);

        if (state.config.audio) {
            const std::uint32_t audioSsrc = randomSsrc();
            rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
            audio.addPCMACodec(kAudioPayloadType);
            audio.addSSRC(audioSsrc, "audio");
            state.audioTrack = state.connection->addTrack(audio);

            auto audioRtp = std::make_shared<rtc::RtpPacketizationConfig>(audioSsrc, "audio", kAudioPayloadType,
                                                                        kAudioClockRate);
            auto audioPacketizer = std::make_shared<rtc::PCMARtpPacketizer>(audioRtp);
            audioPacketizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(audioRtp));
            audioPacketizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
            state.audioTrack->setMediaHandler(audioPacketizer);
        }

        auto gathered = std::make_shared<std::promise<void>>();
        auto gatheredFuture = gathered->get_future();
        auto gatheringDone = std::make_shared<std::atomic<bool>>(false);
        state.connection->onGatheringStateChange([gathered, gatheringDone](rtc::PeerConnection::GatheringState gathering) {
            if (gathering == rtc::PeerConnection::GatheringState::Complete && !gatheringDone->exchange(true)) {
                gathered->set_value();
            }
        });
        auto opened = std::make_shared<std::promise<void>>();
        auto openedFuture = opened->get_future();
        auto openedDone = std::make_shared<std::atomic<bool>>(false);
        state.videoTrack->onOpen([opened, openedDone] {
            if (!openedDone->exchange(true)) {
                opened->set_value();
            }
        });
        state.connection->onStateChange([&state](rtc::PeerConnection::State connectionState) {
            if (connectionState == rtc::PeerConnection::State::Failed ||
                connectionState == rtc::PeerConnection::State::Closed ||
                connectionState == rtc::PeerConnection::State::Disconnected) {
                state.ready = false;
                if (!state.closed.exchange(true) && state.config.onClosed) {
                    state.config.onClosed();
                }
            }
        });

        state.connection->setLocalDescription();
        if (gatheredFuture.wait_for(state.config.iceGatheringTimeout) != std::future_status::ready) {
            return Outcome::failure(failure(ErrorKind::Timeout, "ICE gathering did not finish", true));
        }
        const auto offer = state.connection->localDescription();
        if (!offer) {
            return Outcome::failure(failure(ErrorKind::Internal, "no local description", false));
        }

        const auto client = drogon::HttpClient::newHttpClient(state.endpoint.origin, httpLoop());
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(drogon::Post);
        request->setPath(state.endpoint.path);
        request->setContentTypeString("application/sdp");
        request->setBody(std::string(*offer));
        if (!state.config.bearerToken.empty()) {
            request->addHeader("Authorization", "Bearer " + state.config.bearerToken);
        }
        const auto [outcome, response] =
            client->sendRequest(request, std::chrono::duration<double>(state.config.connectTimeout).count());
        if (outcome != drogon::ReqResult::Ok || !response) {
            return Outcome::failure(failure(ErrorKind::Unreachable, "media server did not answer", true));
        }
        if (response->statusCode() != drogon::k201Created) {
            const bool denied = response->statusCode() == drogon::k401Unauthorized ||
                                response->statusCode() == drogon::k403Forbidden;
            return Outcome::failure(failure(denied ? ErrorKind::Forbidden : ErrorKind::Protocol,
                                            "media server rejected the stream: HTTP " +
                                                std::to_string(static_cast<int>(response->statusCode())),
                                            !denied));
        }
        state.sessionPath = response->getHeader("location");
        if (const auto scheme = state.sessionPath.find("://"); scheme != std::string::npos) {
            const auto pathStart = state.sessionPath.find('/', scheme + 3);
            state.sessionPath = pathStart == std::string::npos ? std::string() : state.sessionPath.substr(pathStart);
        }

        state.connection->setRemoteDescription(
            rtc::Description(std::string(response->body()), rtc::Description::Type::Answer));

        if (openedFuture.wait_for(state.config.connectTimeout) != std::future_status::ready) {
            state.deleteSession();
            return Outcome::failure(failure(ErrorKind::Timeout, "WebRTC connection did not open", true));
        }
    } catch (const std::exception& exception) {
        state.deleteSession();
        return Outcome::failure(failure(ErrorKind::Internal, std::string("WebRTC setup failed: ") + exception.what(), true));
    }

    state.ready = true;
    return Outcome::success(std::unique_ptr<Publisher>(new Publisher(std::move(impl))));
}

void Publisher::push(const dahua::MediaPacket& packet) {
    Impl& state = *impl_;
    if (!state.ready || packet.payload.empty()) {
        return;
    }
    if (packet.kind != dahua::MediaKind::Video && packet.kind != dahua::MediaKind::Audio) {
        return;
    }
    if (state.baseTimestampMs < 0) {
        state.baseTimestampMs = packet.timestampMs;
    }
    const double seconds =
        static_cast<double>(std::max<std::int64_t>(packet.timestampMs - state.baseTimestampMs, 0)) /
        kMillisecondsPerSecond;
    const rtc::FrameInfo info{std::chrono::duration<double>(seconds)};
    const auto* bytes = reinterpret_cast<const rtc::byte*>(packet.payload.data());

    try {
        if (packet.kind == dahua::MediaKind::Video) {
            if (!state.videoTrack || !state.videoTrack->isOpen()) {
                return;
            }
            if (state.videoTrack->bufferedAmount() > state.config.maxBufferedBytes) {
                state.waitingForKeyFrame = true;
            }
            if (state.waitingForKeyFrame) {
                if (!packet.video.keyFrame) {
                    ++state.droppedFrames;
                    return;
                }
                if (state.videoTrack->bufferedAmount() > state.config.maxBufferedBytes) {
                    ++state.droppedFrames;
                    return;
                }
                state.waitingForKeyFrame = false;
            }
            state.videoTrack->sendFrame(bytes, packet.payload.size(), info);
            ++state.videoFrames;
            return;
        }
        if (state.audioTrack && state.audioTrack->isOpen() && packet.audio.codec == dahua::AudioCodec::G711A) {
            state.audioTrack->sendFrame(bytes, packet.payload.size(), info);
            ++state.audioFrames;
        }
    } catch (const std::exception&) {
        ++state.droppedFrames;
    }
}

bool Publisher::connected() const noexcept { return impl_->ready.load() && !impl_->closed.load(); }

PublisherStats Publisher::stats() const noexcept {
    return PublisherStats{impl_->videoFrames.load(), impl_->audioFrames.load(), impl_->droppedFrames.load()};
}

}
