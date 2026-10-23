#include "security/SecretBox.hpp"

#include <sodium.h>

namespace security {

namespace {

constexpr std::string_view kContext = "dvr.credential.v1";

domain::Error secretError(const domain::ErrorKind kind, std::string message) {
    return domain::Error{kind, std::move(message), 0, false};
}

}

domain::Result<SecretBox> SecretBox::fromBase64(const std::string_view encodedKey) {
    if (const auto ready = initializeCrypto(); !ready) {
        return domain::Result<SecretBox>::failure(ready.error());
    }
    std::array<std::uint8_t, kKeySize> key{};
    std::size_t decoded = 0;
    const char* end = nullptr;
    const int status = sodium_base642bin(key.data(), key.size(), encodedKey.data(), encodedKey.size(), nullptr,
                                         &decoded, &end, sodium_base64_VARIANT_ORIGINAL);
    if (status != 0 || decoded != kKeySize || end != encodedKey.data() + encodedKey.size()) {
        sodium_memzero(key.data(), key.size());
        return domain::Result<SecretBox>::failure(
            secretError(domain::ErrorKind::InvalidArgument, "secret key must be 32 bytes encoded as base64"));
    }
    SecretBox box(key);
    sodium_memzero(key.data(), key.size());
    return domain::Result<SecretBox>::success(std::move(box));
}

SecretBox::~SecretBox() { sodium_memzero(key_.data(), key_.size()); }

domain::Result<Bytes> SecretBox::seal(const std::string_view plaintext) const {
    if (const auto ready = initializeCrypto(); !ready) {
        return domain::Result<Bytes>::failure(ready.error());
    }
    Bytes sealed(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + plaintext.size() +
                 crypto_aead_xchacha20poly1305_ietf_ABYTES);
    randombytes_buf(sealed.data(), crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);

    unsigned long long cipherLength = 0;
    const int status = crypto_aead_xchacha20poly1305_ietf_encrypt(
        sealed.data() + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, &cipherLength,
        reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size(),
        reinterpret_cast<const unsigned char*>(kContext.data()), kContext.size(), nullptr, sealed.data(),
        key_.data());
    if (status != 0) {
        return domain::Result<Bytes>::failure(secretError(domain::ErrorKind::Internal, "encryption failed"));
    }
    sealed.resize(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + static_cast<std::size_t>(cipherLength));
    return domain::Result<Bytes>::success(std::move(sealed));
}

domain::Result<std::string> SecretBox::open(const Bytes& sealed) const {
    constexpr std::size_t kOverhead =
        crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + crypto_aead_xchacha20poly1305_ietf_ABYTES;
    if (sealed.size() < kOverhead) {
        return domain::Result<std::string>::failure(
            secretError(domain::ErrorKind::InvalidArgument, "sealed value is too short"));
    }
    if (const auto ready = initializeCrypto(); !ready) {
        return domain::Result<std::string>::failure(ready.error());
    }
    std::string plaintext(sealed.size() - kOverhead, '\0');
    unsigned long long plainLength = 0;
    const int status = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char*>(plaintext.data()), &plainLength, nullptr,
        sealed.data() + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,
        sealed.size() - crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,
        reinterpret_cast<const unsigned char*>(kContext.data()), kContext.size(), sealed.data(), key_.data());
    if (status != 0) {
        return domain::Result<std::string>::failure(
            secretError(domain::ErrorKind::InvalidArgument, "decryption failed: wrong key or corrupted value"));
    }
    plaintext.resize(static_cast<std::size_t>(plainLength));
    return domain::Result<std::string>::success(std::move(plaintext));
}

}
