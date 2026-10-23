#include "persistence/Database.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <thread>

namespace persistence {

namespace {

using domain::Error;
using domain::ErrorKind;
using drogon::orm::DrogonDbException;

bool startsWith(const std::string_view text, const std::string_view prefix) {
    return text.starts_with(prefix);
}

std::string firstLine(const char* message) {
    std::string text(message);
    if (const auto end = text.find('\n'); end != std::string::npos) {
        text.resize(end);
    }
    constexpr std::string_view kPrefix = "ERROR:";
    if (startsWith(text, kPrefix)) {
        text.erase(0, kPrefix.size());
    }
    while (!text.empty() && text.front() == ' ') {
        text.erase(text.begin());
    }
    return text;
}

Error classify(const std::string_view sqlState, std::string message) {
    if (sqlState == "23505") {
        return Error{ErrorKind::AlreadyExists, std::move(message), 0, false};
    }
    if (sqlState == "57014") {
        return Error{ErrorKind::Timeout, std::move(message), 0, true};
    }
    if (sqlState == "40001" || sqlState == "40P01" || startsWith(sqlState, "53")) {
        return Error{ErrorKind::Busy, std::move(message), 0, true};
    }
    if (startsWith(sqlState, "08") || startsWith(sqlState, "57P")) {
        return Error{ErrorKind::Unreachable, std::move(message), 0, true};
    }
    if (startsWith(sqlState, "22") || startsWith(sqlState, "23")) {
        return Error{ErrorKind::InvalidArgument, std::move(message), 0, false};
    }
    return Error{ErrorKind::Internal, std::move(message), 0, false};
}

template <typename Type>
bool is(const DrogonDbException& exception) {
    return dynamic_cast<const Type*>(&exception) != nullptr;
}

Error translate(const DrogonDbException& exception) {
    std::string message = firstLine(exception.base().what());
    if (is<drogon::orm::UniqueViolation>(exception)) {
        return Error{ErrorKind::AlreadyExists, std::move(message), 0, false};
    }
    if (is<drogon::orm::IntegrityConstraintViolation>(exception) || is<drogon::orm::DataException>(exception) ||
        is<drogon::orm::ArgumentError>(exception) || is<drogon::orm::ConversionError>(exception)) {
        return Error{ErrorKind::InvalidArgument, std::move(message), 0, false};
    }
    if (is<drogon::orm::TimeoutError>(exception)) {
        return Error{ErrorKind::Timeout, std::move(message), 0, true};
    }
    if (is<drogon::orm::TransactionRollback>(exception) || is<drogon::orm::InsufficientResources>(exception)) {
        return Error{ErrorKind::Busy, std::move(message), 0, true};
    }
    if (is<drogon::orm::BrokenConnection>(exception)) {
        return Error{ErrorKind::Unreachable, std::move(message), 0, true};
    }
    if (const auto* sql = dynamic_cast<const drogon::orm::SqlError*>(&exception)) {
        return classify(sql->sqlState(), std::move(message));
    }
    return Error{ErrorKind::Internal, std::move(message), 0, false};
}

template <typename Value>
class Slot {
public:
    void set(Value value) {
        if (!done_.exchange(true)) {
            promise_.set_value(std::move(value));
        }
    }

