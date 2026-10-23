#include "persistence/UserRepository.hpp"

#include <regex>

#include "detail/Sql.hpp"
#include "security/PasswordHasher.hpp"

namespace persistence {

namespace {

using domain::Error;
using domain::ErrorKind;

const std::string& userColumns() {
    static const std::string columns = "id::text, username, role::text, disabled, " +
                                       detail::epochMillis("locked_until") + ", " +
                                       detail::epochMillis("created_at") + ", " + detail::epochMillis("updated_at");
    return columns;
}

User userFromRow(const Rows& rows, const std::size_t row) {
    User user;
    user.id = rows.text(row, 0);
    user.username = rows.text(row, 1);
    user.role = parseUserRole(rows.text(row, 2)).value_or(UserRole::Viewer);
    user.disabled = rows.boolean(row, 3);
    user.lockedUntil = rows.optionalTime(row, 4);
    user.createdAt = rows.time(row, 5);
    user.updatedAt = rows.time(row, 6);
    return user;
}

bool validUsername(const std::string_view username) {
    static const std::regex pattern("^[A-Za-z0-9._@-]{3,64}$");
    return std::regex_match(username.begin(), username.end(), pattern);
}

const std::string& decoyHash() {
    static const std::string hash = [] {
        auto computed = security::hashPassword("decoy-password-for-timing");
        return computed ? computed.value() : std::string();
    }();
    return hash;
}

}

UserRepository::UserRepository(std::shared_ptr<Database> database) : database_(std::move(database)) {}

domain::Result<User> UserRepository::create(const std::string_view username, const std::string_view password,
                                            const UserRole role) {
    using Outcome = domain::Result<User>;
    if (!validUsername(username)) {
        return Outcome::failure(
            detail::invalid("username must be 3-64 characters: letters, digits, '.', '_', '@' or '-'"));
    }
    const auto hash = security::hashPassword(password);
    if (!hash) {
        return Outcome::failure(hash.error());
    }
    const auto rows = database_->query("INSERT INTO users (username, password_hash, role) "
                                       "VALUES ($1, $2, $3::user_role) RETURNING " +
                                           userColumns(),
                                       {std::string(username), hash.value(), std::string(toString(role))});
    if (!rows) {
        if (rows.error().kind == ErrorKind::AlreadyExists || detail::mentions(rows.error(), "users_username_key")) {
            return Outcome::failure(Error{ErrorKind::AlreadyExists, "username is already in use", 0, false});
        }
        return Outcome::failure(rows.error());
    }
    return Outcome::success(userFromRow(rows.value(), 0));
}

domain::Result<User> UserRepository::authenticate(const std::string_view username, const std::string_view password) {
    using Outcome = domain::Result<User>;
    const auto rows = database_->query(
        "SELECT " + userColumns() + ", password_hash, (locked_until IS NOT NULL AND locked_until > now()) "
                                    "FROM users WHERE lower(username) = lower($1)",
        {std::string(username)});
    if (!rows) {
        return Outcome::failure(rows.error());
    }
    if (rows.value().size() == 0) {
        static_cast<void>(security::verifyPassword(decoyHash(), password));
        return Outcome::failure(detail::invalidCredentials());
    }

    const User user = userFromRow(rows.value(), 0);
    const std::string storedHash = rows.value().text(0, 7);
    const bool locked = rows.value().boolean(0, 8);
    if (locked) {
        return Outcome::failure(Error{ErrorKind::AccountLocked, "account temporarily locked", 0, true});
    }

    if (!security::verifyPassword(storedHash, password) || user.disabled) {
        const auto failed = database_->query(
            "UPDATE users SET failed_logins = CASE WHEN failed_logins + 1 >= $2::int THEN 0 ELSE failed_logins + 1 END, "
            "locked_until = CASE WHEN failed_logins + 1 >= $2::int THEN now() + make_interval(mins => $3::int) "
            "ELSE locked_until END WHERE id = $1::uuid",
            {user.id, std::to_string(kMaxFailedLogins), std::to_string(kLockoutMinutes)});
        if (!failed) {
            return Outcome::failure(failed.error());
        }
        return Outcome::failure(detail::invalidCredentials());
    }

    Param refreshedHash;
    if (security::passwordNeedsRehash(storedHash)) {
        if (auto rehashed = security::hashPassword(password)) {
            refreshedHash = std::move(rehashed).value();
        }
    }
    const auto reset = database_->query("UPDATE users SET failed_logins = 0, locked_until = NULL, "
                                        "password_hash = COALESCE($2, password_hash) WHERE id = $1::uuid",
                                        {user.id, refreshedHash});
    if (!reset) {
        return Outcome::failure(reset.error());
    }
    return Outcome::success(user);
}

domain::Result<User> UserRepository::get(const std::string_view id) {
    using Outcome = domain::Result<User>;
    if (!detail::isUuid(id)) {
        return Outcome::failure(detail::invalid("user id must be a uuid"));
    }
    const auto rows = database_->query("SELECT " + userColumns() + " FROM users WHERE id = $1::uuid",
                                       {std::string(id)});
    if (!rows) {
        return Outcome::failure(rows.error());
    }
    if (rows.value().size() == 0) {
        return Outcome::failure(detail::notFound("user not found"));
    }
    return Outcome::success(userFromRow(rows.value(), 0));
}

domain::Result<std::vector<User>> UserRepository::list() {
    using Outcome = domain::Result<std::vector<User>>;
    const auto rows = database_->query("SELECT " + userColumns() + " FROM users ORDER BY lower(username)");
    if (!rows) {
        return Outcome::failure(rows.error());
    }
    std::vector<User> users;
    users.reserve(rows.value().size());
    for (std::size_t row = 0; row < rows.value().size(); ++row) {
        users.push_back(userFromRow(rows.value(), row));
    }
    return Outcome::success(std::move(users));
}

domain::Result<User> UserRepository::update(const std::string_view id, const UserPatch& patch) {
    using Outcome = domain::Result<User>;
    if (!detail::isUuid(id)) {
        return Outcome::failure(detail::invalid("user id must be a uuid"));
    }
    Param role;
    if (patch.role) {
        role = std::string(toString(*patch.role));
    }
    Param disabled;
    if (patch.disabled) {
        disabled = *patch.disabled ? "true" : "false";
    }
    Param hash;
    if (patch.password) {
        auto hashed = security::hashPassword(*patch.password);
        if (!hashed) {
            return Outcome::failure(hashed.error());
        }
        hash = std::move(hashed).value();
    }
    const auto rows = database_->query(
        "UPDATE users SET role = COALESCE($2::user_role, role), disabled = COALESCE($3::boolean, disabled), "
        "password_hash = COALESCE($4, password_hash), "
        "failed_logins = CASE WHEN $4::text IS NOT NULL THEN 0 ELSE failed_logins END, "
        "locked_until = CASE WHEN $4::text IS NOT NULL THEN NULL ELSE locked_until END, updated_at = now() "
        "WHERE id = $1::uuid RETURNING " +
            userColumns(),
        {std::string(id), role, disabled, hash});
    if (!rows) {
        return Outcome::failure(rows.error());
    }
    if (rows.value().size() == 0) {
        return Outcome::failure(detail::notFound("user not found"));
    }
    return Outcome::success(userFromRow(rows.value(), 0));
}

domain::Result<void> UserRepository::remove(const std::string_view id) {
    if (!detail::isUuid(id)) {
        return domain::Result<void>::failure(detail::invalid("user id must be a uuid"));
    }
    const auto removed = database_->query("DELETE FROM users WHERE id = $1::uuid", {std::string(id)});
    if (!removed) {
        return domain::Result<void>::failure(removed.error());
    }
    if (removed.value().affected() == 0) {
        return domain::Result<void>::failure(detail::notFound("user not found"));
    }
    return domain::Result<void>::success();
}

domain::Result<long long> UserRepository::count() {
    const auto rows = database_->query("SELECT count(*) FROM users");
    if (!rows) {
        return domain::Result<long long>::failure(rows.error());
    }
    return domain::Result<long long>::success(rows.value().integer(0, 0));
}

}
