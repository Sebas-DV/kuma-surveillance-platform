#include <gtest/gtest.h>

#include <array>
#include <cstdlib>

#include "dahua/G711.hpp"

namespace {

using namespace dahua;

TEST(G711Test, AlawKnownValues) {
    EXPECT_EQ(decodeAlawSample(0xD5), 8);
    EXPECT_EQ(decodeAlawSample(0x55), -8);
    EXPECT_EQ(decodeAlawSample(0xAA), 32256);
    EXPECT_EQ(decodeAlawSample(0x2A), -32256);
}

TEST(G711Test, UlawKnownValues) {
    EXPECT_EQ(decodeUlawSample(0xFF), 0);
    EXPECT_EQ(decodeUlawSample(0x7F), 0);
    EXPECT_EQ(decodeUlawSample(0x80), 32124);
    EXPECT_EQ(decodeUlawSample(0x00), -32124);
}

TEST(G711Test, AlawDecodeIsSymmetric) {
    for (unsigned code = 0; code < 256; ++code) {
        const auto value = static_cast<std::uint8_t>(code);
        const auto mirrored = static_cast<std::uint8_t>(code ^ 0x80U);
        EXPECT_EQ(decodeAlawSample(value), -decodeAlawSample(mirrored));
    }
}

TEST(G711Test, AlawEncodeDecodeRoundTripStaysWithinQuantizationError) {
    for (int sample = -32768; sample <= 32767; sample += 7) {
        const auto encoded = encodeAlawSample(static_cast<std::int16_t>(sample));
        const int decoded = decodeAlawSample(encoded);
        const int tolerance = std::max(16, std::abs(sample) / 16);
        EXPECT_LE(std::abs(decoded - sample), tolerance) << "sample " << sample;
    }
}

TEST(G711Test, UlawEncodeDecodeRoundTripStaysWithinQuantizationError) {
    for (int sample = -32124; sample <= 32124; sample += 7) {
        const auto encoded = encodeUlawSample(static_cast<std::int16_t>(sample));
        const int decoded = decodeUlawSample(encoded);
        const int tolerance = std::max(16, std::abs(sample) / 16);
        EXPECT_LE(std::abs(decoded - sample), tolerance) << "sample " << sample;
    }
}

TEST(G711Test, DecodingIsIdempotentOverAllCodes) {
    for (int code = 0; code < 256; ++code) {
        const auto value = static_cast<std::uint8_t>(code);
        EXPECT_EQ(decodeAlawSample(encodeAlawSample(decodeAlawSample(value))), decodeAlawSample(value));
    }
}

TEST(G711Test, BufferHelpersMatchSampleFunctions) {
    const std::array<std::int16_t, 4> samples{0, 1000, -1000, 30000};
    const auto alaw = encodeAlaw(samples);
    ASSERT_EQ(alaw.size(), samples.size());
    const auto decoded = decodeAlaw(alaw);
    ASSERT_EQ(decoded.size(), samples.size());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        EXPECT_EQ(alaw[index], static_cast<std::byte>(encodeAlawSample(samples[index])));
        EXPECT_EQ(decoded[index], decodeAlawSample(static_cast<std::uint8_t>(alaw[index])));
    }
}

}
