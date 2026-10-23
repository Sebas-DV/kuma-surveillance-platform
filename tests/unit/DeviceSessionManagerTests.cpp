#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "dahua/DeviceSessionManager.hpp"

namespace {

using namespace std::chrono_literals;
using namespace dahua;
using domain::Error;
using domain::ErrorKind;
using domain::Result;

class FakeSession final : public IDeviceSession {
public:
    FakeSession(DeviceId id, const std::uint64_t token, std::atomic<int>& alive)
        : id_(std::move(id)), token_(token), alive_(&alive) {
        info_.channelCount = 8;
        info_.serialNumber = "SERIAL";
        alive_->fetch_add(1);
    }

    FakeSession(const FakeSession&) = delete;
    FakeSession& operator=(const FakeSession&) = delete;
    FakeSession(FakeSession&&) = delete;
    FakeSession& operator=(FakeSession&&) = delete;

    ~FakeSession() override { alive_->fetch_sub(1); }

    [[nodiscard]] const DeviceId& id() const noexcept override { return id_; }
    [[nodiscard]] const DeviceInfo& info() const noexcept override { return info_; }
    [[nodiscard]] std::uint64_t connectionToken() const noexcept override { return token_; }

    [[nodiscard]] Result<std::shared_ptr<ILiveStream>> openLiveStream(const LiveStreamRequest&, PacketHandler) override {
        return Result<std::shared_ptr<ILiveStream>>::failure(Error{ErrorKind::Unsupported, "fake", 0, false});
    }

    [[nodiscard]] Result<std::shared_ptr<ITalkSession>> openTalk(const TalkRequest&) override {
        return Result<std::shared_ptr<ITalkSession>>::failure(Error{ErrorKind::Unsupported, "fake", 0, false});
    }

private:
    DeviceId id_;
    DeviceInfo info_;
    std::uint64_t token_;
    std::atomic<int>* alive_;
};

class FakeFactory final : public ISessionFactory {
public:
    void script(std::vector<std::optional<Error>> outcomes) {
        const std::scoped_lock lock(mutex_);
        outcomes_.assign(outcomes.begin(), outcomes.end());
    }

    [[nodiscard]] Result<std::unique_ptr<IDeviceSession>> connect(const RegistrationEvent& event,
                                                                  const Credentials&) override {
        const std::scoped_lock lock(mutex_);
        calls_.push_back(event);
        if (!outcomes_.empty()) {
            auto outcome = std::move(outcomes_.front());
            outcomes_.pop_front();
            if (outcome.has_value()) {
                return Result<std::unique_ptr<IDeviceSession>>::failure(*outcome);
            }
        }
        return Result<std::unique_ptr<IDeviceSession>>::success(
            std::make_unique<FakeSession>(event.deviceId, nextToken_++, alive));
    }

    [[nodiscard]] std::size_t calls() const {
        const std::scoped_lock lock(mutex_);
        return calls_.size();
    }

    std::atomic<int> alive{0};

private:
    mutable std::mutex mutex_;
    std::deque<std::optional<Error>> outcomes_;
    std::vector<RegistrationEvent> calls_;
    std::uint64_t nextToken_{100};
};

class FixedCredentials final : public ICredentialProvider {
public:
    explicit FixedCredentials(const bool available) : available_(available) {}

