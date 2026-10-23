#include "DhavFrameBuilder.hpp"

#include <algorithm>

namespace testing {

namespace {

void appendLe32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

}

std::vector<std::byte> buildFrame(const FrameSpec& spec) {
    std::vector<std::byte> frame;
    const std::size_t total = 24 + spec.extension.size() + spec.payload.size() + 8;
    frame.reserve(total);

    for (const char character : {'D', 'H', 'A', 'V'}) {
        frame.push_back(static_cast<std::byte>(character));
    }
    frame.push_back(static_cast<std::byte>(spec.type));
    frame.insert(frame.end(), 3, std::byte{0});
    appendLe32(frame, spec.sequence);
    appendLe32(frame, static_cast<std::uint32_t>(total));
    appendLe32(frame, spec.packedDateTime);
    frame.push_back(static_cast<std::byte>(spec.timestamp & 0xFFU));
    frame.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(spec.timestamp) >> 8U) & 0xFFU));
    frame.push_back(static_cast<std::byte>(spec.extension.size()));

    unsigned checksum = 0;
    for (const std::byte value : frame) {
        checksum += std::to_integer<unsigned>(value);
    }
    frame.push_back(static_cast<std::byte>(checksum & 0xFFU));

    for (const std::uint8_t value : spec.extension) {
        frame.push_back(static_cast<std::byte>(value));
    }
    for (const std::uint8_t value : spec.payload) {
        frame.push_back(static_cast<std::byte>(value));
    }
    for (const char character : {'d', 'h', 'a', 'v'}) {
        frame.push_back(static_cast<std::byte>(character));
    }
    appendLe32(frame, static_cast<std::uint32_t>(total));
    return frame;
}

std::vector<std::uint8_t> audioExtension(const std::uint8_t codecId, const std::uint8_t rateCode) {
    return {0x83, 0x01, codecId, rateCode, 0x96, 0x00, 0x00, 0x00};
}

std::vector<std::uint8_t> videoExtension(const std::uint8_t widthUnits, const std::uint8_t heightUnits,
                                         const std::uint8_t codecId, const std::uint8_t framesPerSecond) {
    return {0x80, 0x00, widthUnits, heightUnits, 0x81, 0x32, codecId, framesPerSecond,
            0x88, 0x17, 0x61, 0xC3, 0x03, 0x00, 0x00, 0x00};
}

std::vector<std::byte> concatenate(const std::span<const std::vector<std::byte>> parts) {
    std::vector<std::byte> result;
    for (const auto& part : parts) {
        result.insert(result.end(), part.begin(), part.end());
    }
    return result;
}

}
