#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "DbCredentialProvider.hpp"
#include "domain/Environment.hpp"
#include "persistence/ApiKeyRepository.hpp"
#include "persistence/DeviceRepository.hpp"
#include "persistence/DriverLog.hpp"
#include "persistence/Migrator.hpp"
#include "persistence/UserRepository.hpp"
#include "persistence/VideoTokenRepository.hpp"
#include "security/SecretBox.hpp"

namespace {

using namespace persistence;
using domain::ErrorKind;

constexpr std::string_view kKey ="MDEyMzQ1Njc4OWFiY2RlZjAxMjM0NTY3ODlhYmNkZWY=";
constexpr std::string_view kPassword = "a-long-enough-password";

class PersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto url = domain::readEnvironment("SURVEILLANCE_TEST_DATABASE_URL");
        if (!url) {
            GTEST_SKIP() << "SURVEILLANCE_TEST_DATABASE_URL is not set";
        }
        if (url->find("_test") == std::string::npos) {
            GTEST_SKIP() << "refusing to run against a database whose name does not contain _test";
        }
        auto opened = Database::open(DatabaseConfig{*url, 8, std::chrono::milliseconds(5000),
                                                    std::chrono::milliseconds(15000)});
        ASSERT_TRUE(opened) << opened.error().message;
        database_ = opened.value();

        const auto migrated = migrate(*database_, SURVEILLANCE_MIGRATIONS_DIR);
        ASSERT_TRUE(migrated) << migrated.error().message;
        ASSERT_TRUE(database_->query("TRUNCATE devices, users CASCADE"));

        devices_ = std::make_unique<DeviceRepository>(database_, security::SecretBox::fromBase64(kKey).value());
        users_ = std::make_unique<UserRepository>(database_);
        keys_ = std::make_unique<ApiKeyRepository>(database_);
        tokens_ = std::make_unique<VideoTokenRepository>(database_);
    }

    Device makeDevice(const std::string& name = "Casa Norte", const std::string& registrationId = "0",
                      const int channels = 4) {
        NewDevice request;
        request.name = name;
        request.registrationId = registrationId;
        request.username = "admin";
        request.password = "dvr-secret-1234";
        request.channelCount = channels;
        auto created = devices_->create(request);
        EXPECT_TRUE(created) << (created ? "" : created.error().message);
        return created ? created.value() : Device{};
    }

    User makeUser(const std::string& name = "operator1", const UserRole role = UserRole::Operator) {
        auto created = users_->create(name, kPassword, role);
        EXPECT_TRUE(created) << (created ? "" : created.error().message);
        return created ? created.value() : User{};
    }

    std::shared_ptr<Database> database_;
    std::unique_ptr<DeviceRepository> devices_;
    std::unique_ptr<UserRepository> users_;
    std::unique_ptr<ApiKeyRepository> keys_;
    std::unique_ptr<VideoTokenRepository> tokens_;
};

TEST_F(PersistenceTest, MigrationsAreIdempotent) {
    const auto again = migrate(*database_, SURVEILLANCE_MIGRATIONS_DIR);
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_TRUE(again.value().applied.empty());
    EXPECT_GE(again.value().alreadyApplied, 1U);
}

TEST_F(PersistenceTest, MigrationWithMissingDirectoryFails) {
    const auto missing = migrate(*database_, "no/such/directory");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().kind, ErrorKind::InvalidArgument);
}

TEST_F(PersistenceTest, CreatedDeviceGetsUuidAndDefaultChannels) {
    const Device device = makeDevice();
    EXPECT_EQ(device.id.size(), 36U);
    EXPECT_EQ(device.channelCount, 4);
    ASSERT_EQ(device.channels.size(), 4U);
    EXPECT_EQ(device.channels[0].name, "Canal 1");
    EXPECT_EQ(device.channels[3].index, 3);
    EXPECT_FALSE(device.lastSeenAt.has_value());

    const auto fetched = devices_->get(device.id);
    ASSERT_TRUE(fetched);
    EXPECT_EQ(fetched.value().registrationId, "0");
    EXPECT_EQ(fetched.value().channels.size(), 4U);
}

