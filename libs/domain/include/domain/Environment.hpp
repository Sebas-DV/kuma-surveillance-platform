#pragma once

#include <optional>
#include <string>

namespace domain {

[[nodiscard]] std::optional<std::string> readEnvironment(const std::string& name);

}
