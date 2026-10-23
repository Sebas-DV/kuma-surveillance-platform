#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "domain/Result.hpp"
#include "persistence/Database.hpp"
#include "persistence/Models.hpp"

namespace persistence {

class ApiKeyRepository {
public:
    explicit ApiKeyRepository(std::shared_ptr<Database> database);

    [[nodiscard]] domain::Result<IssuedApiKey> issue(std::string_view userId, std::string_view name, ApiKeyKind kind,
                                                     std::optional<std::chrono::seconds> ttl);
    [[nodiscard]] domain::Result<Principal> authenticate(std::string_view token);
    [[nodiscard]] domain::Result<std::vector<ApiKey>> list(std::string_view userId);
    [[nodiscard]] domain::Result<void> revoke(std::string_view keyId, std::optional<std::string_view> ownerUserId);
    [[nodiscard]] domain::Result<std::size_t> purgeExpired();

private:
    std::shared_ptr<Database> database_;
};

}
