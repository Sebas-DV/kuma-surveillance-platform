#include "dahua/LoginErrors.hpp"

namespace dahua {

domain::Error loginErrorFromNative(const int nativeCode) {
    using domain::ErrorKind;

    auto make = [nativeCode](const ErrorKind kind, std::string message, const bool retryable) {
        return domain::Error{kind, std::move(message), nativeCode, retryable};
    };

    switch (nativeCode) {
        case 1: return make(ErrorKind::Unauthenticated, "wrong password", false);
        case 2: return make(ErrorKind::Unauthenticated, "user does not exist", false);
        case 3: return make(ErrorKind::Timeout, "login timed out", true);
        case 4: return make(ErrorKind::Busy, "account already logged in", true);
        case 5: return make(ErrorKind::AccountLocked, "account locked", true);
        case 6: return make(ErrorKind::Forbidden, "account in block list", false);
        case 7: return make(ErrorKind::Busy, "device busy or out of resources", true);
        case 8: return make(ErrorKind::Unreachable, "sub-connection failed", true);
        case 9: return make(ErrorKind::Unreachable, "main connection failed", true);
        case 10: return make(ErrorKind::Busy, "maximum connections exceeded", true);
        case 11: return make(ErrorKind::Unsupported, "device only supports protocol 3", false);
        case 12: return make(ErrorKind::Forbidden, "security key missing or invalid", false);
        case 13: return make(ErrorKind::Forbidden, "client address not authorized", false);
        case 18: return make(ErrorKind::Unsupported, "device account not initialized", false);
        case 0: return make(ErrorKind::Protocol, "login rejected without an error code", true);
        default: return make(ErrorKind::Internal, "unknown login error", true);
    }
}

}
