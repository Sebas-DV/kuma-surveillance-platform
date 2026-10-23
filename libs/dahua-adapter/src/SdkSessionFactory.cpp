#include "dahua/SdkSessionFactory.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "detail/Secure.hpp"
#include "detail/SdkSupport.hpp"
#include "dhnetsdk.h"
#include "dahua/DhavParser.hpp"
#include "dahua/G711.hpp"
#include "dahua/LoginErrors.hpp"

namespace dahua {

namespace {

using domain::Error;
using domain::ErrorKind;

constexpr int kTalkSampleRate = 8000;
constexpr int kTalkPacketPeriodMs = 25;

DH_RealPlayType realPlayType(const StreamProfile profile) {
    switch (profile) {
        case StreamProfile::Main: return DH_RType_Realplay_0;
        case StreamProfile::Sub1: return DH_RType_Realplay_1;
        case StreamProfile::Sub2: return DH_RType_Realplay_2;
        case StreamProfile::Sub3: return DH_RType_Realplay_3;
    }
    return DH_RType_Realplay_0;
}

class SdkLiveStream final : public ILiveStream {
public:
    explicit SdkLiveStream(PacketHandler handler)
        : parser_([this](const MediaPacket& packet) { onPacket(packet); }), handler_(std::move(handler)) {}

    SdkLiveStream(const SdkLiveStream&) = delete;
    SdkLiveStream& operator=(const SdkLiveStream&) = delete;
    SdkLiveStream(SdkLiveStream&&) = delete;
    SdkLiveStream& operator=(SdkLiveStream&&) = delete;

    ~SdkLiveStream() override { stop(); }

    [[nodiscard]] domain::Result<void> start(const LLONG loginHandle, const LiveStreamRequest& request) {
        const std::scoped_lock lock(mutex_);
        realHandle_ = CLIENT_RealPlayEx(loginHandle, request.channel, nullptr, realPlayType(request.profile));
        if (realHandle_ == 0) {
            return domain::Result<void>::failure(detail::sdkError("CLIENT_RealPlayEx"));
        }
        if (CLIENT_SetRealDataCallBackEx2(realHandle_, &SdkLiveStream::onData, reinterpret_cast<LDWORD>(this),
                                          static_cast<DWORD>(REALDATA_FLAG_RAW_DATA)) == FALSE) {
            auto error = detail::sdkError("CLIENT_SetRealDataCallBackEx2");
            CLIENT_StopRealPlayEx(realHandle_);
            realHandle_ = 0;
            return domain::Result<void>::failure(std::move(error));
        }
        return domain::Result<void>::success();
    }

    void stop() override {
        const std::scoped_lock lock(mutex_);
        if (realHandle_ != 0) {
            CLIENT_StopRealPlayEx(realHandle_);
            realHandle_ = 0;
        }
    }

    [[nodiscard]] std::uint64_t packetCount() const noexcept override { return packets_.load(); }

private:
    static void CALLBACK onData(LLONG, const DWORD dataType, BYTE* buffer, const DWORD size, LLONG, const LDWORD user) {
        auto* self = reinterpret_cast<SdkLiveStream*>(user);
        if (self == nullptr || dataType != 0 || buffer == nullptr || size == 0) {
            return;
        }
        self->parser_.feed(std::as_bytes(std::span<const BYTE>(buffer, size)));
    }

    void onPacket(const MediaPacket& packet) {
        packets_.fetch_add(1);
        if (handler_) {
            handler_(packet);
        }
    }

    std::mutex mutex_;
    DhavParser parser_;
    PacketHandler handler_;
    std::atomic<std::uint64_t> packets_{0};
    LLONG realHandle_{0};
};

struct TalkFormat {
    DH_TALK_CODING_TYPE type;
    int bits;
};

TalkFormat talkFormat(const TalkCodec codec) {
    switch (codec) {
        case TalkCodec::Pcm16: return {DH_TALK_PCM, 16};
        case TalkCodec::G711A: return {DH_TALK_G711a, 8};
        case TalkCodec::G711U: return {DH_TALK_G711u, 8};
    }
    return {DH_TALK_PCM, 16};
}

void CALLBACK ignoreTalkAudio(LLONG, char*, DWORD, BYTE, LDWORD) {}

class SdkTalkSession final : public ITalkSession {
public:
    explicit SdkTalkSession(const TalkCodec codec) : codec_(codec) {}

    SdkTalkSession(const SdkTalkSession&) = delete;
    SdkTalkSession& operator=(const SdkTalkSession&) = delete;
    SdkTalkSession(SdkTalkSession&&) = delete;
    SdkTalkSession& operator=(SdkTalkSession&&) = delete;

    ~SdkTalkSession() override { stop(); }

