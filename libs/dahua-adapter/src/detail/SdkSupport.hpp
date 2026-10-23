#pragma once

#include <span>
#include <string>
#include <string_view>

#include "domain/Result.hpp"

namespace dahua::detail {

[[nodiscard]] domain::Error sdkError(std::string_view operation);

void copyToFixed(std::span<char> destination, std::string_view source) noexcept;

[[nodiscard]] std::string readFixed(std::span<const unsigned char> source);

[[nodiscard]] std::string readCString(const void* data, std::size_t length, std::size_t maxLength);

[[nodiscard]] std::string readCString(std::span<const char> source, std::size_t maxLength);

}