TEST_F(PersistenceTest, DeviceIdsAreUniqueAndGeneratedByTheDatabase) {
    const Device first = makeDevice("A", "1");
    const Device second = makeDevice("B", "2");
    EXPECT_NE(first.id, second.id);
}

TEST_F(PersistenceTest, PasswordIsStoredEncryptedAndRecoverable) {
    const Device device = makeDevice();
    const auto raw = database_->query("SELECT dvr_password_encrypted FROM devices WHERE id = $1::uuid", {device.id});
    ASSERT_TRUE(raw);
    const auto stored = raw.value().bytes(0, 0);
    const std::string asText(stored.begin(), stored.end());
    EXPECT_EQ(asText.find("dvr-secret-1234"), std::string::npos);
    EXPECT_GT(stored.size(), 24U);

    const auto credentials = devices_->findCredentials("0");
    ASSERT_TRUE(credentials);
    ASSERT_TRUE(credentials.value().has_value());
    const auto found = credentials.value().value_or(DeviceCredentials{});
    EXPECT_EQ(found.username, "admin");
    EXPECT_EQ(found.password, "dvr-secret-1234");
    EXPECT_EQ(found.deviceId, device.id);
}

TEST_F(PersistenceTest, CredentialsFailWithTheWrongKey) {
    makeDevice();
    DeviceRepository other(database_, security::SecretBox::fromBase64(
                                          "ZmVkY2JhOTg3NjU0MzIxMGZlZGNiYTk4NzY1NDMyMTA=")
                                          .value());
    EXPECT_FALSE(other.findCredentials("0"));
}

TEST_F(PersistenceTest, UnknownAndDisabledDevicesHaveNoCredentials) {
    const Device device = makeDevice();
    const auto unknown = devices_->findCredentials("999");
    ASSERT_TRUE(unknown);
    EXPECT_FALSE(unknown.value().has_value());

    DevicePatch patch;
    patch.enabled = false;
    ASSERT_TRUE(devices_->update(device.id, patch));
    const auto disabled = devices_->findCredentials("0");
    ASSERT_TRUE(disabled);
    EXPECT_FALSE(disabled.value().has_value());
}

TEST_F(PersistenceTest, DuplicateRegistrationIdAndNameAreRejected) {
    makeDevice("Casa Norte", "0");
    NewDevice sameId;
    sameId.name = "Otro";
    sameId.registrationId = "0";
    sameId.username = "admin";
    sameId.password = "x";
    const auto byId = devices_->create(sameId);
    ASSERT_FALSE(byId);
    EXPECT_EQ(byId.error().kind, ErrorKind::AlreadyExists) << byId.error().message;

    NewDevice sameName = sameId;
    sameName.registrationId = "7";
    sameName.name = "casa norte";
    const auto byName = devices_->create(sameName);
    ASSERT_FALSE(byName);
    EXPECT_EQ(byName.error().kind, ErrorKind::AlreadyExists) << byName.error().message;
}

TEST_F(PersistenceTest, FailedCreateLeavesNoPartialRows) {
    NewDevice request;
    request.name = "Con canales repetidos";
    request.registrationId = "5";
    request.username = "admin";
    request.password = "x";
    request.channels = {Channel{0, "A", true}, Channel{0, "B", true}};
    EXPECT_FALSE(devices_->create(request));
    const auto count = database_->query("SELECT (SELECT count(*) FROM devices) + (SELECT count(*) FROM device_channels)");
    ASSERT_TRUE(count);
    EXPECT_EQ(count.value().integer(0, 0), 0);
}

