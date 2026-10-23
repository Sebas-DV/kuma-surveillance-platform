#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "dahua/DhavParser.hpp"
#include "dahua/G711.hpp"
#include "domain/Environment.hpp"

namespace {

using namespace dahua;

std::vector<std::byte> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](const char value) { return static_cast<std::byte>(value); });
    return bytes;
}

class DhavFixtureTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto dav = domain::readEnvironment("SURVEILLANCE_DAV_FIXTURE");
        const auto pcm = domain::readEnvironment("SURVEILLANCE_PCM_REFERENCE");
        if (!dav || !pcm) {
            GTEST_SKIP() << "set SURVEILLANCE_DAV_FIXTURE and SURVEILLANCE_PCM_REFERENCE to run the device capture test";
        }
        stream_ = readFile(*dav);
        reference_ = readFile(*pcm);
        ASSERT_FALSE(stream_.empty());
        ASSERT_FALSE(reference_.empty());
    }

    std::vector<std::byte> stream_;
    std::vector<std::byte> reference_;
};

TEST_F(DhavFixtureTest, ParsesARealDeviceCaptureExactly) {
    std::vector<std::int16_t> decoded;
    std::uint64_t videoPackets = 0;
    std::uint64_t keyFrames = 0;
    std::uint32_t previousAudioSequence = 0;
    bool audioSequenceContinuous = true;
    bool keyFramesDescribed = true;
    bool firstAudio = true;

    DhavParser parser([&](const MediaPacket& packet) {
        if (packet.kind == MediaKind::Audio && packet.audio.codec == AudioCodec::G711A) {
            const auto samples = decodeAlaw(packet.payload);
            decoded.insert(decoded.end(), samples.begin(), samples.end());
            if (!firstAudio && packet.sequence != previousAudioSequence + 1) {
                audioSequenceContinuous = false;
            }
            previousAudioSequence = packet.sequence;
            firstAudio = false;
        } else if (packet.kind == MediaKind::Video) {
            ++videoPackets;
            if (packet.video.keyFrame) {
                ++keyFrames;
                keyFramesDescribed = keyFramesDescribed && packet.video.width > 0 && packet.video.height > 0 &&
                                     packet.video.framesPerSecond > 0 && packet.video.codec != VideoCodec::Unknown;
            }
        }
    });

    std::mt19937 random(5);
    std::uniform_int_distribution<std::size_t> chunkSize(1, 9000);
    for (std::size_t offset = 0; offset < stream_.size();) {
        const std::size_t count = std::min(chunkSize(random), stream_.size() - offset);
        parser.feed(std::span<const std::byte>(stream_).subspan(offset, count));
        offset += count;
    }

    EXPECT_EQ(parser.stats().checksumErrors, 0U);
    EXPECT_EQ(parser.stats().trailerErrors, 0U);
    EXPECT_EQ(parser.stats().bytesDiscarded, 0U);
    EXPECT_GT(videoPackets, 0U);
    EXPECT_GT(keyFrames, 0U);
    EXPECT_TRUE(keyFramesDescribed);
    EXPECT_TRUE(audioSequenceContinuous);

    ASSERT_EQ(decoded.size() * sizeof(std::int16_t), reference_.size());
    EXPECT_EQ(std::memcmp(decoded.data(), reference_.data(), reference_.size()), 0);
}

}
