#include "domain/Environment.hpp"

#include <cstdlib>

namespace domain {

std::optional<std::string> readEnvironment(const std::string& name) {
#ifdef _WIN32
    std::size_t required = 0;
    if (getenv_s(&required, nullptr, 0, name.c_str()) != 0 || required == 0) {
        return std::nullopt;
    }
    std::string value(required, '\0');
    std::size_t written = 0;
    if (getenv_s(&written, value.data(), value.size(), name.c_str()) != 0 || written == 0) {
        return std::nullopt;
    }
    value.resize(written - 1);
    return value;
#else
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

}