TEST_F(PersistenceTest, ValidationRejectsBadInput) {
    NewDevice request;
    request.name = "   ";
    request.registrationId = "1";
    request.username = "admin";
    request.password = "x";
    EXPECT_EQ(devices_->create(request).error().kind, ErrorKind::InvalidArgument);
    request.name = "ok";
    request.channelCount = 9999;
    EXPECT_EQ(devices_->create(request).error().kind, ErrorKind::InvalidArgument);
    request.channelCount = 1;
    request.password.clear();
    EXPECT_EQ(devices_->create(request).error().kind, ErrorKind::InvalidArgument);
    EXPECT_EQ(devices_->get("not-a-uuid").error().kind, ErrorKind::InvalidArgument);
}

TEST_F(PersistenceTest, SqlInjectionAttemptsAreTreatedAsData) {
    const std::string hostile = "x'); DROP TABLE devices; --";
    NewDevice request;
    request.name = hostile;
    request.registrationId = hostile;
    request.username = hostile;
    request.password = hostile;
    const auto created = devices_->create(request);
    ASSERT_TRUE(created);
    EXPECT_EQ(created.value().name, hostile);
    EXPECT_TRUE(devices_->list());
}

TEST_F(PersistenceTest, UpdateChangesOnlyProvidedFields) {
    const Device device = makeDevice();
    DevicePatch patch;
    patch.name = "Casa Sur";
    patch.password = "new-dvr-password";
    const auto updated = devices_->update(device.id, patch);
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated.value().name, "Casa Sur");
    EXPECT_EQ(updated.value().username, "admin");
    EXPECT_EQ(updated.value().registrationId, "0");
    EXPECT_GE(updated.value().updatedAt, device.updatedAt);

    const auto credentials = devices_->findCredentials("0");
    ASSERT_TRUE(credentials);
    EXPECT_EQ(credentials.value().value_or(DeviceCredentials{}).password, "new-dvr-password");

    EXPECT_EQ(devices_->update("00000000-0000-0000-0000-000000000000", patch).error().kind, ErrorKind::NotFound);
}

TEST_F(PersistenceTest, ReplaceChannelsRewritesTheSet) {
    const Device device = makeDevice();
    const std::vector<Channel> channels{Channel{0, "Entrada", true}, Channel{5, "Patio", false}};
    ASSERT_TRUE(devices_->replaceChannels(device.id, channels));
    const auto fetched = devices_->get(device.id);
    ASSERT_TRUE(fetched);
    ASSERT_EQ(fetched.value().channels.size(), 2U);
    EXPECT_EQ(fetched.value().channels[1].index, 5);
    EXPECT_FALSE(fetched.value().channels[1].enabled);
    EXPECT_EQ(fetched.value().channelCount, 6);
    EXPECT_FALSE(devices_->replaceChannels(device.id, {Channel{0, "A", true}, Channel{0, "B", true}}));
    EXPECT_EQ(devices_->get(device.id).value().channels.size(), 2U);
}

TEST_F(PersistenceTest, RecordSeenUpdatesDeviceAndCreatesMissingChannels) {
    const Device device = makeDevice("Sin canales", "3", 0);
    EXPECT_TRUE(device.channels.empty());
    ASSERT_TRUE(devices_->recordSeen("3", "9A01B2C3", 8));
    const auto fetched = devices_->get(device.id);
    ASSERT_TRUE(fetched);
    EXPECT_EQ(fetched.value().channelCount, 8);
    EXPECT_EQ(fetched.value().channels.size(), 8U);
    EXPECT_EQ(fetched.value().serialNumber, std::optional<std::string>("9A01B2C3"));
    EXPECT_TRUE(fetched.value().lastSeenAt.has_value());
    ASSERT_TRUE(devices_->recordSeen("3", "", 8));
    EXPECT_EQ(devices_->get(device.id).value().channels.size(), 8U);
    EXPECT_EQ(devices_->get(device.id).value().serialNumber, std::optional<std::string>("9A01B2C3"));
    EXPECT_EQ(devices_->recordSeen("404", "", 1).error().kind, ErrorKind::NotFound);
}

