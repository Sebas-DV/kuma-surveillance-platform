#include "domain/Result.hpp"

namespace domain {

std::string_view toString(const ErrorKind kind) noexcept {
    switch (kind) {
        case ErrorKind::InvalidArgument: return "invalid_argument";
        case ErrorKind::InvalidState: return "invalid_state";
        case ErrorKind::NotFound: return "not_found";
        case ErrorKind::AlreadyExists: return "already_exists";
        case ErrorKind::Unauthenticated: return "unauthenticated";
        case ErrorKind::AccountLocked: return "account_locked";
        case ErrorKind::Forbidden: return "forbidden";
        case ErrorKind::Unreachable: return "unreachable";
        case ErrorKind::Timeout: return "timeout";
        case ErrorKind::Busy: return "busy";
        case ErrorKind::Unsupported: return "unsupported";
        case ErrorKind::Protocol: return "protocol";
        case ErrorKind::Internal: return "internal";
    }
    return "internal";
}

}
