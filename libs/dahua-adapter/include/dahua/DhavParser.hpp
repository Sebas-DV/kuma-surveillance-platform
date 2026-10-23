#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "dahua/MediaPacket.hpp"

namespace dahua {

class DhavParser {
public:
    using Handler = std::function<void(const MediaPacket&)>;

    struct Stats {
        std::uint64_t packets{0};
        std::uint64_t audioPackets{0};
        std::uint64_t videoPackets{0};
        std::uint64_t otherPackets{0};
        std::uint64_t bytesDiscarded{0};
        std::uint64_t checksumErrors{0};
        std::uint64_t trailerErrors{0};
    };

    explicit DhavParser(Handler handler);

    void feed(std::span<const std::byte> data);
    void reset();

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    void drain();
    void emit(std::span<const std::byte> frame);
    void discard(std::size_t count);
    std::int64_t unwrap(std::uint16_t timestamp);

    Handler handler_;
    std::vector<std::byte> buffer_;
    std::size_t readPosition_{0};
    Stats stats_;
    bool hasTimestamp_{false};
    std::uint16_t lastTimestamp_{0};
    std::int64_t unwrapped_{0};
    std::int64_t firstTimestamp_{0};
};

}