TEST_F(PersistenceTest, RemoveCascadesToChannelsAndTokens) {
    const Device device = makeDevice();
    const User user = makeUser();
    ASSERT_TRUE(tokens_->issue(VideoTokenRequest{user.id, device.id, 0, 0, std::chrono::seconds(60)}));
    ASSERT_TRUE(devices_->remove(device.id));
    EXPECT_EQ(devices_->remove(device.id).error().kind, ErrorKind::NotFound);
    const auto leftovers = database_->query("SELECT (SELECT count(*) FROM device_channels) + "
                                            "(SELECT count(*) FROM video_tokens)");
    ASSERT_TRUE(leftovers);
    EXPECT_EQ(leftovers.value().integer(0, 0), 0);
}

TEST_F(PersistenceTest, ListIsSortedByNameWithChannels) {
    makeDevice("Zeta", "1", 1);
    makeDevice("alfa", "2", 2);
    const auto listed = devices_->list();
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 2U);
    EXPECT_EQ(listed.value()[0].name, "alfa");
    EXPECT_EQ(listed.value()[0].channels.size(), 2U);
    EXPECT_EQ(listed.value()[1].channels.size(), 1U);
}

TEST_F(PersistenceTest, UsersAuthenticateWithTheirPassword) {
    const User user = makeUser("Operator.One");
    EXPECT_EQ(user.role, UserRole::Operator);
    const auto good = users_->authenticate("operator.one", kPassword);
    ASSERT_TRUE(good);
    EXPECT_EQ(good.value().id, user.id);
    const auto bad = users_->authenticate("operator.one", "wrong-password-value");
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().kind, ErrorKind::Unauthenticated);
    const auto missing = users_->authenticate("nobody", kPassword);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().kind, ErrorKind::Unauthenticated);
    EXPECT_EQ(missing.error().message, bad.error().message);
}

TEST_F(PersistenceTest, PasswordsAreNeverStoredInClear) {
    const User user = makeUser();
    const auto raw = database_->query("SELECT password_hash FROM users WHERE id = $1::uuid", {user.id});
    ASSERT_TRUE(raw);
    EXPECT_EQ(raw.value().text(0, 0).find(std::string(kPassword)), std::string::npos);
    EXPECT_EQ(raw.value().text(0, 0).rfind("$argon2id$", 0), 0U);
}

TEST_F(PersistenceTest, UsernamesAreUniqueIgnoringCase) {
    makeUser("Operator");
    const auto duplicate = users_->create("OPERATOR", kPassword, UserRole::Viewer);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().kind, ErrorKind::AlreadyExists);
    EXPECT_EQ(users_->create("a b", kPassword, UserRole::Viewer).error().kind, ErrorKind::InvalidArgument);
    EXPECT_EQ(users_->create("valid.name", "short", UserRole::Viewer).error().kind, ErrorKind::InvalidArgument);
}

TEST_F(PersistenceTest, AccountLocksAfterRepeatedFailures) {
    const User user = makeUser();
    for (int attempt = 0; attempt < kMaxFailedLogins; ++attempt) {
        EXPECT_EQ(users_->authenticate("operator1", "wrong-password-value").error().kind, ErrorKind::Unauthenticated);
    }
    const auto locked = users_->authenticate("operator1", kPassword);
    ASSERT_FALSE(locked);
    EXPECT_EQ(locked.error().kind, ErrorKind::AccountLocked);

    UserPatch unlock;
    unlock.password = "another-long-password";
    ASSERT_TRUE(users_->update(user.id, unlock));
    EXPECT_TRUE(users_->authenticate("operator1", "another-long-password"));
}

