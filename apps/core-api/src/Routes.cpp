#include "api/Routes.hpp"

#include <charconv>
#include <chrono>
#include <stdexcept>

#include <algorithm>

#include "Http.hpp"
#include "Serialization.hpp"
#include "security/LoginThrottle.hpp"

namespace api {

namespace {

using drogon::Delete;
using drogon::Get;
using drogon::Post;
using drogon::Put;
using domain::Error;
using domain::ErrorKind;
using namespace persistence;

using Handler0 = std::function<Reply(const RequestContext&)>;
using Handler1 = std::function<Reply(const RequestContext&, const std::string&)>;
using Handler2 = std::function<Reply(const RequestContext&, const std::string&, const std::string&)>;

constexpr std::chrono::hours kSessionLifetime{12};
constexpr long long kMaxServiceKeyTtlSeconds = 60LL * 60 * 24 * 365;
constexpr int kProfileMain = 0;
constexpr std::size_t kMaxLoginFailuresPerAddress = 10;
constexpr std::chrono::seconds kLoginFailureWindow{300};

bool isLoopback(const std::string& address) { return address == "127.0.0.1" || address == "::1"; }

std::string queryValue(const std::string& query, const std::string& name) {
    const std::string needle = name + "=";
    std::size_t start = 0;
    while (start <= query.size()) {
        const std::size_t end = std::min(query.find('&', start), query.size());
        if (query.compare(start, needle.size(), needle) == 0) {
            return query.substr(start + needle.size(), end - start - needle.size());
        }
        start = end + 1;
    }
    return {};
}

const Principal& caller(const RequestContext& context) {
    if (!context.principal) {
        throw std::logic_error("request is not authenticated");
    }
    return *context.principal;
}

bool isAdmin(const RequestContext& context) { return caller(context).role == UserRole::Admin; }

Reply invalid(std::string message) { return failure(Error{ErrorKind::InvalidArgument, std::move(message), 0, false}); }

std::optional<int> parseChannel(const std::string& text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 0 || value >= kMaxChannels) {
        return std::nullopt;
    }
    return value;
}

domain::Result<std::vector<Channel>> parseChannels(const Json::Value* json) {
    using Outcome = domain::Result<std::vector<Channel>>;
    std::vector<Channel> channels;
    if (json == nullptr) {
        return Outcome::success(std::move(channels));
    }
    for (const auto& item : *json) {
        if (!item.isObject() || !item["index"].isInt() || !item["name"].isString() ||
            (item.isMember("enabled") && !item["enabled"].isBool())) {
            return Outcome::failure(
                Error{ErrorKind::InvalidArgument, "channels must be objects with index, name and optional enabled", 0,
                      false});
        }
        channels.push_back(Channel{item["index"].asInt(), item["name"].asString(),
                                   item.isMember("enabled") ? item["enabled"].asBool() : true});
    }
    return Outcome::success(std::move(channels));
}

void registerAuthRoutes(const Router& router, const Services& services) {
    const auto throttle = std::make_shared<security::LoginThrottle>(kMaxLoginFailuresPerAddress, kLoginFailureWindow);

    router.add({Post}, "/api/v1/auth/login", Access::Public,
               Handler0([users = services.users, keys = services.keys, throttle](const RequestContext& context) {
                   const std::string client = context.http->peerAddr().toIp();
                   if (throttle->blocked(client)) {
                       return failure(Error{ErrorKind::AccountLocked, "too many failed attempts", 0, true});
                   }
                   auto body = JsonBody::parse(context.http);
                   if (!body) {
                       return failure(body.error());
                   }
                   const auto username = body.value().text("username");
                   const auto password = body.value().text("password");
                   if (const auto problem = body.value().problem()) {
                       return failure(*problem);
                   }
                   if (!username || !password) {
                       return invalid("username and password are required");
                   }
                   const auto user = users->authenticate(*username, *password);
                   if (!user) {
                       if (user.error().kind == ErrorKind::Unauthenticated ||
                           user.error().kind == ErrorKind::AccountLocked) {
                           throttle->recordFailure(client);
                       }
                       return failure(user.error());
                   }
                   throttle->recordSuccess(client);
                   const auto issued = keys->issue(user.value().id, "session", ApiKeyKind::Session,
                                                   std::chrono::duration_cast<std::chrono::seconds>(kSessionLifetime));
                   if (!issued) {
                       return failure(issued.error());
                   }
                   Json::Value result(Json::objectValue);
                   result["token"] = issued.value().token;
                   result["token_type"] = "Bearer";
                   result["expires_at"] = toIso(*issued.value().key.expiresAt);
                   result["user"] = toJson(user.value());
                   return ok(std::move(result));
               }));

    router.add({Post}, "/api/v1/auth/logout", Access::Viewer,
               Handler0([keys = services.keys](const RequestContext& context) {
                   const auto revoked = keys->revoke(caller(context).apiKeyId, std::string_view(caller(context).userId));
                   if (!revoked) {
                       return failure(revoked.error());
                   }
                   return noContent();
               }));

    router.add({Get}, "/api/v1/auth/me", Access::Viewer,
               Handler0([users = services.users](const RequestContext& context) {
                   const auto user = users->get(caller(context).userId);
                   if (!user) {
                       return failure(user.error());
                   }
                   return ok(toJson(user.value()));
               }));
}

void registerDeviceRoutes(const Router& router, const Services& services) {
    router.add({Get}, "/api/v1/devices", Access::Viewer,
               Handler0([devices = services.devices](const RequestContext&) {
                   const auto listed = devices->list();
                   if (!listed) {
                       return failure(listed.error());
                   }
                   Json::Value items(Json::arrayValue);
                   for (const auto& device : listed.value()) {
                       items.append(toJson(device));
                   }
                   Json::Value result(Json::objectValue);
                   result["total"] = static_cast<Json::UInt64>(listed.value().size());
                   result["devices"] = std::move(items);
                   return ok(std::move(result));
               }));

    router.add({Post}, "/api/v1/devices", Access::Admin,
               Handler0([devices = services.devices](const RequestContext& context) {
                   auto body = JsonBody::parse(context.http);
                   if (!body) {
                       return failure(body.error());
                   }
                   NewDevice request;
                   request.name = body.value().text("name").value_or("");
                   request.registrationId = body.value().text("registration_id").value_or("");
                   request.username = body.value().text("username").value_or("");
                   request.password = body.value().text("password").value_or("");
                   request.channelCount = static_cast<int>(body.value().integer("channel_count").value_or(0));
                   request.enabled = body.value().boolean("enabled").value_or(true);
                   auto channels = parseChannels(body.value().array("channels"));
                   if (const auto problem = body.value().problem()) {
                       return failure(*problem);
                   }
                   if (!channels) {
                       return failure(channels.error());
                   }
                   request.channels = std::move(channels).value();
                   const auto created = devices->create(request);
                   if (!created) {
                       return failure(created.error());
                   }
                   return ok(toJson(created.value()), drogon::k201Created);
               }));

    router.add({Get}, "/api/v1/devices/{1}", Access::Viewer,
               Handler1([devices = services.devices](const RequestContext&, const std::string& id) {
                   const auto device = devices->get(id);
                   if (!device) {
                       return failure(device.error());
                   }
                   return ok(toJson(device.value()));
               }));

    router.add({drogon::Patch}, "/api/v1/devices/{1}", Access::Admin,
               Handler1([devices = services.devices](const RequestContext& context, const std::string& id) {
                   auto body = JsonBody::parse(context.http);
                   if (!body) {
                       return failure(body.error());
                   }
                   DevicePatch patch;
                   patch.name = body.value().text("name");
                   patch.registrationId = body.value().text("registration_id");
                   patch.username = body.value().text("username");
                   patch.password = body.value().text("password");
                   patch.enabled = body.value().boolean("enabled");
                   if (const auto problem = body.value().problem()) {
                       return failure(*problem);
                   }
                   const auto updated = devices->update(id, patch);
                   if (!updated) {
                       return failure(updated.error());
                   }
                   return ok(toJson(updated.value()));
               }));

    router.add({Put}, "/api/v1/devices/{1}/channels", Access::Admin,
               Handler1([devices = services.devices](const RequestContext& context, const std::string& id) {
                   auto body = JsonBody::parse(context.http);
                   if (!body) {
                       return failure(body.error());
                   }
                   const Json::Value* array = body.value().array("channels");
                   if (const auto problem = body.value().problem()) {
                       return failure(*problem);
                   }
                   if (array == nullptr) {
                       return invalid("channels is required");
                   }
                   const auto channels = parseChannels(array);
                   if (!channels) {
                       return failure(channels.error());
                   }
                   if (const auto replaced = devices->replaceChannels(id, channels.value()); !replaced) {
                       return failure(replaced.error());
                   }
                   const auto device = devices->get(id);
                   if (!device) {
                       return failure(device.error());
                   }
                   return ok(toJson(device.value()));
               }));

    router.add({Delete}, "/api/v1/devices/{1}", Access::Admin,
               Handler1([devices = services.devices](const RequestContext&, const std::string& id) {
                   const auto removed = devices->remove(id);
                   if (!removed) {
                       return failure(removed.error());
                   }
                   return noContent();
               }));
}

void registerUserRoutes(const Router& router, const Services& services) {
    router.add({Get}, "/api/v1/users", Access::Admin, Handler0([users = services.users](const RequestContext&) {
                   const auto listed = users->list();
                   if (!listed) {
                       return failure(listed.error());
                   }
                   Json::Value items(Json::arrayValue);
                   for (const auto& user : listed.value()) {
                       items.append(toJson(user));
                   }
                   Json::Value result(Json::objectValue);
                   result["users"] = std::move(items);
                   return ok(std::move(result));
               }));

    router.add({Post}, "/api/v1/users", Access::Admin,
               Handler0([users = services.users](const RequestContext& context) {
                   auto body = JsonBody::parse(context.http);
                   if (!body) {
                       return failure(body.error());
                   }
                   const auto username = body.value().text("username");
                   const auto password = body.value().text("password");
                   const auto roleText = body.value().text("role");
                   if (const auto problem = body.value().problem()) {
                       return failure(*problem);
                   }
                   if (!username || !password) {
                       return invalid("username and password are required");
                   }
                   const auto role = parseUserRole(roleText.value_or("viewer"));
                   if (!role) {
                       return invalid("role must be viewer, operator or admin");
                   }
                   const auto created = users->create(*username, *password, *role);
                   if (!created) {
                       return failure(created.error());
                   }
                   return ok(toJson(created.value()), drogon::k201Created);
               }));

    router.add({drogon::Patch}, "/api/v1/users/{1}", Access::Admin,
               Handler1([users = services.users](const RequestContext& context, const std::string& id) {
                   auto body = JsonBody::parse(context.http);
                   if (!body) {
                       return failure(body.error());
                   }
                   UserPatch patch;
                   if (const auto roleText = body.value().text("role")) {
                       patch.role = parseUserRole(*roleText);
                       if (!patch.role) {
                           return invalid("role must be viewer, operator or admin");
                       }
                   }
                   patch.disabled = body.value().boolean("disabled");
                   patch.password = body.value().text("password");
                   if (const auto problem = body.value().problem()) {
                       return failure(*problem);
                   }
                   if (id == caller(context).userId && (patch.disabled == true ||
                                                        (patch.role && *patch.role != UserRole::Admin))) {
                       return forbidden("administrators cannot disable or demote themselves");
                   }
                   const auto updated = users->update(id, patch);
                   if (!updated) {
                       return failure(updated.error());
                   }
                   return ok(toJson(updated.value()));
               }));

    router.add({Delete}, "/api/v1/users/{1}", Access::Admin,
               Handler1([users = services.users](const RequestContext& context, const std::string& id) {
                   if (id == caller(context).userId) {
                       return forbidden("administrators cannot delete themselves");
                   }
                   const auto removed = users->remove(id);
                   if (!removed) {
                       return failure(removed.error());
                   }
                   return noContent();
               }));
}

void registerKeyRoutes(const Router& router, const Services& services) {
    router.add({Get}, "/api/v1/api-keys", Access::Viewer,
               Handler0([keys = services.keys](const RequestContext& context) {
                   std::string owner = caller(context).userId;
                   if (const std::string requested = context.http->getParameter("user_id"); !requested.empty()) {
                       if (requested != owner && !isAdmin(context)) {
                           return forbidden("only administrators can list keys of other users");
                       }
                       owner = requested;
                   }
                   const auto listed = keys->list(owner);
                   if (!listed) {
                       return failure(listed.error());
                   }
                   Json::Value items(Json::arrayValue);
                   for (const auto& key : listed.value()) {
                       items.append(toJson(key));
                   }
                   Json::Value result(Json::objectValue);
                   result["api_keys"] = std::move(items);
                   return ok(std::move(result));
               }));

    router.add({Post}, "/api/v1/api-keys", Access::Operator,
               Handler0([keys = services.keys](const RequestContext& context) {
                   auto body = JsonBody::parse(context.http);
                   if (!body) {
                       return failure(body.error());
                   }
                   const auto name = body.value().text("name");
                   const auto userId = body.value().text("user_id");
                   const auto ttl = body.value().integer("ttl_seconds");
                   if (const auto problem = body.value().problem()) {
                       return failure(*problem);
                   }
                   if (!name) {
                       return invalid("name is required");
                   }
                   if (userId && *userId != caller(context).userId && !isAdmin(context)) {
                       return forbidden("only administrators can create keys for other users");
                   }
                   if (ttl && (*ttl <= 0 || *ttl > kMaxServiceKeyTtlSeconds)) {
                       return invalid("ttl_seconds must be between 1 and 31536000");
                   }
                   std::optional<std::chrono::seconds> lifetime;
                   if (ttl) {
                       lifetime = std::chrono::seconds(*ttl);
                   }
                   const auto issued =
                       keys->issue(userId.value_or(caller(context).userId), *name, ApiKeyKind::Service, lifetime);
                   if (!issued) {
                       return failure(issued.error());
                   }
                   Json::Value result = toJson(issued.value().key);
                   result["token"] = issued.value().token;
                   return ok(std::move(result), drogon::k201Created);
               }));

    router.add({Delete}, "/api/v1/api-keys/{1}", Access::Viewer,
               Handler1([keys = services.keys](const RequestContext& context, const std::string& id) {
                   std::optional<std::string_view> owner;
                   if (!isAdmin(context)) {
                       owner = std::string_view(caller(context).userId);
                   }
                   const auto revoked = keys->revoke(id, owner);
                   if (!revoked) {
                       return failure(revoked.error());
                   }
                   return noContent();
               }));
}

void registerVideoRoutes(const Router& router, const Services& services) {
    router.add({Post}, "/api/v1/devices/{1}/channels/{2}/video-tokens", Access::Viewer,
               Handler2([tokens = services.tokens](const RequestContext& context, const std::string& deviceId,
                                                   const std::string& channelText) {
                   const auto channel = parseChannel(channelText);
                   if (!channel) {
                       return invalid("channel must be a non-negative integer");
                   }
                   auto body = JsonBody::parse(context.http, true);
                   if (!body) {
                       return failure(body.error());
                   }
                   const auto profile = body.value().integer("profile");
                   const auto ttl = body.value().integer("ttl_seconds");
                   if (const auto problem = body.value().problem()) {
                       return failure(*problem);
                   }
                   VideoTokenRequest request;
                   request.userId = caller(context).userId;
                   request.deviceId = deviceId;
                   request.channel = *channel;
                   request.profile = static_cast<int>(profile.value_or(kProfileMain));
                   if (ttl) {
                       request.ttl = std::chrono::seconds(*ttl);
                   }
                   const auto issued = tokens->issue(request);
                   if (!issued) {
                       return failure(issued.error());
                   }
                   Json::Value result(Json::objectValue);
                   result["id"] = issued.value().id;
                   result["token"] = issued.value().token;
                   result["expires_at"] = toIso(issued.value().expiresAt);
                   return ok(std::move(result), drogon::k201Created);
               }));

    router.add({Post}, "/api/v1/media/auth", Access::Public,
               Handler0([tokens = services.tokens, keys = services.keys](const RequestContext& context) {
                   const auto mediaKey = keys->authenticate(context.http->getParameter("key"));
                   if (!mediaKey || !atLeast(mediaKey.value().role, UserRole::Operator)) {
                       return failure(Error{ErrorKind::Unauthenticated, "invalid media server key", 0, false});
                   }
                   auto body = JsonBody::parse(context.http);
                   if (!body) {
                       return failure(body.error());
                   }
                   const std::string action = body.value().text("action").value_or("");
                   const std::string path = body.value().text("path").value_or("");
                   const std::string address = body.value().text("ip").value_or("");
                   std::string token = body.value().text("token").value_or("");
                   if (token.empty()) {
                       token = body.value().text("password").value_or("");
                   }
                   if (token.empty()) {
                       token = queryValue(body.value().text("query").value_or(""), "token");
                   }
                   if (const auto problem = body.value().problem()) {
                       return failure(*problem);
                   }
                   if (action == "publish") {
                       if (!isLoopback(address)) {
                           return forbidden("publishing is only allowed from the local host");
                       }
                       return noContent();
                   }
                   if (action != "read" && action != "playback") {
                       return forbidden("action not allowed");
                   }
                   if (token.empty()) {
                       return failure(Error{ErrorKind::Unauthenticated, "token is required", 0, false});
                   }
                   const auto grant = tokens->consume(token, address);
                   if (!grant) {
                       return failure(grant.error());
                   }
                   if (path != grant.value().registrationId + "/" + std::to_string(grant.value().channel)) {
                       return forbidden("token does not grant this stream");
                   }
                   Json::Value session(Json::objectValue);
                   session["expireAt"] = toIso(grant.value().sessionEndsAt);
                   return ok(std::move(session));
               }));

}

}

void registerRoutes(const Services& services) {
    const Router router(services.keys);

    router.add({Get}, "/api/v1/health", Access::Public, Handler0([](const RequestContext&) {
                   Json::Value result(Json::objectValue);
                   result["status"] = "ok";
                   result["service"] = "surveillance-core";
                   result["version"] = "0.1.0";
                   return ok(std::move(result));
               }));

    registerAuthRoutes(router, services);
    registerDeviceRoutes(router, services);
    registerUserRoutes(router, services);
    registerKeyRoutes(router, services);
    registerVideoRoutes(router, services);
}

}
