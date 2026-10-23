#pragma once

#include <string>
#include <string_view>

#include "domain/Result.hpp"

namespace security {

inline constexpr std::size_t kMinPasswordLength = 10;
inline constexpr std::size_t kMaxPasswordLength = 256;

[[nodiscard]] domain::Result<std::string> hashPassword(std::string_view password);

[[nodiscard]] bool verifyPassword(std::string_view hash, std::string_view password) noexcept;

[[nodiscard]] bool passwordNeedsRehash(std::string_view hash) noexcept;

}