TEST_F(PersistenceTest, DisabledUsersCannotAuthenticateOrGetKeys) {
    const User user = makeUser();
    UserPatch patch;
    patch.disabled = true;
    ASSERT_TRUE(users_->update(user.id, patch));
    EXPECT_FALSE(users_->authenticate("operator1", kPassword));
    EXPECT_EQ(keys_->issue(user.id, "k", ApiKeyKind::Service, std::nullopt).error().kind, ErrorKind::NotFound);
}

TEST_F(PersistenceTest, RoleAndUserLifecycle) {
    const User user = makeUser();
    UserPatch patch;
    patch.role = UserRole::Admin;
    const auto promoted = users_->update(user.id, patch);
    ASSERT_TRUE(promoted);
    EXPECT_EQ(promoted.value().role, UserRole::Admin);
    EXPECT_EQ(users_->count().value(), 1);
    ASSERT_TRUE(users_->remove(user.id));
    EXPECT_EQ(users_->get(user.id).error().kind, ErrorKind::NotFound);
    EXPECT_EQ(users_->count().value(), 0);
}

TEST_F(PersistenceTest, ApiKeysAuthenticateAndStoreOnlyTheHash) {
    const User user = makeUser("admin.user", UserRole::Admin);
    const auto issued = keys_->issue(user.id, "ci", ApiKeyKind::Service, std::nullopt);
    ASSERT_TRUE(issued);
    EXPECT_EQ(issued.value().token.rfind("key_", 0), 0U);

    const auto raw = database_->query("SELECT count(*) FROM api_keys WHERE position($1::bytea in key_hash) > 0",
                                      {"\\x" + security::toHex(security::Bytes(
                                                   issued.value().token.begin(), issued.value().token.end()))});
    ASSERT_TRUE(raw);
    EXPECT_EQ(raw.value().integer(0, 0), 0);

    const auto principal = keys_->authenticate(issued.value().token);
    ASSERT_TRUE(principal);
    EXPECT_EQ(principal.value().userId, user.id);
    EXPECT_EQ(principal.value().role, UserRole::Admin);
    EXPECT_FALSE(keys_->authenticate(issued.value().token + "x"));
    EXPECT_FALSE(keys_->authenticate(""));
}

TEST_F(PersistenceTest, ApiKeysCanBeRevokedAndExpire) {
    const User user = makeUser();
    const auto revocable = keys_->issue(user.id, "one", ApiKeyKind::Service, std::nullopt);
    ASSERT_TRUE(revocable);
    ASSERT_TRUE(keys_->revoke(revocable.value().key.id, std::string_view(user.id)));
    EXPECT_FALSE(keys_->authenticate(revocable.value().token));
    EXPECT_EQ(keys_->revoke(revocable.value().key.id, std::nullopt).error().kind, ErrorKind::NotFound);

    const auto expiring = keys_->issue(user.id, "two", ApiKeyKind::Session, std::chrono::seconds(3600));
    ASSERT_TRUE(expiring);
    EXPECT_EQ(expiring.value().token.rfind("ses_", 0), 0U);
    ASSERT_TRUE(keys_->authenticate(expiring.value().token));
    ASSERT_TRUE(database_->query("UPDATE api_keys SET expires_at = now() - interval '1 second' WHERE id = $1::uuid",
                                 {expiring.value().key.id}));
    EXPECT_FALSE(keys_->authenticate(expiring.value().token));
}

TEST_F(PersistenceTest, KeysCannotBeRevokedByAnotherUser) {
    const User owner = makeUser("owner.user");
    const User intruder = makeUser("intruder.user");
    const auto issued = keys_->issue(owner.id, "mine", ApiKeyKind::Service, std::nullopt);
    ASSERT_TRUE(issued);
    EXPECT_EQ(keys_->revoke(issued.value().key.id, std::string_view(intruder.id)).error().kind, ErrorKind::NotFound);
    EXPECT_TRUE(keys_->authenticate(issued.value().token));
    const auto listed = keys_->list(owner.id);
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed.value().size(), 1U);
    EXPECT_TRUE(keys_->list(intruder.id).value().empty());
}

