#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "dahua/MediaPacket.hpp"
#include "domain/Result.hpp"

namespace whip {

struct PublisherConfig {
    std::string endpoint;
    std::string bearerToken;
    dahua::VideoCodec video{dahua::VideoCodec::H264};
    bool audio{true};
    std::chrono::milliseconds connectTimeout{5000};
    std::chrono::milliseconds iceGatheringTimeout{2000};
    std::size_t maxBufferedBytes{512 * 1024};
    std::function<void()> onKeyFrameRequest;
    std::function<void()> onClosed;
};

struct PublisherStats {
    std::uint64_t videoFrames{0};
    std::uint64_t audioFrames{0};
    std::uint64_t droppedFrames{0};
};

class Publisher {
public:
    [[nodiscard]] static domain::Result<std::unique_ptr<Publisher>> connect(PublisherConfig config);

    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    Publisher(Publisher&&) = delete;
    Publisher& operator=(Publisher&&) = delete;
    ~Publisher();

    void push(const dahua::MediaPacket& packet);

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] PublisherStats stats() const noexcept;

private:
    struct Impl;
    explicit Publisher(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}
