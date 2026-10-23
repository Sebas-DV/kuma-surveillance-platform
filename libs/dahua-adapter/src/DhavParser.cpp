#include "dahua/DhavParser.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace dahua {

namespace {

constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kTrailerSize = 8;
constexpr std::size_t kChecksumOffset = 23;
constexpr std::size_t kExtensionLengthOffset = 22;
constexpr std::size_t kMaxFrameSize = std::size_t{8} * 1024 * 1024;
constexpr std::size_t kCompactThreshold = std::size_t{1} << 16U;

constexpr std::array<std::byte, 4> kSync{std::byte{'D'}, std::byte{'H'}, std::byte{'A'}, std::byte{'V'}};
constexpr std::array<std::byte, 4> kTrailerMark{std::byte{'d'}, std::byte{'h'}, std::byte{'a'}, std::byte{'v'}};

constexpr std::uint8_t kAudioFrame = 0xF0;
constexpr std::uint8_t kKeyFrame = 0xFD;
constexpr std::uint8_t kPredictedFrame = 0xFC;

constexpr std::uint8_t kVideoSizeTag = 0x80;
constexpr std::uint8_t kVideoCodecTag = 0x81;
constexpr std::uint8_t kAudioTag = 0x83;

constexpr std::uint8_t kAudioIdG711A = 0x0E;
constexpr std::uint8_t kAudioIdPcm16 = 0x10;
constexpr std::uint8_t kVideoIdH265 = 0x0C;
constexpr std::uint8_t kSampleRateCode8k = 2;

std::uint8_t byteAt(const std::span<const std::byte> data, const std::size_t offset) {
    return std::to_integer<std::uint8_t>(data[offset]);
}

std::uint16_t readLe16(const std::span<const std::byte> data, const std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(byteAt(data, offset)) |
                                      (static_cast<std::uint32_t>(byteAt(data, offset + 1)) << 8U));
}

std::uint32_t readLe32(const std::span<const std::byte> data, const std::size_t offset) {
    return static_cast<std::uint32_t>(byteAt(data, offset)) |
           (static_cast<std::uint32_t>(byteAt(data, offset + 1)) << 8U) |
           (static_cast<std::uint32_t>(byteAt(data, offset + 2)) << 16U) |
           (static_cast<std::uint32_t>(byteAt(data, offset + 3)) << 24U);
}

std::size_t findStartCode(const std::span<const std::byte> payload) {
    const std::size_t limit = std::min<std::size_t>(payload.size(), 64);
    for (std::size_t index = 0; index + 3 < limit; ++index) {
        if (byteAt(payload, index) == 0 && byteAt(payload, index + 1) == 0) {
            if (byteAt(payload, index + 2) == 1) {
                return index + 3;
            }
            if (byteAt(payload, index + 2) == 0 && byteAt(payload, index + 3) == 1) {
                return index + 4;
            }
        }
    }
    return payload.size();
}

VideoCodec sniffVideoCodec(const std::span<const std::byte> payload) {
    const std::size_t position = findStartCode(payload);
    if (position >= payload.size()) {
        return VideoCodec::Unknown;
    }
    const auto header = static_cast<std::uint32_t>(byteAt(payload, position));
    const std::uint32_t h264Type = header & 0x1FU;
    if ((header & 0x80U) == 0 && (header & 0x60U) != 0 && (h264Type == 7 || h264Type == 8 || h264Type == 5)) {
        return VideoCodec::H264;
    }
    const std::uint32_t h265Type = (header >> 1U) & 0x3FU;
    if ((header & 0x81U) == 0 && h265Type >= 32 && h265Type <= 34) {
        return VideoCodec::H265;
    }
    return VideoCodec::Unknown;
}

AudioCodec audioCodecFromId(const std::uint8_t id) {
    switch (id) {
        case kAudioIdG711A: return AudioCodec::G711A;
        case kAudioIdPcm16: return AudioCodec::Pcm16;
        default: return AudioCodec::Unknown;
    }
}

}

DhavParser::DhavParser(Handler handler) : handler_(std::move(handler)) {}

void DhavParser::reset() {
    buffer_.clear();
    readPosition_ = 0;
    hasTimestamp_ = false;
    lastTimestamp_ = 0;
    unwrapped_ = 0;
    firstTimestamp_ = 0;
}

void DhavParser::feed(const std::span<const std::byte> data) {
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    drain();
    if (readPosition_ == buffer_.size()) {
        buffer_.clear();
        readPosition_ = 0;
    } else if (readPosition_ > kCompactThreshold) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(readPosition_));
        readPosition_ = 0;
    }
}

void DhavParser::discard(const std::size_t count) {
    readPosition_ += count;
    stats_.bytesDiscarded += count;
}