TEST_F(PersistenceTest, SessionKeysRequireALifetime) {
    const User user = makeUser();
    EXPECT_EQ(keys_->issue(user.id, "s", ApiKeyKind::Session, std::nullopt).error().kind, ErrorKind::InvalidArgument);
    EXPECT_EQ(keys_->issue(user.id, "s", ApiKeyKind::Service, std::chrono::seconds(0)).error().kind,
              ErrorKind::InvalidArgument);
}

TEST_F(PersistenceTest, VideoTokensBindToTheFirstViewer) {
    const Device device = makeDevice();
    const User user = makeUser();
    const auto issued = tokens_->issue(VideoTokenRequest{user.id, device.id, 2, 1, std::chrono::seconds(60)});
    ASSERT_TRUE(issued);
    EXPECT_EQ(issued.value().token.rfind("vtk_", 0), 0U);
    EXPECT_GT(issued.value().expiresAt, std::chrono::system_clock::now());

    const auto first = tokens_->consume(issued.value().token, "10.0.0.5");
    ASSERT_TRUE(first);
    EXPECT_EQ(first.value().deviceId, device.id);
    EXPECT_EQ(first.value().registrationId, "0");
    EXPECT_EQ(first.value().channel, 2);
    EXPECT_EQ(first.value().profile, 1);
    EXPECT_EQ(first.value().userId, user.id);

    EXPECT_GT(first.value().sessionEndsAt, std::chrono::system_clock::now() + std::chrono::hours(7));

    const auto sameViewer = tokens_->consume(issued.value().token, "10.0.0.5");
    ASSERT_TRUE(sameViewer);
    EXPECT_EQ(sameViewer.value().channel, 2);
    EXPECT_EQ(sameViewer.value().sessionEndsAt, first.value().sessionEndsAt);

    const auto otherViewer = tokens_->consume(issued.value().token, "10.0.0.6");
    ASSERT_FALSE(otherViewer);
    EXPECT_EQ(otherViewer.error().kind, ErrorKind::Unauthenticated);
    EXPECT_EQ(tokens_->consume(issued.value().token, "").error().kind, ErrorKind::InvalidArgument);
}

TEST_F(PersistenceTest, VideoTokensExpire) {
    const Device device = makeDevice();
    const User user = makeUser();
    const auto issued = tokens_->issue(VideoTokenRequest{user.id, device.id, 0, 0, std::chrono::seconds(30)});
    ASSERT_TRUE(issued);
    ASSERT_TRUE(database_->query("UPDATE video_tokens SET expires_at = now() - interval '1 second', "
                                 "created_at = now() - interval '1 minute'"));
    EXPECT_FALSE(tokens_->consume(issued.value().token, "10.0.0.5"));
    const auto purged = tokens_->purgeExpired();
    ASSERT_TRUE(purged);
}

TEST_F(PersistenceTest, VideoTokensStoreOnlyTheHash) {
    const Device device = makeDevice();
    const User user = makeUser();
    const auto issued = tokens_->issue(VideoTokenRequest{user.id, device.id, 0, 0, std::chrono::seconds(60)});
    ASSERT_TRUE(issued);
    const auto columns = database_->query("SELECT row_to_json(t)::text FROM video_tokens t");
    ASSERT_TRUE(columns);
    EXPECT_EQ(columns.value().text(0, 0).find(issued.value().token), std::string::npos);
}

