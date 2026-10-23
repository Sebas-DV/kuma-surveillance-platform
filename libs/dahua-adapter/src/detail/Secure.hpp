#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace dahua::detail {

void secureWipe(void* data, std::size_t size) noexcept;
void secureWipe(std::span<char> buffer) noexcept;
void secureWipe(std::string& text) noexcept;

}
