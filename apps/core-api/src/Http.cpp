#include "Http.hpp"

#include <algorithm>
#include <cctype>
#include <memory>

#include "telemetry/Logger.hpp"

namespace api {

namespace {

using domain::Error;
using domain::ErrorKind;
using persistence::UserRole;

constexpr std::string_view kBearer = "Bearer ";

drogon::HttpStatusCode statusFor(const ErrorKind kind) {
    switch (kind) {
        case ErrorKind::InvalidArgument: return drogon::k400BadRequest;
        case ErrorKind::Unauthenticated: return drogon::k401Unauthorized;
        case ErrorKind::Forbidden: return drogon::k403Forbidden;
        case ErrorKind::NotFound: return drogon::k404NotFound;
        case ErrorKind::AlreadyExists:
        case ErrorKind::InvalidState: return drogon::k409Conflict;
        case ErrorKind::AccountLocked: return drogon::k429TooManyRequests;
        case ErrorKind::Busy:
        case ErrorKind::Unreachable: return drogon::k503ServiceUnavailable;
        case ErrorKind::Timeout: return drogon::k504GatewayTimeout;
        case ErrorKind::Unsupported: return drogon::k501NotImplemented;
        case ErrorKind::Protocol: return drogon::k502BadGateway;
        case ErrorKind::Internal: return drogon::k500InternalServerError;
    }
    return drogon::k500InternalServerError;
}

bool exposesMessage(const ErrorKind kind) {
    return kind != ErrorKind::Internal && kind != ErrorKind::Unreachable && kind != ErrorKind::Busy &&
           kind != ErrorKind::Timeout && kind != ErrorKind::Protocol;
}

Json::Value errorBody(const std::string_view code, const std::string& message) {
    Json::Value error(Json::objectValue);
    error["code"] = std::string(code);
    error["message"] = message;
    Json::Value body(Json::objectValue);
    body["error"] = std::move(error);
    return body;
}

std::optional<std::string> bearerToken(const drogon::HttpRequestPtr& request) {
    const std::string header = request->getHeader("authorization");
    if (header.size() <= kBearer.size() ||
        !std::equal(kBearer.begin(), kBearer.end(), header.begin(), [](const char expected, const char actual) {
            return std::tolower(static_cast<unsigned char>(expected)) == std::tolower(static_cast<unsigned char>(actual));
        })) {
        return std::nullopt;
    }
    return header.substr(kBearer.size());
}

drogon::HttpResponsePtr render(const Reply& reply) {
    auto response = reply.status == drogon::k204NoContent ? drogon::HttpResponse::newHttpResponse()
                                                          : drogon::HttpResponse::newHttpJsonResponse(reply.body);
    response->setStatusCode(reply.status);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("X-Content-Type-Options", "nosniff");
    if (reply.status == drogon::k401Unauthorized) {
        response->addHeader("WWW-Authenticate", "Bearer");
    }
    return response;
}

bool allows(const UserRole role, const Access access) {
    switch (access) {
        case Access::Public:
        case Access::Viewer: return true;
        case Access::Operator: return persistence::atLeast(role, UserRole::Operator);
        case Access::Admin: return persistence::atLeast(role, UserRole::Admin);
    }
    return false;
}

}

Reply ok(Json::Value body, const drogon::HttpStatusCode status) { return Reply{status, std::move(body)}; }

Reply noContent() { return Reply{drogon::k204NoContent, Json::Value(Json::objectValue)}; }

Reply failure(const Error& error) {
    const std::string message = exposesMessage(error.kind) ? error.message : "request could not be completed";
    return Reply{statusFor(error.kind), errorBody(domain::toString(error.kind), message)};
}

Reply badRequest(const std::string& message) {
    return Reply{drogon::k400BadRequest, errorBody("invalid_argument", message)};
}

Reply forbidden(const std::string& message) { return Reply{drogon::k403Forbidden, errorBody("forbidden", message)}; }

domain::Result<JsonBody> JsonBody::parse(const drogon::HttpRequestPtr& request, const bool allowEmpty) {
    using Outcome = domain::Result<JsonBody>;
    const std::string_view raw = request->body();
    if (raw.empty() && allowEmpty) {
        return Outcome::success(JsonBody(Json::Value(Json::objectValue)));
    }
    Json::Value root;
    Json::CharReaderBuilder builder;
    builder["allowComments"] = false;
    builder["failIfExtra"] = true;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(raw.data(), raw.data() + raw.size(), &root, &errors) || !root.isObject()) {
        return Outcome::failure(Error{ErrorKind::InvalidArgument, "request body must be a JSON object", 0, false});
    }
    return Outcome::success(JsonBody(std::move(root)));
}

void JsonBody::reject(const char* key, const char* expected) {
    if (!problem_) {
        problem_ = Error{ErrorKind::InvalidArgument, std::string(key) + " must be " + expected, 0, false};
    }
}

std::optional<std::string> JsonBody::text(const char* key) {
    if (!root_.isMember(key) || root_[key].isNull()) {
        return std::nullopt;
    }
    if (!root_[key].isString()) {
        reject(key, "a string");
        return std::nullopt;
    }
    return root_[key].asString();
}

std::optional<bool> JsonBody::boolean(const char* key) {
    if (!root_.isMember(key) || root_[key].isNull()) {
        return std::nullopt;
    }
    if (!root_[key].isBool()) {
        reject(key, "a boolean");
        return std::nullopt;
    }
    return root_[key].asBool();
}

std::optional<long long> JsonBody::integer(const char* key) {
    if (!root_.isMember(key) || root_[key].isNull()) {
        return std::nullopt;
    }
    if (!root_[key].isInt64()) {
        reject(key, "an integer");
        return std::nullopt;
    }
    return static_cast<long long>(root_[key].asInt64());
}

const Json::Value* JsonBody::array(const char* key) {
    if (!root_.isMember(key) || root_[key].isNull()) {
        return nullptr;
    }
    if (!root_[key].isArray()) {
        reject(key, "an array");
        return nullptr;
    }
    return &root_[key];
}

drogon::HttpResponsePtr dispatch(const std::shared_ptr<persistence::ApiKeyRepository>& keys,
                                 const drogon::HttpRequestPtr& request, const Access access,
                                 const std::function<Reply(const RequestContext&)>& handler) {
    try {
        RequestContext context{request, std::nullopt};
        if (access != Access::Public) {
            const auto token = bearerToken(request);
            if (!token) {
                return render(failure(Error{ErrorKind::Unauthenticated, "missing bearer token", 0, false}));
            }
            auto principal = keys->authenticate(*token);
            if (!principal) {
                if (principal.error().kind == ErrorKind::Unauthenticated) {
                    return render(failure(Error{ErrorKind::Unauthenticated, "invalid or expired token", 0, false}));
                }
                telemetry::Logger::global().error("api.auth_backend_failed",
                                                        {{"reason", principal.error().message}});
                return render(failure(principal.error()));
            }
            if (!allows(principal.value().role, access)) {
                return render(forbidden("insufficient role for this operation"));
            }
            context.principal = std::move(principal).value();
        }
        const Reply reply = handler(context);
        if (reply.status == drogon::k500InternalServerError) {
            telemetry::Logger::global().error("api.internal_error", {{"path", std::string(request->path())}});
        }
        return render(reply);
    } catch (const std::exception&) {
        telemetry::Logger::global().error("api.handler_exception", {{"path", std::string(request->path())}});
        return render(failure(Error{ErrorKind::Internal, "internal error", 0, false}));
    }
}

}