TEST_F(PersistenceTest, VideoTokenRejectsInvalidTargets) {
    const Device device = makeDevice();
    const User user = makeUser();
    EXPECT_EQ(tokens_->issue(VideoTokenRequest{user.id, device.id, 99, 0, std::chrono::seconds(60)}).error().kind,
              ErrorKind::NotFound);
    EXPECT_EQ(tokens_->issue(VideoTokenRequest{user.id, device.id, 0, 9, std::chrono::seconds(60)}).error().kind,
              ErrorKind::InvalidArgument);
    EXPECT_EQ(tokens_->issue(VideoTokenRequest{user.id, device.id, 0, 0, std::chrono::seconds(1)}).error().kind,
              ErrorKind::InvalidArgument);
    EXPECT_EQ(tokens_->issue(VideoTokenRequest{user.id, device.id, 0, 0, std::chrono::seconds(3600)}).error().kind,
              ErrorKind::InvalidArgument);
    EXPECT_EQ(tokens_->issue(VideoTokenRequest{user.id, "bad", 0, 0, std::chrono::seconds(60)}).error().kind,
              ErrorKind::InvalidArgument);

    DevicePatch patch;
    patch.enabled = false;
    ASSERT_TRUE(devices_->update(device.id, patch));
    EXPECT_EQ(tokens_->issue(VideoTokenRequest{user.id, device.id, 0, 0, std::chrono::seconds(60)}).error().kind,
              ErrorKind::NotFound);
}

TEST_F(PersistenceTest, TokenIssuedBeforeDeviceWasDisabledIsRejected) {
    const Device device = makeDevice();
    const User user = makeUser();
    const auto issued = tokens_->issue(VideoTokenRequest{user.id, device.id, 0, 0, std::chrono::seconds(60)});
    ASSERT_TRUE(issued);
    DevicePatch patch;
    patch.enabled = false;
    ASSERT_TRUE(devices_->update(device.id, patch));
    EXPECT_FALSE(tokens_->consume(issued.value().token, "10.0.0.5"));
}