    [[nodiscard]] std::future<Value> future() { return promise_.get_future(); }

private:
    std::promise<Value> promise_;
    std::atomic<bool> done_{false};
};

Error timedOut() {
    return Error{ErrorKind::Timeout, "database did not answer in time", 0, true};
}

std::string withSessionOptions(const DatabaseConfig& config) {
    if (config.connectionString.find("://") != std::string::npos) {
        return config.connectionString;
    }
    return config.connectionString + " options='-c statement_timeout=" +
           std::to_string(config.statementTimeout.count()) + " -c client_min_messages=warning'";
}

}

Session::Session(std::shared_ptr<drogon::orm::DbClient> executor,
                 std::shared_ptr<drogon::orm::Transaction> transaction, const std::chrono::milliseconds timeout) noexcept
    : executor_(std::move(executor)), transaction_(std::move(transaction)), timeout_(timeout) {}

domain::Result<Rows> Session::query(const std::string_view sql, const Params& params) const {
    using Outcome = domain::Result<Rows>;
    const auto slot = std::make_shared<Slot<Outcome>>();
    auto answer = slot->future();

    try {
        {
            auto binder = *executor_ << std::string(sql);
            for (const auto& param : params) {
                if (param.has_value()) {
                    binder << *param;
                } else {
                    binder << nullptr;
                }
            }
            binder >> [slot](const drogon::orm::Result& result) { slot->set(Outcome::success(Rows(result))); };
            binder >> [slot](const DrogonDbException& exception) { slot->set(Outcome::failure(translate(exception))); };
        }
    } catch (const DrogonDbException& exception) {
        return Outcome::failure(translate(exception));
    }

    if (answer.wait_for(timeout_) != std::future_status::ready) {
        return Outcome::failure(timedOut());
    }
    return answer.get();
}

Database::Database(DatabaseConfig config, std::shared_ptr<drogon::orm::DbClient> client)
    : config_(std::move(config)), client_(std::move(client)) {}

Database::~Database() {
    constexpr auto kPatience = std::chrono::seconds(5);
    const auto deadline = std::chrono::steady_clock::now() + kPatience;
    while (client_.use_count() > 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client_.reset();
}

domain::Result<std::shared_ptr<Database>> Database::open(DatabaseConfig config) {
    using Outcome = domain::Result<std::shared_ptr<Database>>;
    if (config.connectionString.empty()) {
        return Outcome::failure(Error{ErrorKind::InvalidArgument, "database connection string is empty", 0, false});
    }
    config.poolSize = std::max<std::size_t>(config.poolSize, 1);

    std::shared_ptr<drogon::orm::DbClient> client;
    try {
        client = drogon::orm::DbClient::newPgClient(withSessionOptions(config), config.poolSize);
    } catch (const std::exception&) {
        return Outcome::failure(Error{ErrorKind::Unreachable, "database client could not be created", 0, true});
    }
    if (!client) {
        return Outcome::failure(Error{ErrorKind::Unsupported, "PostgreSQL support is not available", 0, false});
    }

    std::shared_ptr<Database> database(new Database(std::move(config), std::move(client)));
    const auto probe = database->query("SELECT 1");
    if (!probe) {
        const auto& cause = probe.error();
        const ErrorKind kind = cause.kind == ErrorKind::Timeout ? ErrorKind::Unreachable : cause.kind;
        const std::string message =
            cause.kind == ErrorKind::Timeout ? "database connection failed: no response" : cause.message;
        return Outcome::failure(Error{kind, message, cause.nativeCode, true});
    }
    return Outcome::success(std::move(database));
}

domain::Result<Session> Database::session() {
    return domain::Result<Session>::success(
        Session(client_, nullptr, config_.statementTimeout + config_.acquireTimeout));
}

domain::Result<Rows> Database::query(const std::string_view sql, const Params& params) {
    return Session(client_, nullptr, config_.statementTimeout + config_.acquireTimeout).query(sql, params);
}

domain::Result<Session> Database::beginTransaction() {
    using Outcome = domain::Result<Session>;
    const auto slot = std::make_shared<Slot<std::shared_ptr<drogon::orm::Transaction>>>();
    auto started = slot->future();
    try {
        client_->newTransactionAsync(
            [slot](const std::shared_ptr<drogon::orm::Transaction>& transaction) { slot->set(transaction); });
    } catch (const DrogonDbException& exception) {
        return Outcome::failure(translate(exception));
    }
    const auto timeout = config_.statementTimeout + config_.acquireTimeout;
    if (started.wait_for(timeout) != std::future_status::ready) {
        return Outcome::failure(timedOut());
    }
    auto transaction = started.get();
    if (!transaction) {
        return Outcome::failure(Error{ErrorKind::Unreachable, "database transaction could not be started", 0, true});
    }
    return Outcome::success(Session(transaction, transaction, timeout));
}

domain::Result<void> Database::commit(Session& session) const {
    using Outcome = domain::Result<void>;
    auto transaction = std::move(session.transaction_);
    session.executor_.reset();
    if (!transaction) {
        return Outcome::failure(Error{ErrorKind::InvalidState, "no active transaction", 0, false});
    }
    const auto slot = std::make_shared<Slot<bool>>();
    auto finished = slot->future();
    transaction->setCommitCallback([slot](const bool committed) { slot->set(committed); });
    transaction.reset();
    if (finished.wait_for(config_.statementTimeout + config_.acquireTimeout) != std::future_status::ready) {
        return Outcome::failure(timedOut());
    }
    if (!finished.get()) {
        return Outcome::failure(Error{ErrorKind::Internal, "transaction commit failed", 0, false});
    }
    return Outcome::success();
}

void Database::rollback(Session& session) const noexcept {
    try {
        if (session.transaction_) {
            session.transaction_->rollback();
        }
    } catch (const std::exception&) {
        session.transaction_.reset();
    }
    session.transaction_.reset();
    session.executor_.reset();
}

}