std::int64_t DhavParser::unwrap(const std::uint16_t timestamp) {
    if (!hasTimestamp_) {
        hasTimestamp_ = true;
        firstTimestamp_ = timestamp;
        unwrapped_ = timestamp;
    } else {
        const auto delta = static_cast<std::int16_t>(static_cast<std::uint16_t>(timestamp - lastTimestamp_));
        unwrapped_ += delta;
    }
    lastTimestamp_ = timestamp;
    return unwrapped_ - firstTimestamp_;
}

void DhavParser::drain() {
    while (true) {
        const std::span<const std::byte> pending(buffer_.data() + readPosition_, buffer_.size() - readPosition_);
        const auto sync = std::ranges::search(pending, kSync).begin();

        if (sync == pending.end()) {
            const std::size_t keep = std::min<std::size_t>(pending.size(), kSync.size() - 1);
            discard(pending.size() - keep);
            return;
        }

        if (sync != pending.begin()) {
            discard(static_cast<std::size_t>(sync - pending.begin()));
            continue;
        }

        if (pending.size() < kHeaderSize) {
            return;
        }

        unsigned checksum = 0;
        for (std::size_t index = 0; index < kChecksumOffset; ++index) {
            checksum += byteAt(pending, index);
        }
        if ((checksum & 0xFFU) != byteAt(pending, kChecksumOffset)) {
            ++stats_.checksumErrors;
            discard(kSync.size());
            continue;
        }

        const std::size_t total = readLe32(pending, 12);
        const std::size_t extensionLength = byteAt(pending, kExtensionLengthOffset);
        if (total > kMaxFrameSize || total < kHeaderSize + extensionLength + kTrailerSize) {
            ++stats_.checksumErrors;
            discard(kSync.size());
            continue;
        }

        if (pending.size() < total) {
            return;
        }

        const auto frame = pending.first(total);
        const bool trailerValid = std::equal(kTrailerMark.begin(), kTrailerMark.end(), frame.end() - kTrailerSize) &&
                                  readLe32(frame, total - 4) == total;
        if (!trailerValid) {
            ++stats_.trailerErrors;
            discard(kSync.size());
            continue;
        }

        emit(frame);
        readPosition_ += total;
    }
}

void DhavParser::emit(const std::span<const std::byte> frame) {
    const std::size_t extensionLength = byteAt(frame, kExtensionLengthOffset);
    const std::span<const std::byte> extension = frame.subspan(kHeaderSize, extensionLength);

    MediaPacket packet;
    packet.frameType = byteAt(frame, 4);
    packet.sequence = readLe32(frame, 8);
    packet.packedDateTime = readLe32(frame, 16);
    packet.timestampMs = unwrap(readLe16(frame, 20));
    packet.payload = frame.subspan(kHeaderSize + extensionLength, frame.size() - kHeaderSize - extensionLength - kTrailerSize);

    if (packet.frameType == kAudioFrame) {
        packet.kind = MediaKind::Audio;
        for (std::size_t index = 0; index + 3 < extension.size(); index += 4) {
            if (byteAt(extension, index) == kAudioTag) {
                packet.audio.channels = byteAt(extension, index + 1);
                packet.audio.codecId = byteAt(extension, index + 2);
                packet.audio.codec = audioCodecFromId(packet.audio.codecId);
                packet.audio.sampleRate = byteAt(extension, index + 3) == kSampleRateCode8k ? 8000U : 0U;
                break;
            }
        }
        ++stats_.audioPackets;
    } else if (packet.frameType == kKeyFrame || packet.frameType == kPredictedFrame) {
        packet.kind = MediaKind::Video;
        packet.video.keyFrame = packet.frameType == kKeyFrame;
        std::uint8_t codecId = 0;
        for (std::size_t index = 0; index + 3 < extension.size(); index += 4) {
            const std::uint8_t tag = byteAt(extension, index);
            if (tag == kVideoSizeTag) {
                packet.video.width = static_cast<std::uint16_t>(byteAt(extension, index + 2) * 8U);
                packet.video.height = static_cast<std::uint16_t>(byteAt(extension, index + 3) * 8U);
            } else if (tag == kVideoCodecTag) {
                codecId = byteAt(extension, index + 2);
                packet.video.framesPerSecond = byteAt(extension, index + 3);
            }
        }
        packet.video.codec = codecId == kVideoIdH265 ? VideoCodec::H265 : VideoCodec::Unknown;
        if (packet.video.keyFrame) {
            const VideoCodec sniffed = sniffVideoCodec(packet.payload);
            if (sniffed != VideoCodec::Unknown) {
                packet.video.codec = sniffed;
            }
        }
        ++stats_.videoPackets;
    } else {
        packet.kind = MediaKind::Other;
        ++stats_.otherPackets;
    }

    ++stats_.packets;
    if (handler_) {
        handler_(packet);
    }
}

}
