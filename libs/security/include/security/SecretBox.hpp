#pragma once

#include <array>
#include <string>
#include <string_view>

#include "domain/Result.hpp"
#include "security/Crypto.hpp"

namespace security {

class SecretBox {
public:
    static constexpr std::size_t kKeySize = 32;

    [[nodiscard]] static domain::Result<SecretBox> fromBase64(std::string_view encodedKey);

    SecretBox(const SecretBox&) = default;
    SecretBox(SecretBox&&) noexcept = default;
    SecretBox& operator=(const SecretBox&) = default;
    SecretBox& operator=(SecretBox&&) noexcept = default;
    ~SecretBox();

    [[nodiscard]] domain::Result<Bytes> seal(std::string_view plaintext) const;

    [[nodiscard]] domain::Result<std::string> open(const Bytes& sealed) const;

private:
    explicit SecretBox(const std::array<std::uint8_t, kKeySize>& key) : key_(key) {}

    std::array<std::uint8_t, kKeySize> key_{};
};

}