    [[nodiscard]] domain::Result<void> start(const LLONG loginHandle, const TalkRequest& request) {
        const std::scoped_lock lock(mutex_);
        const TalkFormat format = talkFormat(request.codec);

        if (CLIENT_SetDeviceMode(loginHandle, DH_TALK_CLIENT_MODE, nullptr) == FALSE) {
            return domain::Result<void>::failure(detail::sdkError("CLIENT_SetDeviceMode(client mode)"));
        }

        DHDEV_TALKDECODE_INFO decode{};
        decode.encodeType = format.type;
        decode.nAudioBit = format.bits;
        decode.dwSampleRate = kTalkSampleRate;
        decode.nPacketPeriod = kTalkPacketPeriodMs;
        if (CLIENT_SetDeviceMode(loginHandle, DH_TALK_ENCODE_TYPE, &decode) == FALSE) {
            return domain::Result<void>::failure(detail::sdkError("CLIENT_SetDeviceMode(encode type)"));
        }

        NET_TALK_TRANSFER_PARAM transfer{};
        transfer.dwSize = sizeof(transfer);
        transfer.bTransfer = request.transfer ? TRUE : FALSE;
        if (CLIENT_SetDeviceMode(loginHandle, DH_TALK_TRANSFER_MODE, &transfer) == FALSE) {
            return domain::Result<void>::failure(detail::sdkError("CLIENT_SetDeviceMode(transfer mode)"));
        }

        int channel = request.channel;
        if (CLIENT_SetDeviceMode(loginHandle, DH_TALK_TALK_CHANNEL, &channel) == FALSE) {
            return domain::Result<void>::failure(detail::sdkError("CLIENT_SetDeviceMode(talk channel)"));
        }

        auto speak = std::make_unique<NET_SPEAK_PARAM>();
        speak->dwSize = sizeof(NET_SPEAK_PARAM);
        speak->nMode = 0;
        speak->nSpeakerChannel = request.channel;
        if (CLIENT_SetDeviceMode(loginHandle, DH_TALK_SPEAK_PARAM, speak.get()) == FALSE) {
            return domain::Result<void>::failure(detail::sdkError("CLIENT_SetDeviceMode(speak parameters)"));
        }

        handle_ = CLIENT_StartTalkEx(loginHandle, &ignoreTalkAudio, 0);
        if (handle_ == 0) {
            return domain::Result<void>::failure(detail::sdkError("CLIENT_StartTalkEx"));
        }
        return domain::Result<void>::success();
    }

    [[nodiscard]] domain::Result<void> sendAudio(const std::span<const std::byte> pcm16) override {
        if (pcm16.empty() || pcm16.size() % sizeof(std::int16_t) != 0) {
            return domain::Result<void>::failure(
                Error{ErrorKind::InvalidArgument, "audio must be a non-empty whole number of 16-bit samples", 0, false});
        }

        std::vector<BYTE> buffer;
        if (codec_ == TalkCodec::Pcm16) {
            buffer.resize(pcm16.size());
            std::memcpy(buffer.data(), pcm16.data(), pcm16.size());
        } else {
            std::vector<std::int16_t> samples(pcm16.size() / sizeof(std::int16_t));
            std::memcpy(samples.data(), pcm16.data(), pcm16.size());
            const std::vector<std::byte> encoded = codec_ == TalkCodec::G711A ? encodeAlaw(samples) : encodeUlaw(samples);
            buffer.resize(encoded.size());
            std::memcpy(buffer.data(), encoded.data(), encoded.size());
        }

        const TalkFormat format = talkFormat(codec_);
        NET_IN_TALK_SEND_DATA_STREAM input{};
        input.dwSize = sizeof(input);
        input.pBuf = buffer.data();
        input.dwBufSize = static_cast<DWORD>(buffer.size());
        input.bNeedHead = TRUE;
        input.emEncodeType = format.type;
        input.nAudioBit = format.bits;
        input.dwSampleRate = kTalkSampleRate;
        NET_OUT_TALK_SEND_DATA_STREAM output{};
        output.dwSize = sizeof(output);

        const std::scoped_lock lock(mutex_);
        if (handle_ == 0) {
            return domain::Result<void>::failure(Error{ErrorKind::InvalidState, "talk session stopped", 0, false});
        }
        if (CLIENT_TalkSendDataByStream(handle_, &input, &output) < 0) {
            return domain::Result<void>::failure(detail::sdkError("CLIENT_TalkSendDataByStream"));
        }
        return domain::Result<void>::success();
    }

    void stop() override {
        const std::scoped_lock lock(mutex_);
        if (handle_ != 0) {
            CLIENT_StopTalkEx(handle_);
            handle_ = 0;
        }
    }

private:
    std::mutex mutex_;
    TalkCodec codec_;
    LLONG handle_{0};
};

class SdkDeviceSession final : public IDeviceSession {
public:
    SdkDeviceSession(std::shared_ptr<NetSdkRuntime> runtime, DeviceId id, DeviceInfo info, const LLONG loginHandle)
        : runtime_(std::move(runtime)), id_(std::move(id)), info_(std::move(info)), loginHandle_(loginHandle) {}

    SdkDeviceSession(const SdkDeviceSession&) = delete;
    SdkDeviceSession& operator=(const SdkDeviceSession&) = delete;
    SdkDeviceSession(SdkDeviceSession&&) = delete;
    SdkDeviceSession& operator=(SdkDeviceSession&&) = delete;

    ~SdkDeviceSession() override {
        stopAll();
        CLIENT_Logout(loginHandle_);
    }

