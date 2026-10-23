#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "domain/Result.hpp"
#include "persistence/Database.hpp"

namespace persistence {

struct MigrationReport {
    std::vector<std::string> applied;
    std::size_t alreadyApplied{0};
};

[[nodiscard]] domain::Result<MigrationReport> migrate(Database& database, const std::filesystem::path& directory);

}
