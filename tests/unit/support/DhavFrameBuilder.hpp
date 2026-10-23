#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace testing {

struct FrameSpec {
    std::uint8_t type{0xF0};
    std::uint32_t sequence{0};
    std::uint32_t packedDateTime{0};
    std::uint16_t timestamp{0};
    std::vector<std::uint8_t> extension;
    std::vector<std::uint8_t> payload;
};

[[nodiscard]] std::vector<std::byte> buildFrame(const FrameSpec& spec);

[[nodiscard]] std::vector<std::uint8_t> audioExtension(std::uint8_t codecId, std::uint8_t rateCode = 2);

[[nodiscard]] std::vector<std::uint8_t> videoExtension(std::uint8_t widthUnits, std::uint8_t heightUnits,
                                                       std::uint8_t codecId, std::uint8_t framesPerSecond);

[[nodiscard]] std::vector<std::byte> concatenate(std::span<const std::vector<std::byte>> parts);

}
