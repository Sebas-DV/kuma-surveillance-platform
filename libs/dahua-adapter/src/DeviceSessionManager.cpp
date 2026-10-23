#include "dahua/DeviceSessionManager.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace dahua {

namespace {

using Clock = LoginPolicy::Clock;

domain::Error missingCredentials(const DeviceId& id) {
    return domain::Error{domain::ErrorKind::NotFound, "no credentials configured for device " + id.value, 0, false};
}

}

struct DeviceSessionManager::Impl {
    struct Entry {
        explicit Entry(const LoginPolicyConfig& policyConfig) : policy(policyConfig) {}

        DeviceSnapshot snapshot;
        LoginPolicy policy;
        RegistrationEvent lastEvent;
        std::shared_ptr<IDeviceSession> session;
        bool pending{false};
        Clock::time_point due;
    };

    Impl(std::shared_ptr<ISessionFactory> sessionFactory, std::shared_ptr<const ICredentialProvider> credentialProvider,
         const DeviceSessionManagerConfig& managerConfig, const telemetry::Logger& log)
        : factory(std::move(sessionFactory)),
          credentials(std::move(credentialProvider)),
          config(managerConfig),
          logger(log) {}

    std::shared_ptr<ISessionFactory> factory;
    std::shared_ptr<const ICredentialProvider> credentials;
    DeviceSessionManagerConfig config;
    const telemetry::Logger& logger;

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable outboxWake;
    std::map<DeviceId, Entry> entries;
    std::deque<DeviceSnapshot> outbox;
    Observer observer;
    bool stopping{false};
    bool running{false};
    std::thread worker;
    std::thread dispatcher;

    void run();
    void dispatch();
    void scheduleLocked(Entry& entry, Clock::time_point now);
    void publishLocked(const DeviceSnapshot& snapshot);
    void attempt(const DeviceId& id, const RegistrationEvent& event);
    [[nodiscard]] std::optional<DeviceId> nextDueLocked(Clock::time_point& due) const;
};

void DeviceSessionManager::Impl::scheduleLocked(Entry& entry, const Clock::time_point now) {
    if (entry.policy.blocked()) {
        entry.pending = false;
        entry.snapshot.state = SessionState::Blocked;
        return;
    }
    entry.pending = true;
    entry.due = std::max(now, entry.policy.nextAttemptAt().value_or(now));
    entry.snapshot.state = entry.due > now ? SessionState::Backoff : SessionState::Registered;
}

void DeviceSessionManager::Impl::publishLocked(const DeviceSnapshot& snapshot) {
    outbox.push_back(snapshot);
    outboxWake.notify_one();
}

void DeviceSessionManager::Impl::dispatch() {
    std::unique_lock lock(mutex);
    while (true) {
        outboxWake.wait(lock, [this] { return stopping || !outbox.empty(); });
        if (outbox.empty()) {
            return;
        }
        const DeviceSnapshot snapshot = std::move(outbox.front());
        outbox.pop_front();
        const Observer callback = observer;
        lock.unlock();
        if (callback) {
            try {
                callback(snapshot);
            } catch (const std::exception& exception) {
                logger.error("device.observer_failed", {{"reason", exception.what()}});
            }
        }
        lock.lock();
    }
}

std::optional<DeviceId> DeviceSessionManager::Impl::nextDueLocked(Clock::time_point& due) const {
    std::optional<DeviceId> chosen;
    for (const auto& [id, entry] : entries) {
        if (entry.pending && (!chosen.has_value() || entry.due < due)) {
            chosen = id;
            due = entry.due;
        }
    }
    return chosen;
}

void DeviceSessionManager::Impl::run() {
    std::unique_lock lock(mutex);
    while (!stopping) {
        Clock::time_point due;
        const auto next = nextDueLocked(due);
        if (!next.has_value()) {
            wake.wait(lock);
            continue;
        }
        if (Clock::now() < due) {
            wake.wait_until(lock, due);
            continue;
        }

        const DeviceId& id = *next;
        Entry& entry = entries.at(id);
        entry.pending = false;
        entry.snapshot.state = SessionState::Authenticating;
        const RegistrationEvent event = entry.lastEvent;
        publishLocked(entry.snapshot);
        lock.unlock();
        attempt(id, event);
        lock.lock();
    }
}

