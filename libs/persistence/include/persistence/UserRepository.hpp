#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "domain/Result.hpp"
#include "persistence/Database.hpp"
#include "persistence/Models.hpp"

namespace persistence {

inline constexpr int kMaxFailedLogins = 5;
inline constexpr int kLockoutMinutes = 15;

class UserRepository {
public:
    explicit UserRepository(std::shared_ptr<Database> database);

    [[nodiscard]] domain::Result<User> create(std::string_view username, std::string_view password, UserRole role);
    [[nodiscard]] domain::Result<User> authenticate(std::string_view username, std::string_view password);
    [[nodiscard]] domain::Result<User> get(std::string_view id);
    [[nodiscard]] domain::Result<std::vector<User>> list();
    [[nodiscard]] domain::Result<User> update(std::string_view id, const UserPatch& patch);
    [[nodiscard]] domain::Result<void> remove(std::string_view id);
    [[nodiscard]] domain::Result<long long> count();

private:
    std::shared_ptr<Database> database_;
};

}
