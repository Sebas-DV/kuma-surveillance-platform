#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "DbCredentialProvider.hpp"
#include "GatewayConfig.hpp"
#include "dahua/DeviceSessionManager.hpp"
#include "dahua/NetSdkRuntime.hpp"
#include "dahua/RegistrationServer.hpp"
#include "dahua/SdkSessionFactory.hpp"
#include "domain/Environment.hpp"
#include "persistence/Database.hpp"
#include "persistence/DeviceRepository.hpp"
#include "persistence/DriverLog.hpp"
#include "persistence/Migrator.hpp"
#include "security/SecretBox.hpp"
#include "telemetry/Logger.hpp"

namespace {

volatile std::sig_atomic_t gStopRequested = 0;

void onSignal(int) { gStopRequested = 1; }

struct ProbeResult {
    std::uint64_t video{0};
    std::uint64_t audio{0};
    std::uint64_t keyFrames{0};
    std::int64_t maxAudioGapMs{0};
    std::int64_t lastAudioMs{-1};
    std::string videoCodec{"unknown"};
    std::string audioCodec{"unknown"};
    int width{0};
    int height{0};
};

std::string_view toString(const dahua::VideoCodec codec) {
    switch (codec) {
        case dahua::VideoCodec::H264: return "h264";
        case dahua::VideoCodec::H265: return "h265";
        case dahua::VideoCodec::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view toString(const dahua::AudioCodec codec) {
    switch (codec) {
        case dahua::AudioCodec::G711A: return "g711a";
        case dahua::AudioCodec::G711U: return "g711u";
        case dahua::AudioCodec::Pcm16: return "pcm16";
        case dahua::AudioCodec::Unknown: return "unknown";
    }
    return "unknown";
}

class ProbeRunner {
public:
    ProbeRunner(std::shared_ptr<dahua::DeviceSessionManager> manager, const int channel, const int seconds)
        : manager_(std::move(manager)), channel_(channel), seconds_(seconds) {}

    ProbeRunner(const ProbeRunner&) = delete;
    ProbeRunner& operator=(const ProbeRunner&) = delete;
    ProbeRunner(ProbeRunner&&) = delete;
    ProbeRunner& operator=(ProbeRunner&&) = delete;

    ~ProbeRunner() { join(); }

    void onDeviceOnline(const dahua::DeviceId& id) {
        const std::scoped_lock lock(mutex_);
        if (!started_.insert(id.value).second) {
            return;
        }
        threads_.emplace_back(&ProbeRunner::run, this, id);
    }

    void join() {
        std::vector<std::thread> threads;
        {
            const std::scoped_lock lock(mutex_);
            threads.swap(threads_);
        }
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

private:
    void run(const dahua::DeviceId& id) noexcept {
        try {
            probe(id);
        } catch (const std::exception&) {
            telemetry::Logger::global().error("probe.failed");
        }
    }

    void probe(const dahua::DeviceId& id) {
        auto& logger = telemetry::Logger::global();
        const auto session = manager_->session(id);
        if (!session) {
            return;
        }

        ProbeResult result;
        std::mutex resultMutex;
        auto opened = session->openLiveStream(
            dahua::LiveStreamRequest{channel_, dahua::StreamProfile::Main},
            [&](const dahua::MediaPacket& packet) {
                const std::scoped_lock lock(resultMutex);
                if (packet.kind == dahua::MediaKind::Video) {
                    ++result.video;
                    result.keyFrames += packet.video.keyFrame ? 1U : 0U;
                    if (packet.video.keyFrame) {
                        result.videoCodec = std::string(toString(packet.video.codec));
                        result.width = packet.video.width;
                        result.height = packet.video.height;
                    }
                } else if (packet.kind == dahua::MediaKind::Audio) {
                    ++result.audio;
                    result.audioCodec = std::string(toString(packet.audio.codec));
                    if (result.lastAudioMs >= 0) {
                        result.maxAudioGapMs = std::max(result.maxAudioGapMs, packet.timestampMs - result.lastAudioMs);
                    }
                    result.lastAudioMs = packet.timestampMs;
                }
            });

        if (!opened) {
            logger.error("probe.open_failed", {{"device_id", id.value}, {"reason", opened.error().message}});
            return;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds_);
        while (std::chrono::steady_clock::now() < deadline && gStopRequested == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        opened.value()->stop();

        const std::scoped_lock lock(resultMutex);
        logger.info("probe.result", {{"device_id", id.value},
                                     {"channel", std::to_string(channel_)},
                                     {"video_packets", std::to_string(result.video)},
                                     {"key_frames", std::to_string(result.keyFrames)},
                                     {"audio_packets", std::to_string(result.audio)},
                                     {"video_codec", result.videoCodec},
                                     {"audio_codec", result.audioCodec},
                                     {"resolution", std::to_string(result.width) + "x" + std::to_string(result.height)},
                                     {"max_audio_gap_ms", std::to_string(result.maxAudioGapMs)}});
    }

    std::shared_ptr<dahua::DeviceSessionManager> manager_;
    int channel_;
    int seconds_;
    std::mutex mutex_;
    std::set<std::string> started_;
    std::vector<std::thread> threads_;
};

struct DatabaseServices {
    std::shared_ptr<persistence::Database> database;
    std::shared_ptr<persistence::DeviceRepository> devices;
    std::shared_ptr<gateway::DbCredentialProvider> provider;
};

class DeviceRecorder {
public:
    explicit DeviceRecorder(std::shared_ptr<persistence::DeviceRepository> devices)
        : devices_(std::move(devices)) {}

    void onSnapshot(const dahua::DeviceSnapshot& snapshot) {
        if (snapshot.state != dahua::SessionState::Online || !snapshot.info.has_value()) {
            if (snapshot.state != dahua::SessionState::Authenticating) {
                const std::scoped_lock lock(mutex_);
                online_.erase(snapshot.id.value);
            }
            return;
        }
        {
            const std::scoped_lock lock(mutex_);
            if (!online_.insert(snapshot.id.value).second) {
                return;
            }
        }
        const auto recorded =
            devices_->recordSeen(snapshot.id.value, snapshot.info->serialNumber, snapshot.info->channelCount);
        if (!recorded) {
            telemetry::Logger::global().warn(
                "device.record_failed", {{"device_id", snapshot.id.value}, {"reason", recorded.error().message}});
        }
    }

private:
    std::shared_ptr<persistence::DeviceRepository> devices_;
    std::mutex mutex_;
    std::set<std::string> online_;
};

class CredentialWatcher {
public:
    CredentialWatcher(std::shared_ptr<dahua::DeviceSessionManager> manager,
                      std::shared_ptr<gateway::DbCredentialProvider> provider,
                      const std::chrono::milliseconds interval)
        : manager_(std::move(manager)), provider_(std::move(provider)), interval_(interval) {
        thread_ = std::thread(&CredentialWatcher::run, this);
    }

    CredentialWatcher(const CredentialWatcher&) = delete;
    CredentialWatcher& operator=(const CredentialWatcher&) = delete;
    CredentialWatcher(CredentialWatcher&&) = delete;
    CredentialWatcher& operator=(CredentialWatcher&&) = delete;

    ~CredentialWatcher() {
        {
            const std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run() noexcept {
        try {
            std::unique_lock lock(mutex_);
            while (!stopping_) {
                if (wake_.wait_for(lock, interval_, [this] { return stopping_; })) {
                    break;
                }
                lock.unlock();
                inspect();
                lock.lock();
            }
        } catch (const std::exception&) {
            telemetry::Logger::global().error("credentials.watcher_failed");
        }
    }

    void inspect() const {
        for (const auto& snapshot : manager_->snapshot()) {
            if (snapshot.state == dahua::SessionState::Blocked && provider_->changedSinceLastUse(snapshot.id)) {
                telemetry::Logger::global().info("credentials.changed", {{"device_id", snapshot.id.value}});
                manager_->resetCredentials(snapshot.id);
            }
        }
    }

    std::shared_ptr<dahua::DeviceSessionManager> manager_;
    std::shared_ptr<gateway::DbCredentialProvider> provider_;
    std::chrono::milliseconds interval_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    std::thread thread_;
};

domain::Result<std::optional<DatabaseServices>> openDatabase(const gateway::GatewayConfig& config) {
    using Outcome = domain::Result<std::optional<DatabaseServices>>;

    const auto url = domain::readEnvironment("SURVEILLANCE_DATABASE_URL");
    if (!url || url->empty()) {
        return Outcome::success(std::nullopt);
    }
    const auto encodedKey = domain::readEnvironment("SURVEILLANCE_SECRET_KEY");
    if (!encodedKey) {
        return Outcome::failure(domain::Error{domain::ErrorKind::InvalidArgument,
                                              "SURVEILLANCE_SECRET_KEY is required when SURVEILLANCE_DATABASE_URL is set", 0, false});
    }
    persistence::sanitizeDriverLogs();
    auto secretBox = security::SecretBox::fromBase64(*encodedKey);
    if (!secretBox) {
        return Outcome::failure(secretBox.error());
    }
    auto database = persistence::Database::open(persistence::DatabaseConfig{*url, 4});
    if (!database) {
        return Outcome::failure(database.error());
    }
    if (config.migrationsDirectory.has_value()) {
        const auto report = persistence::migrate(*database.value(), *config.migrationsDirectory);
        if (!report) {
            return Outcome::failure(report.error());
        }
        telemetry::Logger::global().info("database.migrated",
                                         {{"applied", std::to_string(report.value().applied.size())},
                                          {"already_applied", std::to_string(report.value().alreadyApplied)}});
    }
    DatabaseServices services;
    services.database = std::move(database).value();
    services.devices = std::make_shared<persistence::DeviceRepository>(services.database, std::move(secretBox).value());
    services.provider = std::make_shared<gateway::DbCredentialProvider>(services.devices);
    return Outcome::success(std::move(services));
}

int runGateway(const int argc, char** argv) {

    std::vector<std::string_view> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const auto parsed = gateway::parseGatewayArguments(arguments);
    if (!parsed) {
        std::cerr << parsed.error().message << "\n\n" << gateway::gatewayUsage();
        return 2;
    }
    const gateway::GatewayConfig& config = parsed.value();
    if (config.showHelp) {
        std::cout << gateway::gatewayUsage();
        return 0;
    }

    auto& logger = telemetry::Logger::global();
    logger.setLevel(config.logLevel);

    auto services = openDatabase(config);
    if (!services) {
        logger.error("gateway.database_failed", {{"kind", std::string(domain::toString(services.error().kind))},
                                                 {"reason", services.error().message}});
        return 1;
    }

    const std::optional<DatabaseServices>& database = services.value();
    std::shared_ptr<const dahua::ICredentialProvider> credentialProvider;
    if (database.has_value()) {
        credentialProvider = database->provider;
        logger.info("gateway.credentials_source", {{"source", "database"}});
    } else {
        auto credentials = gateway::credentialsFromEnvironment();
        if (!credentials) {
            logger.warn("gateway.no_credentials",
                        {{"hint", "set SURVEILLANCE_DATABASE_URL or SURVEILLANCE_DAHUA_USER and SURVEILLANCE_DAHUA_PASSWORD"}});
            credentials = dahua::Credentials{};
        }
        credentialProvider = std::make_shared<dahua::StaticCredentialProvider>(*credentials);
        logger.info("gateway.credentials_source", {{"source", "environment"}});
    }

    const auto runtime = dahua::NetSdkRuntime::acquire();
    if (!runtime) {
        logger.error("gateway.sdk_init_failed", {{"reason", runtime.error().message}});
        return 1;
    }

    auto manager = std::make_shared<dahua::DeviceSessionManager>(
        std::make_shared<dahua::SdkSessionFactory>(runtime.value()), credentialProvider);

    std::unique_ptr<ProbeRunner> probe;
    if (config.probeChannel.has_value()) {
        probe = std::make_unique<ProbeRunner>(manager, *config.probeChannel, config.probeSeconds);
    }
    std::unique_ptr<DeviceRecorder> recorder;
    if (database.has_value()) {
        recorder = std::make_unique<DeviceRecorder>(database->devices);
    }
    if (probe || recorder) {
        manager->setObserver([&probe, &recorder](const dahua::DeviceSnapshot& snapshot) {
            if (recorder) {
                recorder->onSnapshot(snapshot);
            }
            if (probe && snapshot.state == dahua::SessionState::Online) {
                probe->onDeviceOnline(snapshot.id);
            }
        });
    }

    runtime.value()->setDisconnectHandler(
        [manager](const dahua::DisconnectNotice& notice) { manager->handleDisconnect(notice.connectionToken); });

    dahua::RegistrationServer server(
        runtime.value(), dahua::RegistrationServerConfig{config.bindAddress, config.port, 1024},
        [manager](const dahua::RegistrationEvent& event) { manager->handleRegistration(event); });

    manager->start();
    if (const auto started = server.start(); !started) {
        logger.error("gateway.listen_failed",
                     {{"port", std::to_string(config.port)}, {"reason", started.error().message}});
        manager->stop();
        return 1;
    }

    std::unique_ptr<CredentialWatcher> watcher;
    if (database.has_value()) {
        watcher = std::make_unique<CredentialWatcher>(manager, database->provider, std::chrono::seconds(15));
    }

    logger.info("gateway.started", {{"bind", config.bindAddress},
                                    {"port", std::to_string(config.port)},
                                    {"sdk_version", std::to_string(runtime.value()->sdkVersion())}});

    static_cast<void>(std::signal(SIGINT, onSignal));
    static_cast<void>(std::signal(SIGTERM, onSignal));
    const auto started = std::chrono::steady_clock::now();
    const auto lifetime = std::chrono::seconds(config.exitAfterSeconds.value_or(0));
    while (gStopRequested == 0 &&
           (!config.exitAfterSeconds.has_value() || std::chrono::steady_clock::now() - started < lifetime)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    logger.info("gateway.stopping");
    watcher.reset();
    server.stop();
    if (probe) {
        probe->join();
    }
    manager->stop();
    runtime.value()->setDisconnectHandler(nullptr);
    logger.info("gateway.stopped");
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        return runGateway(argc, argv);
    } catch (const std::exception& exception) {
        static_cast<void>(std::fputs("fatal: ", stderr));
        static_cast<void>(std::fputs(exception.what(), stderr));
        static_cast<void>(std::fputc('\n', stderr));
        return 1;
    }
}
