#include "dahua/G711.hpp"

#include <algorithm>
#include <array>

namespace dahua {

namespace {

constexpr std::int16_t alawToLinear(const std::uint8_t code) {
    const unsigned value = static_cast<unsigned>(code) ^ 0x55U;
    const unsigned segment = (value & 0x70U) >> 4U;
    unsigned magnitude = (value & 0x0FU) << 4U;
    magnitude += segment == 0 ? 8U : 0x108U;
    if (segment > 1) {
        magnitude <<= (segment - 1U);
    }
    const auto result = static_cast<int>(magnitude);
    return static_cast<std::int16_t>((value & 0x80U) != 0 ? result : -result);
}

constexpr std::int16_t ulawToLinear(const std::uint8_t code) {
    const unsigned value = ~static_cast<unsigned>(code) & 0xFFU;
    const unsigned magnitude = (((value & 0x0FU) << 3U) + 0x84U) << ((value & 0x70U) >> 4U);
    const auto result = static_cast<int>(magnitude) - 0x84;
    return static_cast<std::int16_t>((value & 0x80U) != 0 ? -result : result);
}

constexpr std::array<std::int16_t, 256> buildAlawTable() {
    std::array<std::int16_t, 256> table{};
    for (std::size_t index = 0; index < table.size(); ++index) {
        table[index] = alawToLinear(static_cast<std::uint8_t>(index));
    }
    return table;
}

constexpr std::array<std::int16_t, 256> buildUlawTable() {
    std::array<std::int16_t, 256> table{};
    for (std::size_t index = 0; index < table.size(); ++index) {
        table[index] = ulawToLinear(static_cast<std::uint8_t>(index));
    }
    return table;
}

constexpr auto kAlawTable = buildAlawTable();
constexpr auto kUlawTable = buildUlawTable();

constexpr std::array<unsigned, 8> kAlawSegmentEnds{0x1FU, 0x3FU, 0x7FU, 0xFFU, 0x1FFU, 0x3FFU, 0x7FFU, 0xFFFU};
constexpr std::array<unsigned, 8> kUlawSegmentEnds{0x3FU, 0x7FU, 0xFFU, 0x1FFU, 0x3FFU, 0x7FFU, 0xFFFU, 0x1FFFU};

unsigned findSegment(const std::array<unsigned, 8>& ends, const unsigned value) {
    unsigned segment = 0;
    while (segment < ends.size() && value > ends[segment]) {
        ++segment;
    }
    return segment;
}

}

std::int16_t decodeAlawSample(const std::uint8_t code) noexcept { return kAlawTable[code]; }

std::int16_t decodeUlawSample(const std::uint8_t code) noexcept { return kUlawTable[code]; }

std::uint8_t encodeAlawSample(const std::int16_t sample) noexcept {
    const int value = sample;
    const bool negative = value < 0;
    const auto distance = static_cast<unsigned>(negative ? -value : value);
    const unsigned magnitude = negative ? (distance - 1U) / 8U : distance / 8U;
    const unsigned mask = negative ? 0x55U : 0xD5U;

    const unsigned segment = findSegment(kAlawSegmentEnds, magnitude);
    if (segment >= kAlawSegmentEnds.size()) {
        return static_cast<std::uint8_t>(0x7FU ^ mask);
    }
    const unsigned mantissa = segment < 2 ? (magnitude >> 1U) & 0x0FU : (magnitude >> segment) & 0x0FU;
    return static_cast<std::uint8_t>(((segment << 4U) | mantissa) ^ mask);
}

std::uint8_t encodeUlawSample(const std::int16_t sample) noexcept {
    const int value = sample;
    const bool negative = value < 0;
    const auto distance = static_cast<unsigned>(negative ? -value : value);
    const unsigned scaled = negative ? (distance + 3U) / 4U : distance / 4U;
    const unsigned mask = negative ? 0x7FU : 0xFFU;

    const unsigned magnitude = std::min(scaled, 8159U) + 0x21U;
    const unsigned segment = findSegment(kUlawSegmentEnds, magnitude);
    if (segment >= kUlawSegmentEnds.size()) {
        return static_cast<std::uint8_t>(0x7FU ^ mask);
    }
    return static_cast<std::uint8_t>(((segment << 4U) | ((magnitude >> (segment + 1U)) & 0x0FU)) ^ mask);
}

std::vector<std::int16_t> decodeAlaw(const std::span<const std::byte> data) {
    std::vector<std::int16_t> samples;
    samples.reserve(data.size());
    for (const std::byte value : data) {
        samples.push_back(decodeAlawSample(std::to_integer<std::uint8_t>(value)));
    }
    return samples;
}

std::vector<std::int16_t> decodeUlaw(const std::span<const std::byte> data) {
    std::vector<std::int16_t> samples;
    samples.reserve(data.size());
    for (const std::byte value : data) {
        samples.push_back(decodeUlawSample(std::to_integer<std::uint8_t>(value)));
    }
    return samples;
}

std::vector<std::byte> encodeAlaw(const std::span<const std::int16_t> samples) {
    std::vector<std::byte> data;
    data.reserve(samples.size());
    for (const std::int16_t sample : samples) {
        data.push_back(static_cast<std::byte>(encodeAlawSample(sample)));
    }
    return data;
}

std::vector<std::byte> encodeUlaw(const std::span<const std::int16_t> samples) {
    std::vector<std::byte> data;
    data.reserve(samples.size());
    for (const std::int16_t sample : samples) {
        data.push_back(static_cast<std::byte>(encodeUlawSample(sample)));
    }
    return data;
}

}
