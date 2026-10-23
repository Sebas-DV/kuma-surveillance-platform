#include "dahua/Types.hpp"

#include <utility>

#include "detail/Secure.hpp"

namespace dahua {

Credentials::Credentials(std::string username, std::string password)
    : username_(std::move(username)), password_(std::move(password)) {}

Credentials::~Credentials() { detail::secureWipe(password_); }

const char* toString(const SessionState state) noexcept {
    switch (state) {
        case SessionState::Registered: return "registered";
        case SessionState::Authenticating: return "authenticating";
        case SessionState::Online: return "online";
        case SessionState::Offline: return "offline";
        case SessionState::Backoff: return "backoff";
        case SessionState::Blocked: return "blocked";
    }
    return "offline";
}

}
