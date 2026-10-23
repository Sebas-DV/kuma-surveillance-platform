#include "security/PasswordHasher.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>

#include "security/Crypto.hpp"

namespace security {

domain::Result<std::string> hashPassword(const std::string_view password) {
    if (password.size() < kMinPasswordLength || password.size() > kMaxPasswordLength) {
        return domain::Result<std::string>::failure(
            domain::Error{domain::ErrorKind::InvalidArgument,
                          "password length must be between " + std::to_string(kMinPasswordLength) + " and " +
                              std::to_string(kMaxPasswordLength) + " characters",
                          0, false});
    }
    if (const auto ready = initializeCrypto(); !ready) {
        return domain::Result<std::string>::failure(ready.error());
    }
    std::array<char, crypto_pwhash_STRBYTES> encoded{};
    if (crypto_pwhash_str_alg(encoded.data(), password.data(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_ARGON2ID13) != 0) {
        return domain::Result<std::string>::failure(
            domain::Error{domain::ErrorKind::Internal, "password hashing failed", 0, false});
    }
    return domain::Result<std::string>::success(std::string(encoded.data()));
}

bool verifyPassword(const std::string_view hash, const std::string_view password) noexcept {
    if (hash.empty() || hash.size() >= crypto_pwhash_STRBYTES || sodium_init() < 0) {
        return false;
    }
    std::array<char, crypto_pwhash_STRBYTES> terminated{};
    std::ranges::copy(hash, terminated.begin());
    return crypto_pwhash_str_verify(terminated.data(), password.data(), password.size()) == 0;
}

bool passwordNeedsRehash(const std::string_view hash) noexcept {
    if (hash.empty() || hash.size() >= crypto_pwhash_STRBYTES || sodium_init() < 0) {
        return true;
    }
    std::array<char, crypto_pwhash_STRBYTES> terminated{};
    std::ranges::copy(hash, terminated.begin());
    return crypto_pwhash_str_needs_rehash(terminated.data(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0;
}

}
