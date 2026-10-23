#include "persistence/ApiKeyRepository.hpp"

#include "detail/Sql.hpp"
#include "security/Crypto.hpp"

namespace persistence {

namespace {

constexpr std::string_view kSessionPrefix = "ses_";
constexpr std::string_view kServicePrefix = "key_";
constexpr std::size_t kMaxNameLength = 120;

const std::string& keyColumns() {
    static const std::string columns = "id::text, user_id::text, name, kind::text, " +
                                       detail::epochMillis("expires_at") + ", " + detail::epochMillis("revoked_at") +
                                       ", " + detail::epochMillis("last_used_at") + ", " +
                                       detail::epochMillis("created_at");
    return columns;
}

ApiKey keyFromRow(const Rows& rows, const std::size_t row) {
    ApiKey key;
    key.id = rows.text(row, 0);
    key.userId = rows.text(row, 1);
    key.name = rows.text(row, 2);
    key.kind = parseApiKeyKind(rows.text(row, 3)).value_or(ApiKeyKind::Service);
    key.expiresAt = rows.optionalTime(row, 4);
    key.revokedAt = rows.optionalTime(row, 5);
    key.lastUsedAt = rows.optionalTime(row, 6);
    key.createdAt = rows.time(row, 7);
    return key;
}

}

ApiKeyRepository::ApiKeyRepository(std::shared_ptr<Database> database) : database_(std::move(database)) {}

domain::Result<IssuedApiKey> ApiKeyRepository::issue(const std::string_view userId, const std::string_view name,
                                                     const ApiKeyKind kind,
                                                     const std::optional<std::chrono::seconds> ttl) {
    using Outcome = domain::Result<IssuedApiKey>;
    if (!detail::isUuid(userId)) {
        return Outcome::failure(detail::invalid("user id must be a uuid"));
    }
    if (detail::isBlank(name) || name.size() > kMaxNameLength) {
        return Outcome::failure(detail::invalid("key name must be between 1 and 120 characters"));
    }
    if (ttl && ttl->count() <= 0) {
        return Outcome::failure(detail::invalid("key lifetime must be positive"));
    }
    if (kind == ApiKeyKind::Session && !ttl) {
        return Outcome::failure(detail::invalid("session keys require a lifetime"));
    }

    auto token = security::generateToken(kind == ApiKeyKind::Session ? kSessionPrefix : kServicePrefix);
    if (!token) {
        return Outcome::failure(token.error());
    }
    const std::string hash = "\\x" + security::toHex(security::hashToken(token.value()));
    Param lifetime;
    if (ttl) {
        lifetime = std::to_string(ttl->count());
    }

    const auto rows = database_->query(
        "INSERT INTO api_keys (user_id, name, kind, key_hash, expires_at) "
        "SELECT u.id, $2, $3::api_key_kind, $4::bytea, "
        "CASE WHEN $5::bigint IS NULL THEN NULL ELSE now() + make_interval(secs => $5::bigint) END "
        "FROM users u WHERE u.id = $1::uuid AND NOT u.disabled RETURNING " +
            keyColumns(),
        {std::string(userId), std::string(name), std::string(toString(kind)), hash, lifetime});
    if (!rows) {
        return Outcome::failure(rows.error());
    }
    if (rows.value().size() == 0) {
        return Outcome::failure(detail::notFound("user not found or disabled"));
    }
    return Outcome::success(IssuedApiKey{keyFromRow(rows.value(), 0), std::move(token).value()});
}

domain::Result<Principal> ApiKeyRepository::authenticate(const std::string_view token) {
    using Outcome = domain::Result<Principal>;
    if (token.size() < 16 || token.size() > 128) {
        return Outcome::failure(detail::invalidCredentials());
    }
    const std::string hash = "\\x" + security::toHex(security::hashToken(token));
    const auto rows = database_->query(
        "SELECT u.id::text, u.username, u.role::text, k.id::text FROM api_keys k JOIN users u ON u.id = k.user_id "
        "WHERE k.key_hash = $1::bytea AND k.revoked_at IS NULL AND (k.expires_at IS NULL OR k.expires_at > now()) "
        "AND NOT u.disabled",
        {hash});
    if (!rows) {
        return Outcome::failure(rows.error());
    }
    if (rows.value().size() == 0) {
        return Outcome::failure(detail::invalidCredentials());
    }
    Principal principal;
    principal.userId = rows.value().text(0, 0);
    principal.username = rows.value().text(0, 1);
    principal.role = parseUserRole(rows.value().text(0, 2)).value_or(UserRole::Viewer);
    principal.apiKeyId = rows.value().text(0, 3);

    static_cast<void>(database_->query("UPDATE api_keys SET last_used_at = now() WHERE id = $1::uuid "
                                       "AND (last_used_at IS NULL OR last_used_at < now() - interval '1 minute')",
                                       {principal.apiKeyId}));
    return Outcome::success(std::move(principal));
}

domain::Result<std::vector<ApiKey>> ApiKeyRepository::list(const std::string_view userId) {
    using Outcome = domain::Result<std::vector<ApiKey>>;
    if (!detail::isUuid(userId)) {
        return Outcome::failure(detail::invalid("user id must be a uuid"));
    }
    const auto rows = database_->query(
        "SELECT " + keyColumns() + " FROM api_keys WHERE user_id = $1::uuid ORDER BY created_at DESC",
        {std::string(userId)});
    if (!rows) {
        return Outcome::failure(rows.error());
    }
    std::vector<ApiKey> keys;
    keys.reserve(rows.value().size());
    for (std::size_t row = 0; row < rows.value().size(); ++row) {
        keys.push_back(keyFromRow(rows.value(), row));
    }
    return Outcome::success(std::move(keys));
}

domain::Result<void> ApiKeyRepository::revoke(const std::string_view keyId,
                                              const std::optional<std::string_view> ownerUserId) {
    using Outcome = domain::Result<void>;
    if (!detail::isUuid(keyId) || (ownerUserId && !detail::isUuid(*ownerUserId))) {
        return Outcome::failure(detail::invalid("ids must be uuids"));
    }
    Param owner;
    if (ownerUserId) {
        owner = std::string(*ownerUserId);
    }
    const auto rows = database_->query(
        "UPDATE api_keys SET revoked_at = now() WHERE id = $1::uuid AND revoked_at IS NULL "
        "AND ($2::uuid IS NULL OR user_id = $2::uuid)",
        {std::string(keyId), owner});
    if (!rows) {
        return Outcome::failure(rows.error());
    }
    if (rows.value().affected() == 0) {
        return Outcome::failure(detail::notFound("api key not found"));
    }
    return Outcome::success();
}

domain::Result<std::size_t> ApiKeyRepository::purgeExpired() {
    const auto rows = database_->query("DELETE FROM api_keys WHERE (expires_at IS NOT NULL AND expires_at < now() - "
                                       "interval '7 days') OR (revoked_at IS NOT NULL AND revoked_at < now() - "
                                       "interval '30 days')");
    if (!rows) {
        return domain::Result<std::size_t>::failure(rows.error());
    }
    return domain::Result<std::size_t>::success(rows.value().affected());
}

}