    [[nodiscard]] std::optional<Credentials> find(const DeviceId&) const override {
        return available_ ? std::optional<Credentials>(Credentials("admin", "secret")) : std::nullopt;
    }

private:
    bool available_;
};

RegistrationEvent registered(const std::string& id, const std::uint16_t port = 40000) {
    RegistrationEvent event;
    event.kind = RegistrationEventKind::Registered;
    event.deviceId = DeviceId{id};
    event.remoteAddress = "192.168.100.62";
    event.remotePort = port;
    event.receivedAt = std::chrono::system_clock::now();
    return event;
}

bool waitFor(const std::function<bool()>& predicate, const std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

class DeviceSessionManagerTest : public ::testing::Test {
protected:
    void build(const bool credentialsAvailable = true) {
        factory = std::make_shared<FakeFactory>();
        DeviceSessionManagerConfig config;
        config.loginPolicy.initialDelay = 20ms;
        config.loginPolicy.maxDelay = 80ms;
        manager = std::make_unique<DeviceSessionManager>(factory, std::make_shared<FixedCredentials>(credentialsAvailable),
                                                         config, quiet);
        manager->start();
    }

    [[nodiscard]] DeviceSnapshot snapshotOf(const std::string& id) const {
        return manager->find(DeviceId{id}).value_or(DeviceSnapshot{});
    }

    [[nodiscard]] SessionState stateOf(const std::string& id) const {
        const auto snapshot = manager->find(DeviceId{id});
        return snapshot.has_value() ? snapshot->state : SessionState::Offline;
    }

    telemetry::Logger quiet{[](const std::string&) {}};
    std::shared_ptr<FakeFactory> factory;
    std::unique_ptr<DeviceSessionManager> manager;
};

TEST_F(DeviceSessionManagerTest, RegisteredDeviceBecomesOnline) {
    build();
    manager->handleRegistration(registered("DVR-1"));

    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Online; }));
    const auto snapshot = snapshotOf("DVR-1");
    EXPECT_EQ(snapshot.state, SessionState::Online);
    EXPECT_EQ(snapshot.info.value_or(DeviceInfo{}).channelCount, 8);
    EXPECT_EQ(snapshot.failedAttempts, 0U);
    EXPECT_NE(manager->session(DeviceId{"DVR-1"}), nullptr);
    EXPECT_EQ(factory->calls(), 1U);
}

TEST_F(DeviceSessionManagerTest, WrongPasswordBlocksWithoutFurtherAttempts) {
    build();
    factory->script({Error{ErrorKind::Unauthenticated, "wrong password", 1, false}});

    manager->handleRegistration(registered("DVR-1", 40000));
    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Blocked; }));

    manager->handleRegistration(registered("DVR-1", 40001));
    manager->handleRegistration(registered("DVR-1", 40002));
    std::this_thread::sleep_for(150ms);

    EXPECT_EQ(factory->calls(), 1U);
    EXPECT_EQ(stateOf("DVR-1"), SessionState::Blocked);
    EXPECT_EQ(manager->session(DeviceId{"DVR-1"}), nullptr);
}

TEST_F(DeviceSessionManagerTest, ResettingCredentialsAllowsANewAttempt) {
    build();
    factory->script({Error{ErrorKind::Unauthenticated, "wrong password", 1, false}});

    manager->handleRegistration(registered("DVR-1"));
    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Blocked; }));

    manager->resetCredentials(DeviceId{"DVR-1"});
    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Online; }));
    EXPECT_EQ(factory->calls(), 2U);
}

TEST_F(DeviceSessionManagerTest, TransientFailureRetriesWithBackoff) {
    build();
    factory->script({Error{ErrorKind::Unreachable, "down", 9, true}, Error{ErrorKind::Timeout, "slow", 3, true}});

    manager->handleRegistration(registered("DVR-1"));

    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Online; }));
    EXPECT_EQ(factory->calls(), 3U);
}

TEST_F(DeviceSessionManagerTest, MissingCredentialsBlockTheDevice) {
    build(false);

    manager->handleRegistration(registered("DVR-1"));

    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Blocked; }));
    EXPECT_EQ(factory->calls(), 0U);
    const auto snapshot = snapshotOf("DVR-1");
    EXPECT_EQ(snapshot.lastError.value_or(Error{}).kind, ErrorKind::NotFound);
}

TEST_F(DeviceSessionManagerTest, DisconnectNoticeMarksTheDeviceOfflineAndReleasesTheSession) {
    build();
    manager->handleRegistration(registered("DVR-1"));
    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Online; }));
    const auto token = manager->session(DeviceId{"DVR-1"})->connectionToken();

    manager->handleDisconnect(token);

    EXPECT_EQ(stateOf("DVR-1"), SessionState::Offline);
    EXPECT_EQ(manager->session(DeviceId{"DVR-1"}), nullptr);
    EXPECT_TRUE(waitFor([&] { return factory->alive.load() == 0; }));
}

