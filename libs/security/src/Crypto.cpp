#include "security/Crypto.hpp"

#include <sodium.h>

#include <array>

namespace security {

domain::Result<void> initializeCrypto() {
    if (sodium_init() < 0) {
        return domain::Result<void>::failure(
            domain::Error{domain::ErrorKind::Internal, "crypto library initialization failed", 0, false});
    }
    return domain::Result<void>::success();
}

domain::Result<std::string> generateToken(const std::string_view prefix) {
    if (const auto ready = initializeCrypto(); !ready) {
        return domain::Result<std::string>::failure(ready.error());
    }
    std::array<unsigned char, 32> random{};
    randombytes_buf(random.data(), random.size());

    std::string encoded(sodium_base64_ENCODED_LEN(random.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
    sodium_bin2base64(encoded.data(), encoded.size(), random.data(), random.size(),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    encoded.resize(std::char_traits<char>::length(encoded.c_str()));
    sodium_memzero(random.data(), random.size());

    std::string token(prefix);
    token += encoded;
    return domain::Result<std::string>::success(std::move(token));
}

Bytes sha256(const std::string_view data) {
    Bytes digest(crypto_hash_sha256_BYTES);
    crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return digest;
}

Bytes hashToken(const std::string_view token) { return sha256(token); }

std::string toHex(const Bytes& bytes) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        text.push_back(kDigits[byte >> 4U]);
        text.push_back(kDigits[byte & 0x0FU]);
    }
    return text;
}

domain::Result<Bytes> fromHex(const std::string_view text) {
    const auto nibble = [](const char symbol) -> int {
        if (symbol >= '0' && symbol <= '9') {
            return symbol - '0';
        }
        if (symbol >= 'a' && symbol <= 'f') {
            return symbol - 'a' + 10;
        }
        if (symbol >= 'A' && symbol <= 'F') {
            return symbol - 'A' + 10;
        }
        return -1;
    };

    if (text.size() % 2 != 0) {
        return domain::Result<Bytes>::failure(
            domain::Error{domain::ErrorKind::InvalidArgument, "hex text has odd length", 0, false});
    }
    Bytes bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const int high = nibble(text[index]);
        const int low = nibble(text[index + 1]);
        if (high < 0 || low < 0) {
            return domain::Result<Bytes>::failure(
                domain::Error{domain::ErrorKind::InvalidArgument, "hex text has invalid digit", 0, false});
        }
        bytes.push_back(static_cast<std::uint8_t>((static_cast<unsigned>(high) << 4U) | static_cast<unsigned>(low)));
    }
    return domain::Result<Bytes>::success(std::move(bytes));
}

void wipe(std::string& value) noexcept {
    if (!value.empty()) {
        sodium_memzero(value.data(), value.size());
    }
    value.clear();
}

}
