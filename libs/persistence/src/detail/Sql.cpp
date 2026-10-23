#include "detail/Sql.hpp"

#include <algorithm>
#include <cctype>

namespace persistence::detail {

bool isUuid(const std::string_view text) noexcept {
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char symbol = text[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (symbol != '-') {
                return false;
            }
        } else if (std::isxdigit(static_cast<unsigned char>(symbol)) == 0) {
            return false;
        }
    }
    return true;
}

domain::Error invalid(std::string message) {
    return domain::Error{domain::ErrorKind::InvalidArgument, std::move(message), 0, false};
}

domain::Error notFound(std::string message) {
    return domain::Error{domain::ErrorKind::NotFound, std::move(message), 0, false};
}

domain::Error invalidCredentials() {
    return domain::Error{domain::ErrorKind::Unauthenticated, "invalid credentials", 0, false};
}

bool mentions(const domain::Error& error, const std::string_view fragment) noexcept {
    return error.message.find(fragment) != std::string::npos;
}

bool isBlank(const std::string_view text) noexcept {
    return std::ranges::all_of(text, [](const char symbol) { return std::isspace(static_cast<unsigned char>(symbol)) != 0; });
}

}
