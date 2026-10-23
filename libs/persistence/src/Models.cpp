#include "persistence/Models.hpp"

namespace persistence {

std::string_view toString(const UserRole role) noexcept {
    switch (role) {
        case UserRole::Viewer: return "viewer";
        case UserRole::Operator: return "operator";
        case UserRole::Admin: return "admin";
    }
    return "viewer";
}

std::optional<UserRole> parseUserRole(const std::string_view text) noexcept {
    if (text == "viewer") {
        return UserRole::Viewer;
    }
    if (text == "operator") {
        return UserRole::Operator;
    }
    if (text == "admin") {
        return UserRole::Admin;
    }
    return std::nullopt;
}

bool atLeast(const UserRole actual, const UserRole required) noexcept {
    return static_cast<int>(actual) >= static_cast<int>(required);
}

std::string_view toString(const ApiKeyKind kind) noexcept {
    switch (kind) {
        case ApiKeyKind::Session: return "session";
        case ApiKeyKind::Service: return "service";
    }
    return "service";
}

std::optional<ApiKeyKind> parseApiKeyKind(const std::string_view text) noexcept {
    if (text == "session") {
        return ApiKeyKind::Session;
    }
    if (text == "service") {
        return ApiKeyKind::Service;
    }
    return std::nullopt;
}

}
