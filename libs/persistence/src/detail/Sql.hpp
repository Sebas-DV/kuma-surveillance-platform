#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "domain/Result.hpp"

namespace persistence::detail {

[[nodiscard]] inline std::string epochMillis(const std::string_view column) {
    return "(extract(epoch from " + std::string(column) + ")*1000)::bigint";
}

[[nodiscard]] bool isUuid(std::string_view text) noexcept;

[[nodiscard]] domain::Error invalid(std::string message);

[[nodiscard]] domain::Error notFound(std::string message);

[[nodiscard]] domain::Error invalidCredentials();

[[nodiscard]] bool mentions(const domain::Error& error, std::string_view fragment) noexcept;

[[nodiscard]] bool isBlank(std::string_view text) noexcept;

}
