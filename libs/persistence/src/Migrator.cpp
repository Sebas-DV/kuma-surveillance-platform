#include "persistence/Migrator.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#include "detail/Sql.hpp"
#include "security/Crypto.hpp"

namespace persistence {

namespace {

constexpr std::string_view kAdvisoryLockKey = "727274";

struct MigrationFile {
    std::string version;
    std::string checksum;
    std::string sql;
};

domain::Result<std::vector<MigrationFile>> loadFiles(const std::filesystem::path& directory) {
    using Outcome = domain::Result<std::vector<MigrationFile>>;
    std::error_code failure;
    if (!std::filesystem::is_directory(directory, failure)) {
        return Outcome::failure(detail::invalid("migrations directory not found: " + directory.string()));
    }
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(directory, failure)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sql") {
            paths.push_back(entry.path());
        }
    }
    if (failure) {
        return Outcome::failure(detail::invalid("cannot read migrations directory: " + failure.message()));
    }
    std::ranges::sort(paths);

    std::vector<MigrationFile> files;
    for (const auto& path : paths) {
        const std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            return Outcome::failure(detail::invalid("cannot read migration " + path.filename().string()));
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        MigrationFile file;
        file.version = path.stem().string();
        file.sql = buffer.str();
        file.checksum = security::toHex(security::sha256(file.sql));
        files.push_back(std::move(file));
    }
    return Outcome::success(std::move(files));
}

std::vector<std::string> splitStatements(const std::string& script) {
    std::vector<std::string> statements;
    std::string current;
    bool quoted = false;
    for (std::size_t index = 0; index < script.size(); ++index) {
        const char symbol = script[index];
        if (!quoted && symbol == '-' && index + 1 < script.size() && script[index + 1] == '-') {
            while (index < script.size() && script[index] != '\n') {
                ++index;
            }
            current.push_back('\n');
            continue;
        }
        if (symbol == '\'') {
            quoted = !quoted;
        }
        if (symbol == ';' && !quoted) {
            statements.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(symbol);
    }
    statements.push_back(std::move(current));
    std::erase_if(statements, [](const std::string& statement) { return detail::isBlank(statement); });
    return statements;
}

domain::Result<MigrationReport> applyAll(const Session& session, const std::vector<MigrationFile>& files) {
    using Outcome = domain::Result<MigrationReport>;
    if (const auto locked = session.query("SELECT pg_advisory_xact_lock($1::bigint)", {std::string(kAdvisoryLockKey)});
        !locked) {
        return Outcome::failure(locked.error());
    }
    if (const auto created = session.execute("CREATE TABLE IF NOT EXISTS schema_migrations ("
                                             "version text PRIMARY KEY, checksum text NOT NULL, "
                                             "applied_at timestamptz NOT NULL DEFAULT now())");
        !created) {
        return Outcome::failure(created.error());
    }
    const auto existing = session.query("SELECT version, checksum FROM schema_migrations");
    if (!existing) {
        return Outcome::failure(existing.error());
    }
    std::map<std::string, std::string> applied;
    for (std::size_t row = 0; row < existing.value().size(); ++row) {
        applied.emplace(existing.value().text(row, 0), existing.value().text(row, 1));
    }

    MigrationReport report;
    for (const auto& file : files) {
        if (const auto found = applied.find(file.version); found != applied.end()) {
            if (found->second != file.checksum) {
                return Outcome::failure(domain::Error{domain::ErrorKind::InvalidState,
                                                      "migration " + file.version + " was modified after being applied",
                                                      0, false});
            }
            ++report.alreadyApplied;
            continue;
        }
        for (const auto& statement : splitStatements(file.sql)) {
            if (const auto executed = session.execute(statement); !executed) {
                const auto& cause = executed.error();
                return Outcome::failure(domain::Error{cause.kind, "migration " + file.version + " failed: " +
                                                                      cause.message,
                                                      cause.nativeCode, cause.retryable});
            }
        }
        if (const auto recorded = session.query("INSERT INTO schema_migrations (version, checksum) VALUES ($1, $2)",
                                                {file.version, file.checksum});
            !recorded) {
            return Outcome::failure(recorded.error());
        }
        report.applied.push_back(file.version);
    }
    return Outcome::success(std::move(report));
}

}

domain::Result<MigrationReport> migrate(Database& database, const std::filesystem::path& directory) {
    auto files = loadFiles(directory);
    if (!files) {
        return domain::Result<MigrationReport>::failure(files.error());
    }
    return database.transact(
        [&](Session& session) -> domain::Result<MigrationReport> { return applyAll(session, files.value()); });
}

}
