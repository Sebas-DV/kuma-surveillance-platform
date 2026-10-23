#include "detail/SdkSupport.hpp"

#include <algorithm>
#include <format>

#include "dhnetsdk.h"

namespace dahua::detail {

domain::Error sdkError(const std::string_view operation) {
    const DWORD code = CLIENT_GetLastError();
    domain::ErrorKind kind = domain::ErrorKind::Internal;
    bool retryable = false;

    switch (code) {
        case NET_NETWORK_ERROR:
        case NET_LOGIN_ERROR_NETWORK:
        case NET_LOGIN_ERROR_CONNECT:
        case NET_LOGIN_ERROR_SUBCONNECT:
            kind = domain::ErrorKind::Unreachable;
            retryable = true;
            break;
        case NET_LOGIN_ERROR_TIMEOUT:
            kind = domain::ErrorKind::Timeout;
            retryable = true;
            break;
        case NET_LOGIN_ERROR_PASSWORD:
        case NET_LOGIN_ERROR_USER:
        case NET_LOGIN_ERROR_USER_OR_PASSOWRD:
            kind = domain::ErrorKind::Unauthenticated;
            break;
        case NET_LOGIN_ERROR_LOCKED:
            kind = domain::ErrorKind::AccountLocked;
            retryable = true;
            break;
        case NET_LOGIN_ERROR_BLACKLIST:
        case NET_LOGIN_ERROR_NO_AUTHORIZED:
            kind = domain::ErrorKind::Forbidden;
            break;
        case NET_LOGIN_ERROR_BUSY:
        case NET_LOGIN_ERROR_MAXCONNECT:
        case NET_LOGIN_ERROR_RELOGGIN:
            kind = domain::ErrorKind::Busy;
            retryable = true;
            break;
        default:
            break;
    }

    return domain::Error{kind, std::format("{} failed (sdk 0x{:08x})", operation, static_cast<unsigned long>(code)),
                         static_cast<std::int64_t>(code), retryable};
}

void copyToFixed(const std::span<char> destination, const std::string_view source) noexcept {
    if (destination.empty()) {
        return;
    }
    const std::size_t count = std::min(source.size(), destination.size() - 1);
    std::copy_n(source.begin(), count, destination.begin());
    std::fill(destination.begin() + static_cast<std::ptrdiff_t>(count), destination.end(), '\0');
}

std::string readFixed(const std::span<const unsigned char> source) {
    std::string result;
    for (const unsigned char value : source) {
        if (value == 0) {
            break;
        }
        result.push_back(static_cast<char>(value));
    }
    return result;
}

std::string readCString(const void* data, const std::size_t length, const std::size_t maxLength) {
    if (data == nullptr || length == 0) {
        return {};
    }
    return readCString(std::span<const char>(static_cast<const char*>(data), length), maxLength);
}

std::string readCString(const std::span<const char> source, const std::size_t maxLength) {
    const std::size_t limit = std::min(source.size(), maxLength);
    const auto end = std::find(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(limit), '\0');
    return {source.begin(), end};
}

}
