#pragma once

#include <drogon/drogon.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "domain/Result.hpp"
#include "persistence/ApiKeyRepository.hpp"
#include "persistence/Models.hpp"

namespace api {

enum class Access { Public, Viewer, Operator, Admin };

struct Reply {
    drogon::HttpStatusCode status{drogon::k200OK};
    Json::Value body{Json::objectValue};
};

struct RequestContext {
    drogon::HttpRequestPtr http;
    std::optional<persistence::Principal> principal;
};

using Responder = std::function<void(const drogon::HttpResponsePtr&)>;

[[nodiscard]] Reply ok(Json::Value body, drogon::HttpStatusCode status = drogon::k200OK);
[[nodiscard]] Reply noContent();
[[nodiscard]] Reply failure(const domain::Error& error);
[[nodiscard]] Reply badRequest(const std::string& message);
[[nodiscard]] Reply forbidden(const std::string& message);

class JsonBody {
public:
    [[nodiscard]] static domain::Result<JsonBody> parse(const drogon::HttpRequestPtr& request,
                                                              bool allowEmpty = false);

    [[nodiscard]] std::optional<std::string> text(const char* key);
    [[nodiscard]] std::optional<bool> boolean(const char* key);
    [[nodiscard]] std::optional<long long> integer(const char* key);
    [[nodiscard]] const Json::Value* array(const char* key);
    [[nodiscard]] std::optional<domain::Error> problem() const { return problem_; }

private:
    explicit JsonBody(Json::Value root) : root_(std::move(root)) {}

    void reject(const char* key, const char* expected);

    Json::Value root_;
    std::optional<domain::Error> problem_;
};

[[nodiscard]] drogon::HttpResponsePtr dispatch(const std::shared_ptr<persistence::ApiKeyRepository>& keys,
                                               const drogon::HttpRequestPtr& request, Access access,
                                               const std::function<Reply(const RequestContext&)>& handler);

class Router {
public:
    explicit Router(std::shared_ptr<persistence::ApiKeyRepository> keys) : keys_(std::move(keys)) {}

    template <typename... Path>
    void add(std::vector<drogon::internal::HttpConstraint> methods, const std::string& pattern, const Access access,
             std::function<Reply(const RequestContext&, const Path&...)> handler) const {
        drogon::app().registerHandler(
            pattern,
            [keys = keys_, access, handler = std::move(handler)](const drogon::HttpRequestPtr& request,
                                                                 Responder&& callback, Path... path) {
                const Responder respond = std::move(callback);
                respond(dispatch(keys, request, access,
                                 [&](const RequestContext& context) { return handler(context, path...); }));
            },
            std::move(methods));
    }

private:
    std::shared_ptr<persistence::ApiKeyRepository> keys_;
};

}
