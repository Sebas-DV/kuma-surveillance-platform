#include "dahua/NetSdkRuntime.hpp"

#include <utility>

#include "detail/SdkSupport.hpp"
#include "dhnetsdk.h"

namespace dahua {

namespace {

std::mutex gAcquireMutex;
std::weak_ptr<NetSdkRuntime> gInstance;

void CALLBACK onDisconnect(const LLONG loginId, char* address, const LONG port, const LDWORD user) {
    const auto* runtime = reinterpret_cast<const NetSdkRuntime*>(user);
    if (runtime == nullptr) {
        return;
    }
    DisconnectNotice notice;
    notice.connectionToken = static_cast<std::uint64_t>(loginId);
    notice.address = address != nullptr ? address : "";
    notice.port = static_cast<int>(port);
    runtime->dispatchDisconnect(notice);
}

}

NetSdkRuntime::NetSdkRuntime(PassKey) {}

NetSdkRuntime::~NetSdkRuntime() {
    if (initialized_) {
        CLIENT_Cleanup();
    }
}

domain::Result<std::shared_ptr<NetSdkRuntime>> NetSdkRuntime::acquire(const NetSdkConfig& config) {
    const std::scoped_lock lock(gAcquireMutex);
    if (auto existing = gInstance.lock()) {
        return domain::Result<std::shared_ptr<NetSdkRuntime>>::success(std::move(existing));
    }

    auto runtime = std::make_shared<NetSdkRuntime>(PassKey{});
    if (const auto initialized = runtime->initialize(config); !initialized) {
        return domain::Result<std::shared_ptr<NetSdkRuntime>>::failure(initialized.error());
    }
    gInstance = runtime;
    return domain::Result<std::shared_ptr<NetSdkRuntime>>::success(std::move(runtime));
}

domain::Result<void> NetSdkRuntime::initialize(const NetSdkConfig& config) {
    if (CLIENT_Init(onDisconnect, reinterpret_cast<LDWORD>(this)) == FALSE) {
        return domain::Result<void>::failure(detail::sdkError("CLIENT_Init"));
    }
    initialized_ = true;
    CLIENT_SetConnectTime(static_cast<int>(config.connectTimeout.count()), config.connectAttempts);
    return domain::Result<void>::success();
}

void NetSdkRuntime::setDisconnectHandler(DisconnectHandler handler) {
    const std::scoped_lock lock(mutex_);
    handler_ = std::move(handler);
}

std::uint32_t NetSdkRuntime::sdkVersion() const noexcept { return CLIENT_GetSDKVersion(); }

void NetSdkRuntime::dispatchDisconnect(const DisconnectNotice& notice) const {
    DisconnectHandler handler;
    {
        const std::scoped_lock lock(mutex_);
        handler = handler_;
    }
    if (handler) {
        handler(notice);
    }
}

}
