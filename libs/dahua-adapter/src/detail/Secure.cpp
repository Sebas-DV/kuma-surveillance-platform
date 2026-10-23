#include "detail/Secure.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cstring>
#endif

namespace dahua::detail {

void secureWipe(void* data, const std::size_t size) noexcept {
    if (data == nullptr || size == 0) {
        return;
    }
#ifdef _WIN32
    SecureZeroMemory(data, size);
#else
    explicit_bzero(data, size);
#endif
}

void secureWipe(const std::span<char> buffer) noexcept { secureWipe(buffer.data(), buffer.size()); }

void secureWipe(std::string& text) noexcept {
    secureWipe(text.data(), text.size());
    text.clear();
}

}
