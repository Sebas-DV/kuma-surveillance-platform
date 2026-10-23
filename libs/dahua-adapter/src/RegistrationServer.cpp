#include "dahua/RegistrationServer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "detail/SdkSupport.hpp"
#include "dhnetsdk.h"
#include "telemetry/Logger.hpp"

namespace dahua {

namespace {

constexpr std::chrono::seconds kCoalesceWindow{3};
constexpr std::size_t kRecentLimit = 256;
constexpr std::size_t kSerialMaxLength = 64;

}

struct RegistrationServer::Impl {
    std::shared_ptr<NetSdkRuntime> runtime;
    RegistrationServerConfig config;
    Handler handler;

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<RegistrationEvent> queue;
    std::map<std::string, std::chrono::steady_clock::time_point> recent;
    bool stopping{false};
    std::thread worker;

    std::atomic<LLONG> listenHandle{0};
    std::atomic<std::uint64_t> dropped{0};

    static int CALLBACK onService(LLONG handle, char* address, WORD port, LONG command, void* parameter,
                                  DWORD length, LDWORD user);

    [[nodiscard]] bool isDuplicate(const RegistrationEvent& event);
    void enqueue(RegistrationEvent event);
    void run();
};

int CALLBACK RegistrationServer::Impl::onService(LLONG, char* address, const WORD port, const LONG command,
                                                 void* parameter, const DWORD length, const LDWORD user) {
    auto* self = reinterpret_cast<Impl*>(user);
    if (self == nullptr) {
        return 0;
    }

    RegistrationEvent event;
    event.remoteAddress = address != nullptr ? address : "";
    event.remotePort = port;
    event.receivedAt = std::chrono::system_clock::now();

    switch (command) {
        case DH_DVR_DISCONNECT:
            event.kind = RegistrationEventKind::Disconnected;
            break;
        case DH_DVR_SERIAL_RETURN:
            event.deviceId.value = detail::readCString(parameter, length, kSerialMaxLength);
            break;
        case DH_DVR_SERIAL_RETURN_EX:
            if (parameter != nullptr && length >= sizeof(NET_CB_SERIAL_RETURN_INFO)) {
                const auto* info = static_cast<const NET_CB_SERIAL_RETURN_INFO*>(parameter);
                event.deviceId.value = detail::readCString(std::span<const char>(info->szDevSerial), kSerialMaxLength);
                event.redirectionSupported = info->bSupportRedirection != FALSE;
            }
            break;
        case NET_DEV_AUTOREGISTER_RETURN:
            if (parameter != nullptr && length >= sizeof(NET_CB_AUTOREGISTER)) {
                const auto* info = static_cast<const NET_CB_AUTOREGISTER*>(parameter);
                event.deviceId.value = detail::readCString(std::span<const char>(info->szDevSerial), kSerialMaxLength);
            }
            break;
        default:
            return 0;
    }

    if (event.kind == RegistrationEventKind::Registered && event.deviceId.value.empty()) {
        return 0;
    }
    if (event.kind == RegistrationEventKind::Registered && self->isDuplicate(event)) {
        return 0;
    }

    self->enqueue(std::move(event));
    return 0;
}

bool RegistrationServer::Impl::isDuplicate(const RegistrationEvent& event) {
    const auto now = std::chrono::steady_clock::now();
    const std::string key = event.deviceId.value + '|' + event.remoteAddress + ':' + std::to_string(event.remotePort);

    const std::scoped_lock lock(mutex);
    if (recent.size() >= kRecentLimit) {
        std::erase_if(recent, [now](const auto& entry) { return now - entry.second > kCoalesceWindow; });
    }
    const auto found = recent.find(key);
    if (found != recent.end() && now - found->second < kCoalesceWindow) {
        return true;
    }
    recent[key] = now;
    return false;
}

void RegistrationServer::Impl::enqueue(RegistrationEvent event) {
    {
        const std::scoped_lock lock(mutex);
        if (queue.size() >= config.queueCapacity) {
            queue.pop_front();
            dropped.fetch_add(1);
        }
        queue.push_back(std::move(event));
    }
    wake.notify_one();
}

void RegistrationServer::Impl::run() {
    std::unique_lock lock(mutex);
    while (true) {
        wake.wait(lock, [this] { return stopping || !queue.empty(); });
        if (queue.empty()) {
            if (stopping) {
                return;
            }
            continue;
        }
        const RegistrationEvent event = std::move(queue.front());
        queue.pop_front();
        lock.unlock();
        try {
            if (handler) {
                handler(event);
            }
        } catch (const std::exception& exception) {
            telemetry::Logger::global().error("registration.handler_failed", {{"reason", exception.what()}});
        }
        lock.lock();
    }
}

RegistrationServer::RegistrationServer(std::shared_ptr<NetSdkRuntime> runtime, RegistrationServerConfig config,
                                       Handler handler)
    : impl_(std::make_unique<Impl>()) {
    impl_->runtime = std::move(runtime);
    impl_->config = std::move(config);
    impl_->handler = std::move(handler);
}

RegistrationServer::~RegistrationServer() { stop(); }

domain::Result<void> RegistrationServer::start() {
    if (impl_->listenHandle.load() != 0) {
        return domain::Result<void>::failure(
            domain::Error{domain::ErrorKind::InvalidState, "registration server already started", 0, false});
    }

    {
        const std::scoped_lock lock(impl_->mutex);
        impl_->stopping = false;
    }
    impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });

    std::string address = impl_->config.bindAddress;
    const LLONG handle = CLIENT_ListenServer(address.data(), impl_->config.port, 1000, &Impl::onService,
                                             reinterpret_cast<LDWORD>(impl_.get()));
    if (handle == 0) {
        auto error = detail::sdkError("CLIENT_ListenServer");
        {
            const std::scoped_lock lock(impl_->mutex);
            impl_->stopping = true;
        }
        impl_->wake.notify_all();
        impl_->worker.join();
        return domain::Result<void>::failure(std::move(error));
    }
    impl_->listenHandle.store(handle);
    return domain::Result<void>::success();
}

void RegistrationServer::stop() {
    const LLONG handle = impl_->listenHandle.exchange(0);
    if (handle != 0) {
        CLIENT_StopListenServer(handle);
    }
    {
        const std::scoped_lock lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

bool RegistrationServer::running() const noexcept { return impl_->listenHandle.load() != 0; }

std::uint64_t RegistrationServer::droppedEvents() const noexcept { return impl_->dropped.load(); }

}