TEST_F(DeviceSessionManagerTest, UnknownDisconnectTokenIsIgnored) {
    build();
    manager->handleRegistration(registered("DVR-1"));
    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Online; }));

    manager->handleDisconnect(999999);

    EXPECT_EQ(stateOf("DVR-1"), SessionState::Online);
}

TEST_F(DeviceSessionManagerTest, RepeatedRegistrationOfTheSameConnectionDoesNotRelogin) {
    build();
    manager->handleRegistration(registered("DVR-1", 40000));
    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Online; }));

    manager->handleRegistration(registered("DVR-1", 40000));
    std::this_thread::sleep_for(100ms);

    EXPECT_EQ(factory->calls(), 1U);
    EXPECT_EQ(factory->alive.load(), 1);
}

TEST_F(DeviceSessionManagerTest, RegistrationFromANewConnectionReplacesTheSession) {
    build();
    manager->handleRegistration(registered("DVR-1", 40000));
    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Online; }));
    const auto firstToken = manager->session(DeviceId{"DVR-1"})->connectionToken();

    manager->handleRegistration(registered("DVR-1", 40010));

    ASSERT_TRUE(waitFor([&] {
        const auto session = manager->session(DeviceId{"DVR-1"});
        return session != nullptr && session->connectionToken() != firstToken;
    }));
    EXPECT_EQ(factory->calls(), 2U);
    EXPECT_TRUE(waitFor([&] { return factory->alive.load() == 1; }));
}

TEST_F(DeviceSessionManagerTest, DisconnectedRegistrationEventMatchesByAddressAndPort) {
    build();
    manager->handleRegistration(registered("DVR-1", 40000));
    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Online; }));

    RegistrationEvent event;
    event.kind = RegistrationEventKind::Disconnected;
    event.remoteAddress = "192.168.100.62";
    event.remotePort = 40000;
    manager->handleRegistration(event);

    EXPECT_EQ(stateOf("DVR-1"), SessionState::Offline);
    EXPECT_EQ(manager->session(DeviceId{"DVR-1"}), nullptr);
}

TEST_F(DeviceSessionManagerTest, DevicesAreIndependent) {
    build();
    factory->script({Error{ErrorKind::Unauthenticated, "wrong password", 1, false}});

    manager->handleRegistration(registered("DVR-1"));
    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Blocked; }));
    manager->handleRegistration(registered("DVR-2"));

    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-2") == SessionState::Online; }));
    EXPECT_EQ(stateOf("DVR-1"), SessionState::Blocked);
    EXPECT_EQ(manager->snapshot().size(), 2U);
}

TEST_F(DeviceSessionManagerTest, ObserverReceivesStateChanges) {
    build();
    std::mutex mutex;
    std::vector<SessionState> states;
    manager->setObserver([&](const DeviceSnapshot& snapshot) {
        const std::scoped_lock lock(mutex);
        states.push_back(snapshot.state);
    });

    manager->handleRegistration(registered("DVR-1"));
    ASSERT_TRUE(waitFor([&] { return stateOf("DVR-1") == SessionState::Online; }));
    ASSERT_TRUE(waitFor([&] {
        const std::scoped_lock lock(mutex);
        return !states.empty() && states.back() == SessionState::Online;
    }));

    const std::scoped_lock lock(mutex);
    ASSERT_GE(states.size(), 2U);
    EXPECT_EQ(states.front(), SessionState::Registered);
}

TEST_F(DeviceSessionManagerTest, StopReleasesEverySession) {
    build();
    manager->handleRegistration(registered("DVR-1"));
    manager->handleRegistration(registered("DVR-2"));
    ASSERT_TRUE(waitFor([&] { return factory->alive.load() == 2; }));

    manager->stop();

    EXPECT_EQ(factory->alive.load(), 0);
}

}
