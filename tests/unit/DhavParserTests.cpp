#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <vector>

#include "DhavFrameBuilder.hpp"
#include "dahua/DhavParser.hpp"

namespace {

using namespace dahua;
using namespace testing;

struct Collected {
    MediaKind kind;
    std::uint8_t frameType;
    std::uint32_t sequence;
    std::int64_t timestampMs;
    VideoInfo video;
    AudioInfo audio;
    std::vector<std::byte> payload;
};

class Collector {
public:
    DhavParser::Handler handler() {
        return [this](const MediaPacket& packet) {
            packets.push_back(Collected{packet.kind, packet.frameType, packet.sequence, packet.timestampMs, packet.video,
                                        packet.audio,
                                        std::vector<std::byte>(packet.payload.begin(), packet.payload.end())});
        };
    }

    std::vector<Collected> packets;
};

FrameSpec audioFrame(const std::uint32_t sequence, const std::uint16_t timestamp, const std::size_t size = 640) {
    FrameSpec spec;
    spec.type = 0xF0;
    spec.sequence = sequence;
    spec.timestamp = timestamp;
    spec.extension = audioExtension(0x0E);
    spec.payload.assign(size, 0xD5);
    return spec;
}

FrameSpec videoFrame(const bool key, const std::uint32_t sequence, const std::uint16_t timestamp,
                     std::vector<std::uint8_t> payload, const std::uint8_t codecId = 0x0C) {
    FrameSpec spec;
    spec.type = key ? 0xFD : 0xFC;
    spec.sequence = sequence;
    spec.timestamp = timestamp;
    spec.extension = videoExtension(120, 135, codecId, 25);
    spec.payload = std::move(payload);
    return spec;
}

TEST(DhavParserTest, ParsesAudioPacket) {
    Collector collector;
    DhavParser parser(collector.handler());
    const auto frame = buildFrame(audioFrame(7, 1000));

    parser.feed(frame);

    ASSERT_EQ(collector.packets.size(), 1U);
    const auto& packet = collector.packets.front();
    EXPECT_EQ(packet.kind, MediaKind::Audio);
    EXPECT_EQ(packet.sequence, 7U);
    EXPECT_EQ(packet.payload.size(), 640U);
    EXPECT_EQ(packet.audio.codec, AudioCodec::G711A);
    EXPECT_EQ(packet.audio.codecId, 0x0E);
    EXPECT_EQ(packet.audio.channels, 1);
    EXPECT_EQ(packet.audio.sampleRate, 8000U);
    EXPECT_EQ(parser.stats().audioPackets, 1U);
    EXPECT_EQ(parser.stats().bytesDiscarded, 0U);
}

TEST(DhavParserTest, RecognisesPcmAndUnknownAudioCodecs) {
    Collector collector;
    DhavParser parser(collector.handler());

    auto pcm = audioFrame(1, 0);
    pcm.extension = audioExtension(0x10);
    auto unknown = audioFrame(2, 80);
    unknown.extension = audioExtension(0x7A, 9);
    parser.feed(buildFrame(pcm));
    parser.feed(buildFrame(unknown));

    ASSERT_EQ(collector.packets.size(), 2U);
    EXPECT_EQ(collector.packets[0].audio.codec, AudioCodec::Pcm16);
    EXPECT_EQ(collector.packets[1].audio.codec, AudioCodec::Unknown);
    EXPECT_EQ(collector.packets[1].audio.codecId, 0x7A);
    EXPECT_EQ(collector.packets[1].audio.sampleRate, 0U);
}

TEST(DhavParserTest, ParsesH265KeyFrameWithGeometry) {
    Collector collector;
    DhavParser parser(collector.handler());
    const std::vector<std::uint8_t> payload{0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01};

    parser.feed(buildFrame(videoFrame(true, 1, 0, payload)));

    ASSERT_EQ(collector.packets.size(), 1U);
    const auto& video = collector.packets.front().video;
    EXPECT_EQ(collector.packets.front().kind, MediaKind::Video);
    EXPECT_TRUE(video.keyFrame);
    EXPECT_EQ(video.codec, VideoCodec::H265);
    EXPECT_EQ(video.width, 960);
    EXPECT_EQ(video.height, 1080);
    EXPECT_EQ(video.framesPerSecond, 25);
}

TEST(DhavParserTest, SniffsH264FromTheFirstNalUnit) {
    Collector collector;
    DhavParser parser(collector.handler());
    const std::vector<std::uint8_t> payload{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1F};

    parser.feed(buildFrame(videoFrame(true, 1, 0, payload, 0x08)));

    ASSERT_EQ(collector.packets.size(), 1U);
    EXPECT_EQ(collector.packets.front().video.codec, VideoCodec::H264);
}

TEST(DhavParserTest, SniffingOverridesAWrongCodecId) {
    Collector collector;
    DhavParser parser(collector.handler());
    const std::vector<std::uint8_t> payload{0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x28};

    parser.feed(buildFrame(videoFrame(true, 1, 0, payload, 0x0C)));

    ASSERT_EQ(collector.packets.size(), 1U);
    EXPECT_EQ(collector.packets.front().video.codec, VideoCodec::H264);
}

TEST(DhavParserTest, PredictedFramesAreNotKeyFrames) {
    Collector collector;
    DhavParser parser(collector.handler());

    parser.feed(buildFrame(videoFrame(false, 2, 40, {0x00, 0x00, 0x00, 0x01, 0x02, 0x01})));

    ASSERT_EQ(collector.packets.size(), 1U);
    EXPECT_FALSE(collector.packets.front().video.keyFrame);
    EXPECT_EQ(collector.packets.front().frameType, 0xFC);
    EXPECT_EQ(collector.packets.front().video.codec, VideoCodec::H265);
}

TEST(DhavParserTest, UnknownFrameTypesAreReportedAsOther) {
    Collector collector;
    DhavParser parser(collector.handler());
    FrameSpec spec;
    spec.type = 0xF1;
    spec.payload = {1, 2, 3};

    parser.feed(buildFrame(spec));

    ASSERT_EQ(collector.packets.size(), 1U);
    EXPECT_EQ(collector.packets.front().kind, MediaKind::Other);
    EXPECT_EQ(parser.stats().otherPackets, 1U);
}

TEST(DhavParserTest, ReassemblesFramesFedOneByteAtATime) {
    Collector collector;
    DhavParser parser(collector.handler());
    const std::array<std::vector<std::byte>, 3> frames{buildFrame(audioFrame(1, 0)), buildFrame(audioFrame(2, 80)),
                                                       buildFrame(audioFrame(3, 160))};
    const auto stream = concatenate(frames);

    for (const std::byte value : stream) {
        parser.feed(std::span<const std::byte>(&value, 1));
    }

    ASSERT_EQ(collector.packets.size(), 3U);
    EXPECT_EQ(collector.packets[2].sequence, 3U);
    EXPECT_EQ(parser.stats().bytesDiscarded, 0U);
}

TEST(DhavParserTest, HandlesSeveralFramesInOneChunk) {
    Collector collector;
    DhavParser parser(collector.handler());
    const std::array<std::vector<std::byte>, 4> frames{buildFrame(audioFrame(1, 0)), buildFrame(audioFrame(2, 80)),
                                                       buildFrame(audioFrame(3, 160)), buildFrame(audioFrame(4, 240))};

    parser.feed(concatenate(frames));

    EXPECT_EQ(collector.packets.size(), 4U);
}

TEST(DhavParserTest, ResynchronisesAfterGarbageBetweenFrames) {
    Collector collector;
    DhavParser parser(collector.handler());
    std::vector<std::byte> stream = buildFrame(audioFrame(1, 0));
    const std::vector<std::byte> garbage(37, std::byte{0x5A});
    stream.insert(stream.end(), garbage.begin(), garbage.end());
    const auto second = buildFrame(audioFrame(2, 80));
    stream.insert(stream.end(), second.begin(), second.end());

    parser.feed(stream);

    ASSERT_EQ(collector.packets.size(), 2U);
    EXPECT_EQ(collector.packets[1].sequence, 2U);
    EXPECT_EQ(parser.stats().bytesDiscarded, 37U);
}

TEST(DhavParserTest, SkipsFramesWithBadChecksumAndRecovers) {
    Collector collector;
    DhavParser parser(collector.handler());
    auto corrupted = buildFrame(audioFrame(1, 0));
    corrupted[23] = static_cast<std::byte>(std::to_integer<unsigned>(corrupted[23]) ^ 0xFFU);
    const auto good = buildFrame(audioFrame(2, 80));

    parser.feed(corrupted);
    parser.feed(good);

    ASSERT_EQ(collector.packets.size(), 1U);
    EXPECT_EQ(collector.packets.front().sequence, 2U);
    EXPECT_GE(parser.stats().checksumErrors, 1U);
}

TEST(DhavParserTest, RejectsFramesWithBadTrailer) {
    Collector collector;
    DhavParser parser(collector.handler());
    auto corrupted = buildFrame(audioFrame(1, 0));
    corrupted[corrupted.size() - 8] = std::byte{'x'};

    parser.feed(corrupted);
    parser.feed(buildFrame(audioFrame(2, 80)));

    ASSERT_EQ(collector.packets.size(), 1U);
    EXPECT_EQ(collector.packets.front().sequence, 2U);
    EXPECT_GE(parser.stats().trailerErrors, 1U);
}

TEST(DhavParserTest, TimestampsAreRelativeToTheFirstPacket) {
    Collector collector;
    DhavParser parser(collector.handler());

    parser.feed(buildFrame(audioFrame(1, 45792)));
    parser.feed(buildFrame(audioFrame(2, 45872)));

    ASSERT_EQ(collector.packets.size(), 2U);
    EXPECT_EQ(collector.packets[0].timestampMs, 0);
    EXPECT_EQ(collector.packets[1].timestampMs, 80);
}

TEST(DhavParserTest, UnwrapsTheSixteenBitClock) {
    Collector collector;
    DhavParser parser(collector.handler());

    parser.feed(buildFrame(audioFrame(1, 65500)));
    parser.feed(buildFrame(audioFrame(2, 44)));
    parser.feed(buildFrame(audioFrame(3, 124)));

    ASSERT_EQ(collector.packets.size(), 3U);
    EXPECT_EQ(collector.packets[1].timestampMs, 80);
    EXPECT_EQ(collector.packets[2].timestampMs, 160);
}

TEST(DhavParserTest, ToleratesSmallOutOfOrderTimestamps) {
    Collector collector;
    DhavParser parser(collector.handler());

    parser.feed(buildFrame(audioFrame(1, 1000)));
    parser.feed(buildFrame(videoFrame(false, 2, 1040, {0, 0, 0, 1, 2, 1})));
    parser.feed(buildFrame(audioFrame(3, 1020)));

    ASSERT_EQ(collector.packets.size(), 3U);
    EXPECT_EQ(collector.packets[1].timestampMs, 40);
    EXPECT_EQ(collector.packets[2].timestampMs, 20);
}

TEST(DhavParserTest, ResetDropsBufferedPartialData) {
    Collector collector;
    DhavParser parser(collector.handler());
    const auto frame = buildFrame(audioFrame(1, 0));

    parser.feed(std::span<const std::byte>(frame).first(frame.size() / 2));
    parser.reset();
    parser.feed(buildFrame(audioFrame(9, 0)));

    ASSERT_EQ(collector.packets.size(), 1U);
    EXPECT_EQ(collector.packets.front().sequence, 9U);
}

TEST(DhavParserTest, DecodesPackedDeviceDateTime) {
    const auto decoded = decodeDeviceDateTime(0x6AAEA371U);
    EXPECT_EQ(decoded.year, 2026);
    EXPECT_EQ(decoded.month, 10);
    EXPECT_EQ(decoded.day, 23);
    EXPECT_EQ(decoded.hour, 10);
    EXPECT_EQ(decoded.minute, 13);
    EXPECT_EQ(decoded.second, 49);
}

}
