#pragma once

#include <string>

namespace persistence {

[[nodiscard]] std::string withoutRowDetails(std::string message);

void sanitizeDriverLogs();

}
