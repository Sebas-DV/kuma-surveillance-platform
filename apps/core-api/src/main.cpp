#include <drogon/drogon.h>

#include <charconv>
#include <cstdio>
#include <iostream>

#include "api/Routes.hpp"
#include "domain/Environment.hpp"
#include "persistence/Database.hpp"
#include "persistence/DriverLog.hpp"
#include "persistence/Migrator.hpp"
#include "security/SecretBox.hpp"
#include "telemetry/Logger.hpp"

namespace {

using domain::Error;
using domain::ErrorKind;
using domain::readEnvironment;

constexpr std::size_t kMaxRequestBody = std::size_t{64} * 1024;
constexpr std::size_t kIdleSessionsSeconds = 60;

domain::Result<api::Services> buildServices() {
    using Outcome = domain::Result<api::Services>;

    const auto url = readEnvironment("SURVEILLANCE_DATABASE_URL");
    const auto encodedKey = readEnvironment("SURVEILLANCE_SECRET_KEY");
    if (!url || url->empty() || !encodedKey) {
        return Outcome::failure(
            Error{ErrorKind::InvalidArgument, "SURVEILLANCE_DATABASE_URL and SURVEILLANCE_SECRET_KEY must be set", 0, false});
    }
    auto secretBox = security::SecretBox::fromBase64(*encodedKey);
    if (!secretBox) {
        return Outcome::failure(secretBox.error());
    }
    auto database = persistence::Database::open(persistence::DatabaseConfig{*url, 8});
    if (!database) {
        return Outcome::failure(database.error());
    }
    if (const auto directory = readEnvironment("SURVEILLANCE_MIGRATIONS_DIR")) {
        const auto report = persistence::migrate(*database.value(), *directory);
        if (!report) {
            return Outcome::failure(report.error());
        }
        telemetry::Logger::global().info("database.migrated",
                                         {{"applied", std::to_string(report.value().applied.size())},
                                          {"already_applied", std::to_string(report.value().alreadyApplied)}});
    }

    api::Services services;
    services.devices = std::make_shared<persistence::DeviceRepository>(database.value(), std::move(secretBox).value());
    services.users = std::make_shared<persistence::UserRepository>(database.value());
    services.keys = std::make_shared<persistence::ApiKeyRepository>(database.value());
    services.tokens = std::make_shared<persistence::VideoTokenRepository>(database.value());
    return Outcome::success(std::move(services));
}

domain::Result<void> bootstrapAdministrator(persistence::UserRepository& users) {
    using Outcome = domain::Result<void>;
    const auto existing = users.count();
    if (!existing) {
        return Outcome::failure(existing.error());
    }
    if (existing.value() > 0) {
        return Outcome::success();
    }
    const auto username = readEnvironment("SURVEILLANCE_BOOTSTRAP_ADMIN_USER");
    const auto password = readEnvironment("SURVEILLANCE_BOOTSTRAP_ADMIN_PASSWORD");
    if (!username || !password) {
        telemetry::Logger::global().warn(
            "api.no_users", {{"hint", "set SURVEILLANCE_BOOTSTRAP_ADMIN_USER and SURVEILLANCE_BOOTSTRAP_ADMIN_PASSWORD"}});
        return Outcome::success();
    }
    const auto created = users.create(*username, *password, persistence::UserRole::Admin);
    if (!created) {
        return Outcome::failure(created.error());
    }
    telemetry::Logger::global().info("api.admin_bootstrapped", {{"username", created.value().username}});
    return Outcome::success();
}

std::uint16_t listenPort() {
    int port = 8080;
    if (const auto text = readEnvironment("SURVEILLANCE_API_PORT")) {
        const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), port);
        if (error != std::errc{} || end != text->data() + text->size() || port < 1 || port > 65535) {
            throw std::invalid_argument("SURVEILLANCE_API_PORT must be between 1 and 65535");
        }
    }
    return static_cast<std::uint16_t>(port);
}

int run() {
    persistence::sanitizeDriverLogs();
    auto services = buildServices();
    if (!services) {
        telemetry::Logger::global().error("api.startup_failed", {{"reason", services.error().message}});
        return 1;
    }
    if (const auto bootstrapped = bootstrapAdministrator(*services.value().users); !bootstrapped) {
        telemetry::Logger::global().error("api.bootstrap_failed", {{"reason", bootstrapped.error().message}});
        return 1;
    }

    api::registerRoutes(services.value());

    const std::string bind = readEnvironment("SURVEILLANCE_API_BIND").value_or("127.0.0.1");
    const std::uint16_t port = listenPort();
    telemetry::Logger::global().info("api.starting", {{"bind", bind}, {"port", std::to_string(port)}});

    drogon::app()
        .addListener(bind, port)
        .setThreadNum(4)
        .setClientMaxBodySize(kMaxRequestBody)
        .setIdleConnectionTimeout(std::chrono::seconds(kIdleSessionsSeconds))
        .run();
    return 0;
}

}

int main() {
    try {
        return run();
    } catch (const std::exception& exception) {
        static_cast<void>(std::fputs("fatal: ", stderr));
        static_cast<void>(std::fputs(exception.what(), stderr));
        static_cast<void>(std::fputc('\n', stderr));
        return 1;
    }
}