    [[nodiscard]] const DeviceId& id() const noexcept override { return id_; }

    [[nodiscard]] const DeviceInfo& info() const noexcept override { return info_; }

    [[nodiscard]] std::uint64_t connectionToken() const noexcept override {
        return static_cast<std::uint64_t>(loginHandle_);
    }

    [[nodiscard]] domain::Result<std::shared_ptr<ILiveStream>> openLiveStream(const LiveStreamRequest& request,
                                                                            PacketHandler handler) override {
        using Output = domain::Result<std::shared_ptr<ILiveStream>>;
        if (request.channel < 0 || request.channel >= info_.channelCount) {
            return Output::failure(Error{ErrorKind::InvalidArgument, "channel out of range", request.channel, false});
        }
        auto stream = std::make_shared<SdkLiveStream>(std::move(handler));
        if (const auto started = stream->start(loginHandle_, request); !started) {
            return Output::failure(started.error());
        }
        const std::scoped_lock lock(mutex_);
        std::erase_if(streams_, [](const std::weak_ptr<ILiveStream>& entry) { return entry.expired(); });
        streams_.emplace_back(stream);
        return Output::success(std::move(stream));
    }

    [[nodiscard]] domain::Result<std::shared_ptr<ITalkSession>> openTalk(const TalkRequest& request) override {
        using Output = domain::Result<std::shared_ptr<ITalkSession>>;
        if (request.channel < 0 || request.channel >= info_.channelCount) {
            return Output::failure(Error{ErrorKind::InvalidArgument, "channel out of range", request.channel, false});
        }
        auto talk = std::make_shared<SdkTalkSession>(request.codec);
        if (const auto started = talk->start(loginHandle_, request); !started) {
            return Output::failure(started.error());
        }
        const std::scoped_lock lock(mutex_);
        std::erase_if(talks_, [](const std::weak_ptr<ITalkSession>& entry) { return entry.expired(); });
        talks_.emplace_back(talk);
        return Output::success(std::move(talk));
    }

private:
    void stopAll() {
        std::vector<std::weak_ptr<ILiveStream>> streams;
        std::vector<std::weak_ptr<ITalkSession>> talks;
        {
            const std::scoped_lock lock(mutex_);
            streams.swap(streams_);
            talks.swap(talks_);
        }
        for (const auto& entry : talks) {
            if (const auto talk = entry.lock()) {
                talk->stop();
            }
        }
        for (const auto& entry : streams) {
            if (const auto stream = entry.lock()) {
                stream->stop();
            }
        }
    }

    std::shared_ptr<NetSdkRuntime> runtime_;
    DeviceId id_;
    DeviceInfo info_;
    LLONG loginHandle_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<ILiveStream>> streams_;
    std::vector<std::weak_ptr<ITalkSession>> talks_;
};

}

SdkSessionFactory::SdkSessionFactory(std::shared_ptr<NetSdkRuntime> runtime) : runtime_(std::move(runtime)) {}

domain::Result<std::unique_ptr<IDeviceSession>> SdkSessionFactory::connect(const RegistrationEvent& event,
                                                                          const Credentials& credentials) {
    using Output = domain::Result<std::unique_ptr<IDeviceSession>>;

    NET_IN_LOGIN_WITH_HIGHLEVEL_SECURITY input{};
    input.dwSize = sizeof(input);
    detail::copyToFixed(input.szIP, event.remoteAddress);
    input.nPort = event.remotePort;
    detail::copyToFixed(input.szUserName, credentials.username());
    detail::copyToFixed(input.szPassword, credentials.password());
    input.emSpecCap = EM_LOGIN_SPEC_CAP_SERVER_CONN;

    std::string identifier = event.deviceId.value;
    input.pCapParam = identifier.data();

    NET_OUT_LOGIN_WITH_HIGHLEVEL_SECURITY output{};
    output.dwSize = sizeof(output);

    const LLONG handle = CLIENT_LoginWithHighLevelSecurity(&input, &output);
    detail::secureWipe(std::span<char>(input.szPassword));

    if (handle == 0) {
        auto error = loginErrorFromNative(output.nError);
        error.message += " (device " + event.deviceId.value + ")";
        return Output::failure(std::move(error));
    }

    DeviceInfo info;
    info.serialNumber = detail::readFixed(output.stuDeviceInfo.sSerialNumber);
    info.channelCount = output.stuDeviceInfo.nChanNum;
    info.alarmInputCount = output.stuDeviceInfo.nAlarmInPortNum;
    info.alarmOutputCount = output.stuDeviceInfo.nAlarmOutPortNum;
    info.diskCount = output.stuDeviceInfo.nDiskNum;
    info.deviceType = output.stuDeviceInfo.nDVRType;

    return Output::success(
        std::make_unique<SdkDeviceSession>(runtime_, event.deviceId, std::move(info), handle));
}

StaticCredentialProvider::StaticCredentialProvider(Credentials credentials) : credentials_(std::move(credentials)) {}

std::optional<Credentials> StaticCredentialProvider::find(const DeviceId&) const { return credentials_; }

}