void DeviceSessionManager::Impl::attempt(const DeviceId& id, const RegistrationEvent& event) {
    const auto credential = credentials->find(id);
    domain::Result<std::unique_ptr<IDeviceSession>> outcome =
        credential.has_value() ? factory->connect(event, *credential)
                               : domain::Result<std::unique_ptr<IDeviceSession>>::failure(missingCredentials(id));

    DeviceSnapshot snapshot;
    std::shared_ptr<IDeviceSession> replaced;
    {
        const std::scoped_lock lock(mutex);
        const auto found = entries.find(id);
        if (found == entries.end()) {
            return;
        }
        Entry& entry = found->second;
        const auto now = Clock::now();

        if (outcome.hasValue()) {
            entry.policy.recordSuccess();
            replaced = std::move(entry.session);
            entry.session = std::shared_ptr<IDeviceSession>(std::move(outcome).value());
            entry.snapshot.state = SessionState::Online;
            entry.snapshot.info = entry.session->info();
            entry.snapshot.failedAttempts = 0;
            entry.snapshot.lastError.reset();
        } else {
            const domain::Error& error = outcome.error();
            if (credential.has_value()) {
                entry.policy.recordFailure(error, now);
            } else {
                entry.policy.recordFailure(
                    domain::Error{domain::ErrorKind::Unauthenticated, error.message, 0, false}, now);
            }
            entry.snapshot.failedAttempts = entry.policy.consecutiveFailures();
            entry.snapshot.lastError = error;
            scheduleLocked(entry, now);
        }
        snapshot = entry.snapshot;
        publishLocked(snapshot);
    }
    wake.notify_all();

    if (snapshot.state == SessionState::Online && snapshot.info.has_value()) {
        logger.info("device.online", {{"device_id", id.value},
                                      {"channels", std::to_string(snapshot.info->channelCount)},
                                      {"remote", event.remoteAddress}});
    } else if (snapshot.lastError.has_value()) {
        logger.warn("device.login_failed", {{"device_id", id.value},
                                            {"state", toString(snapshot.state)},
                                            {"attempts", std::to_string(snapshot.failedAttempts)},
                                            {"kind", std::string(domain::toString(snapshot.lastError->kind))},
                                            {"reason", snapshot.lastError->message}});
    }
}

DeviceSessionManager::DeviceSessionManager(std::shared_ptr<ISessionFactory> factory,
                                           std::shared_ptr<const ICredentialProvider> credentials,
                                           DeviceSessionManagerConfig config, const telemetry::Logger& logger)
    : impl_(std::make_unique<Impl>(std::move(factory), std::move(credentials), config, logger)) {}

DeviceSessionManager::~DeviceSessionManager() {
    try {
        stop();
    } catch (const std::exception&) {
        impl_->logger.error("device.manager_stop_failed");
    }
}

void DeviceSessionManager::setObserver(Observer observer) {
    const std::scoped_lock lock(impl_->mutex);
    impl_->observer = std::move(observer);
}

void DeviceSessionManager::start() {
    const std::scoped_lock lock(impl_->mutex);
    if (impl_->running) {
        return;
    }
    impl_->running = true;
    impl_->stopping = false;
    impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
    impl_->dispatcher = std::thread([impl = impl_.get()] { impl->dispatch(); });
}

