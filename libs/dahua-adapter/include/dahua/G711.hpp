#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dahua {

[[nodiscard]] std::int16_t decodeAlawSample(std::uint8_t code) noexcept;
[[nodiscard]] std::int16_t decodeUlawSample(std::uint8_t code) noexcept;
[[nodiscard]] std::uint8_t encodeAlawSample(std::int16_t sample) noexcept;
[[nodiscard]] std::uint8_t encodeUlawSample(std::int16_t sample) noexcept;

[[nodiscard]] std::vector<std::int16_t> decodeAlaw(std::span<const std::byte> data);
[[nodiscard]] std::vector<std::int16_t> decodeUlaw(std::span<const std::byte> data);
[[nodiscard]] std::vector<std::byte> encodeAlaw(std::span<const std::int16_t> samples);
[[nodiscard]] std::vector<std::byte> encodeUlaw(std::span<const std::int16_t> samples);

}
