#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "domain/Result.hpp"

namespace security {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::size_t kDigestSize = 32;

[[nodiscard]] domain::Result<void> initializeCrypto();

[[nodiscard]] domain::Result<std::string> generateToken(std::string_view prefix);

[[nodiscard]] Bytes sha256(std::string_view data);

[[nodiscard]] Bytes hashToken(std::string_view token);

[[nodiscard]] std::string toHex(const Bytes& bytes);

[[nodiscard]] domain::Result<Bytes> fromHex(std::string_view text);

void wipe(std::string& value) noexcept;

}
