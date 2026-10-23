#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace dahua {

enum class MediaKind { Video, Audio, Other };

enum class VideoCodec { Unknown, H264, H265 };

enum class AudioCodec { Unknown, G711A, G711U, Pcm16 };

struct VideoInfo {
    VideoCodec codec{VideoCodec::Unknown};
    std::uint16_t width{0};
    std::uint16_t height{0};
    std::uint8_t framesPerSecond{0};
    bool keyFrame{false};
};

struct AudioInfo {
    AudioCodec codec{AudioCodec::Unknown};
    std::uint8_t codecId{0};
    std::uint8_t channels{0};
    std::uint32_t sampleRate{0};
};

struct DeviceDateTime {
    int year{0};
    int month{0};
    int day{0};
    int hour{0};
    int minute{0};
    int second{0};

    [[nodiscard]] bool operator==(const DeviceDateTime&) const = default;
};

[[nodiscard]] DeviceDateTime decodeDeviceDateTime(std::uint32_t packed) noexcept;

struct MediaPacket {
    MediaKind kind{MediaKind::Other};
    std::uint8_t frameType{0};
    std::uint32_t sequence{0};
    std::int64_t timestampMs{0};
    std::uint32_t packedDateTime{0};
    VideoInfo video;
    AudioInfo audio;
    std::span<const std::byte> payload;
};

}