TEST_F(PersistenceTest, ConcurrentRedemptionsYieldExactlyOneWinner) {
    const Device device = makeDevice();
    const User user = makeUser();
    const auto issued = tokens_->issue(VideoTokenRequest{user.id, device.id, 0, 0, std::chrono::seconds(60)});
    ASSERT_TRUE(issued);

    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int index = 0; index < 8; ++index) {
        threads.emplace_back([&, index] {
            if (tokens_->consume(issued.value().token, "10.0.0." + std::to_string(index))) {
                ++winners;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(winners.load(), 1);
}

TEST_F(PersistenceTest, PoolSurvivesMoreWorkersThanConnections) {
    makeDevice();
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(24);
    for (int index = 0; index < 24; ++index) {
        threads.emplace_back([&] {
            for (int round = 0; round < 10; ++round) {
                if (!devices_->list()) {
                    ++failures;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
}

TEST_F(PersistenceTest, TransactionRollsBackOnFailure) {
    const auto outcome = database_->transact([](Session& session) -> domain::Result<void> {
        const auto inserted = session.query("INSERT INTO users (username, password_hash) VALUES ('temp.user', 'x')");
        if (!inserted) {
            return domain::Result<void>::failure(inserted.error());
        }
        return domain::Result<void>::failure(
            domain::Error{ErrorKind::Internal, "forced failure", 0, false});
    });
    ASSERT_FALSE(outcome);
    EXPECT_EQ(users_->count().value(), 0);
}

TEST_F(PersistenceTest, CredentialProviderServesDatabaseCredentialsAndDetectsChanges) {
    const Device device = makeDevice();
    const gateway::DbCredentialProvider provider(
        std::make_shared<DeviceRepository>(database_, security::SecretBox::fromBase64(kKey).value()));
    const dahua::DeviceId id{"0"};

    EXPECT_FALSE(provider.changedSinceLastUse(id));
    const auto credentials = provider.find(id).value_or(dahua::Credentials{});
    EXPECT_EQ(credentials.username(), "admin");
    EXPECT_EQ(credentials.password(), "dvr-secret-1234");
    EXPECT_FALSE(provider.changedSinceLastUse(id));

    DevicePatch patch;
    patch.password = "rotated-dvr-password";
    ASSERT_TRUE(devices_->update(device.id, patch));
    EXPECT_TRUE(provider.changedSinceLastUse(id));
    EXPECT_EQ(provider.find(id).value_or(dahua::Credentials{}).password(), "rotated-dvr-password");
    EXPECT_FALSE(provider.changedSinceLastUse(id));
}

TEST_F(PersistenceTest, CredentialProviderHandlesUnknownThenRegisteredDevices) {
    const gateway::DbCredentialProvider provider(
        std::make_shared<DeviceRepository>(database_, security::SecretBox::fromBase64(kKey).value()));
    const dahua::DeviceId id{"0"};
    EXPECT_FALSE(provider.find(id).has_value());
    EXPECT_FALSE(provider.changedSinceLastUse(id));
    makeDevice();
    EXPECT_TRUE(provider.changedSinceLastUse(id));
    EXPECT_TRUE(provider.find(id).has_value());
}

TEST(DatabaseOpenTest, UnreachableServerReportsRetryableError) {
    const auto opened = Database::open(
        DatabaseConfig{"host=127.0.0.1 port=1 dbname=none connect_timeout=2", 2, std::chrono::milliseconds(500),
                       std::chrono::milliseconds(1000)});
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().kind, ErrorKind::Unreachable);
    EXPECT_TRUE(opened.error().retryable);
    EXPECT_FALSE(Database::open(DatabaseConfig{}));
}

}

namespace {

TEST(DriverLogTest, RemovesRowDetailsButKeepsTheErrorAndSource) {
    const std::string raw =
        "20261023 20:00:00 UTC 1 ERROR ERROR:  duplicate key value violates unique constraint \"users_username_key\"\n"
        "DETAIL:  Key (lower(username))=(operator) already exists.\n"
        " - PgBatchConnection.cc:535\n";
    const std::string cleaned = persistence::withoutRowDetails(raw);
    EXPECT_EQ(cleaned.find("operator) already"), std::string::npos);
    EXPECT_EQ(cleaned.find("DETAIL"), std::string::npos);
    EXPECT_NE(cleaned.find("users_username_key"), std::string::npos);
    EXPECT_NE(cleaned.find("PgBatchConnection.cc:535"), std::string::npos);
}

TEST(DriverLogTest, LeavesOtherMessagesUntouched) {
    const std::string plain = "20261023 20:00:00 UTC 1 INFO connected\n";
    EXPECT_EQ(persistence::withoutRowDetails(plain), plain);
    EXPECT_EQ(persistence::withoutRowDetails("x\nDETAIL: secret"), "x");
}

}

namespace {

TEST_F(PersistenceTest, ViewingSessionEndsAfterTheConfiguredLimit) {
    const Device device = makeDevice();
    const User user = makeUser();
    const auto issued = tokens_->issue(VideoTokenRequest{user.id, device.id, 0, 0, std::chrono::seconds(60)});
    ASSERT_TRUE(issued);
    ASSERT_TRUE(tokens_->consume(issued.value().token, "10.0.0.5"));
    ASSERT_TRUE(database_->query("UPDATE video_tokens SET consumed_at = now() - interval '9 hours'"));
    EXPECT_FALSE(tokens_->consume(issued.value().token, "10.0.0.5"));
}

TEST_F(PersistenceTest, DisabledDeviceStopsAnOngoingViewingSession) {
    const Device device = makeDevice();
    const User user = makeUser();
    const auto issued = tokens_->issue(VideoTokenRequest{user.id, device.id, 0, 0, std::chrono::seconds(60)});
    ASSERT_TRUE(issued);
    ASSERT_TRUE(tokens_->consume(issued.value().token, "10.0.0.5"));
    DevicePatch patch;
    patch.enabled = false;
    ASSERT_TRUE(devices_->update(device.id, patch));
    EXPECT_FALSE(tokens_->consume(issued.value().token, "10.0.0.5"));
}

}
