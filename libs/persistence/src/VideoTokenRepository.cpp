#include "persistence/VideoTokenRepository.hpp"

#include "detail/Sql.hpp"
#include "security/Crypto.hpp"

namespace persistence {

namespace {

constexpr std::string_view kTokenPrefix = "vtk_";
constexpr std::size_t kMaxAddressLength = 64;

VideoGrant grantFromRow(const Rows& rows) {
    VideoGrant grant;
    grant.deviceId = rows.text(0, 0);
    grant.registrationId = rows.text(0, 1);
    grant.channel = static_cast<int>(rows.integer(0, 2));
    grant.profile = static_cast<int>(rows.integer(0, 3));
    grant.userId = rows.text(0, 4);
    grant.sessionEndsAt = rows.time(0, 5);
    return grant;
}

}

VideoTokenRepository::VideoTokenRepository(std::shared_ptr<Database> database) : database_(std::move(database)) {}

domain::Result<IssuedVideoToken> VideoTokenRepository::issue(const VideoTokenRequest& request) {
    using Outcome = domain::Result<IssuedVideoToken>;
    if (!detail::isUuid(request.userId) || !detail::isUuid(request.deviceId)) {
        return Outcome::failure(detail::invalid("user and device ids must be uuids"));
    }
    if (request.channel < 0 || request.channel >= kMaxChannels) {
        return Outcome::failure(detail::invalid("channel out of range"));
    }
    if (request.profile < 0 || request.profile > 3) {
        return Outcome::failure(detail::invalid("profile out of range"));
    }
    if (request.ttl < kMinVideoTokenTtl || request.ttl > kMaxVideoTokenTtl) {
        return Outcome::failure(detail::invalid("token lifetime must be between " +
                                                std::to_string(kMinVideoTokenTtl.count()) + " and " +
                                                std::to_string(kMaxVideoTokenTtl.count()) + " seconds"));
    }

    auto token = security::generateToken(kTokenPrefix);
    if (!token) {
        return Outcome::failure(token.error());
    }
    const std::string hash = "\\x" + security::toHex(security::hashToken(token.value()));

    const auto rows = database_->query(
        "INSERT INTO video_tokens (token_hash, user_id, device_id, channel_index, profile, expires_at) "
        "SELECT $1::bytea, $2::uuid, d.id, c.channel_index, $4::smallint, now() + make_interval(secs => $5::bigint) "
        "FROM devices d JOIN device_channels c ON c.device_id = d.id "
        "WHERE d.id = $3::uuid AND d.enabled AND c.channel_index = $6::int AND c.enabled "
        "RETURNING id::text, " +
            detail::epochMillis("expires_at"),
        {hash, request.userId, request.deviceId, std::to_string(request.profile), std::to_string(request.ttl.count()),
         std::to_string(request.channel)});
    if (!rows) {
        return Outcome::failure(rows.error());
    }
    if (rows.value().size() == 0) {
        return Outcome::failure(detail::notFound("device or channel not found or disabled"));
    }
    return Outcome::success(
        IssuedVideoToken{rows.value().text(0, 0), std::move(token).value(), rows.value().time(0, 1)});
}

domain::Result<VideoGrant> VideoTokenRepository::consume(const std::string_view token,
                                                         const std::string_view clientAddress) {
    using Outcome = domain::Result<VideoGrant>;
    if (token.size() < 16 || token.size() > 128) {
        return Outcome::failure(detail::invalidCredentials());
    }
    if (detail::isBlank(clientAddress) || clientAddress.size() > kMaxAddressLength) {
        return Outcome::failure(detail::invalid("client address must be between 1 and 64 characters"));
    }
    const std::string hash = "\\x" + security::toHex(security::hashToken(token));
    const std::string sessionEnd = detail::epochMillis("t.consumed_at + make_interval(hours => $3::int)");
    const Params params{hash, std::string(clientAddress), std::to_string(kViewingSession.count())};

    const auto first = database_->query(
        "UPDATE video_tokens t SET consumed_at = now(), consumed_address = $2 FROM devices d "
        "WHERE t.token_hash = $1::bytea AND t.consumed_at IS NULL AND t.expires_at > now() "
        "AND d.id = t.device_id AND d.enabled "
        "RETURNING t.device_id::text, d.registration_id, t.channel_index, t.profile, t.user_id::text, " +
            sessionEnd,
        params);
    if (!first) {
        return Outcome::failure(first.error());
    }
    if (first.value().size() > 0) {
        return Outcome::success(grantFromRow(first.value()));
    }

    const auto again = database_->query(
        "SELECT t.device_id::text, d.registration_id, t.channel_index, t.profile, t.user_id::text, " + sessionEnd +
            " FROM video_tokens t JOIN devices d ON d.id = t.device_id "
            "WHERE t.token_hash = $1::bytea AND t.consumed_address = $2 "
            "AND t.consumed_at + make_interval(hours => $3::int) > now() AND d.enabled",
        params);
    if (!again) {
        return Outcome::failure(again.error());
    }
    if (again.value().size() == 0) {
        return Outcome::failure(domain::Error{domain::ErrorKind::Unauthenticated,
                                              "token is invalid, expired or already used", 0, false});
    }
    return Outcome::success(grantFromRow(again.value()));
}

domain::Result<std::size_t> VideoTokenRepository::purgeExpired() {
    const auto rows = database_->query("DELETE FROM video_tokens WHERE expires_at < now() - interval '1 day' "
                                       "OR (consumed_at IS NOT NULL AND consumed_at < now() - interval '1 day')");
    if (!rows) {
        return domain::Result<std::size_t>::failure(rows.error());
    }
    return domain::Result<std::size_t>::success(rows.value().affected());
}

}
