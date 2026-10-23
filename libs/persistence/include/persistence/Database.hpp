#pragma once

#include <drogon/orm/DbClient.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "domain/Result.hpp"
#include "security/Crypto.hpp"

namespace persistence {

using Param = std::optional<std::string>;
using Params = std::vector<Param>;

class Rows {
public:
    Rows() = default;
    explicit Rows(drogon::orm::Result result)
        : result_(std::make_shared<const drogon::orm::Result>(std::move(result))) {}

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t affected() const noexcept;
    [[nodiscard]] bool isNull(std::size_t row, std::size_t column) const noexcept;
    [[nodiscard]] std::string text(std::size_t row, std::size_t column) const;
    [[nodiscard]] std::optional<std::string> optionalText(std::size_t row, std::size_t column) const;
    [[nodiscard]] std::int64_t integer(std::size_t row, std::size_t column) const;
    [[nodiscard]] bool boolean(std::size_t row, std::size_t column) const;
    [[nodiscard]] security::Bytes bytes(std::size_t row, std::size_t column) const;
    [[nodiscard]] std::chrono::system_clock::time_point time(std::size_t row, std::size_t column) const;
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> optionalTime(std::size_t row,
                                                                                    std::size_t column) const;

private:
    std::shared_ptr<const drogon::orm::Result> result_;
};

struct DatabaseConfig {
    std::string connectionString;
    std::size_t poolSize{8};
    std::chrono::milliseconds acquireTimeout{5000};
    std::chrono::milliseconds statementTimeout{15000};
};

class Session {
public:
    Session(std::shared_ptr<drogon::orm::DbClient> executor, std::shared_ptr<drogon::orm::Transaction> transaction,
            std::chrono::milliseconds timeout) noexcept;

    [[nodiscard]] domain::Result<Rows> query(std::string_view sql, const Params& params = {}) const;
    [[nodiscard]] domain::Result<Rows> execute(std::string_view sql) const { return query(sql); }

private:
    friend class Database;

    std::shared_ptr<drogon::orm::DbClient> executor_;
    std::shared_ptr<drogon::orm::Transaction> transaction_;
    std::chrono::milliseconds timeout_;
};

class Database {
public:
    [[nodiscard]] static domain::Result<std::shared_ptr<Database>> open(DatabaseConfig config);

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;
    ~Database();

    [[nodiscard]] domain::Result<Session> session();

    [[nodiscard]] domain::Result<Rows> query(std::string_view sql, const Params& params = {});

    template <typename Function>
    [[nodiscard]] auto transact(Function&& function) -> std::invoke_result_t<Function, Session&> {
        using Outcome = std::invoke_result_t<Function, Session&>;
        auto begun = beginTransaction();
        if (!begun) {
            return Outcome::failure(begun.error());
        }
        auto& active = begun.value();
        auto outcome = std::forward<Function>(function)(active);
        if (!outcome) {
            rollback(active);
            return outcome;
        }
        if (const auto committed = commit(active); !committed) {
            return Outcome::failure(committed.error());
        }
        return outcome;
    }

    [[nodiscard]] const DatabaseConfig& config() const noexcept { return config_; }

private:
    explicit Database(DatabaseConfig config, std::shared_ptr<drogon::orm::DbClient> client);

    [[nodiscard]] domain::Result<Session> beginTransaction();
    [[nodiscard]] domain::Result<void> commit(Session& session) const;
    void rollback(Session& session) const noexcept;

    DatabaseConfig config_;
    std::shared_ptr<drogon::orm::DbClient> client_;
};

}