void DeviceSessionManager::stop() {
    std::map<DeviceId, std::shared_ptr<IDeviceSession>> sessions;
    {
        const std::scoped_lock lock(impl_->mutex);
        if (!impl_->running) {
            return;
        }
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    impl_->outboxWake.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    if (impl_->dispatcher.joinable()) {
        impl_->dispatcher.join();
    }
    {
        const std::scoped_lock lock(impl_->mutex);
        for (auto& [id, entry] : impl_->entries) {
            sessions[id] = std::move(entry.session);
            entry.snapshot.state = SessionState::Offline;
        }
        impl_->running = false;
    }
}

void DeviceSessionManager::handleRegistration(const RegistrationEvent& event) {
    std::shared_ptr<IDeviceSession> dropped;
    std::optional<DeviceSnapshot> changed;
    {
        const std::scoped_lock lock(impl_->mutex);
        const auto now = Clock::now();

        if (event.kind == RegistrationEventKind::Disconnected) {
            for (auto& [id, entry] : impl_->entries) {
                if (entry.snapshot.remoteAddress == event.remoteAddress && entry.snapshot.remotePort == event.remotePort) {
                    dropped = std::move(entry.session);
                    entry.pending = false;
                    entry.snapshot.state = SessionState::Offline;
                    changed = entry.snapshot;
                    impl_->publishLocked(entry.snapshot);
                    break;
                }
            }
        } else {
            auto [position, inserted] = impl_->entries.try_emplace(event.deviceId, impl_->config.loginPolicy);
            auto& entry = position->second;
            const bool sameConnection = !inserted && entry.session &&
                                        entry.snapshot.remoteAddress == event.remoteAddress &&
                                        entry.snapshot.remotePort == event.remotePort;
            if (sameConnection) {
                entry.snapshot.lastSeen = event.receivedAt;
                return;
            }

            entry.snapshot.id = event.deviceId;
            entry.snapshot.remoteAddress = event.remoteAddress;
            entry.snapshot.remotePort = event.remotePort;
            entry.snapshot.lastSeen = event.receivedAt;
            entry.lastEvent = event;
            dropped = std::move(entry.session);
            impl_->scheduleLocked(entry, now);
            changed = entry.snapshot;
            impl_->publishLocked(entry.snapshot);
        }
    }
    impl_->wake.notify_all();
    if (changed.has_value()) {
        impl_->logger.info("device.registered", {{"device_id", changed->id.value},
                                                 {"remote", event.remoteAddress + ":" + std::to_string(event.remotePort)},
                                                 {"state", toString(changed->state)}});
    }
}

void DeviceSessionManager::handleDisconnect(const std::uint64_t connectionToken) {
    std::shared_ptr<IDeviceSession> dropped;
    std::optional<DeviceSnapshot> changed;
    {
        const std::scoped_lock lock(impl_->mutex);
        for (auto& [id, entry] : impl_->entries) {
            if (entry.session && entry.session->connectionToken() == connectionToken) {
                dropped = std::move(entry.session);
                entry.pending = false;
                entry.snapshot.state = SessionState::Offline;
                changed = entry.snapshot;
                impl_->publishLocked(entry.snapshot);
                break;
            }
        }
    }
    if (changed.has_value()) {
        impl_->logger.warn("device.disconnected", {{"device_id", changed->id.value}});
    }
}

void DeviceSessionManager::resetCredentials(const DeviceId& id) {
    {
        const std::scoped_lock lock(impl_->mutex);
        const auto found = impl_->entries.find(id);
        if (found == impl_->entries.end()) {
            return;
        }
        found->second.policy.resetCredentials();
        if (!found->second.session) {
            impl_->scheduleLocked(found->second, Clock::now());
        }
    }
    impl_->wake.notify_all();
}

std::vector<DeviceSnapshot> DeviceSessionManager::snapshot() const {
    const std::scoped_lock lock(impl_->mutex);
    std::vector<DeviceSnapshot> result;
    result.reserve(impl_->entries.size());
    for (const auto& [id, entry] : impl_->entries) {
        result.push_back(entry.snapshot);
    }
    return result;
}

std::optional<DeviceSnapshot> DeviceSessionManager::find(const DeviceId& id) const {
    const std::scoped_lock lock(impl_->mutex);
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end()) {
        return std::nullopt;
    }
    return found->second.snapshot;
}

std::shared_ptr<IDeviceSession> DeviceSessionManager::session(const DeviceId& id) const {
    const std::scoped_lock lock(impl_->mutex);
    const auto found = impl_->entries.find(id);
    return found == impl_->entries.end() ? nullptr : found->second.session;
}

}
