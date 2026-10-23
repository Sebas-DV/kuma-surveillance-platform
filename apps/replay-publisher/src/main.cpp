#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "dahua/DhavParser.hpp"
#include "whip/Publisher.hpp"

namespace {

struct StoredPacket {
    dahua::MediaPacket packet;
    std::vector<std::byte> payload;
};

std::vector<std::byte> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](const char value) { return static_cast<std::byte>(value); });
    return bytes;
}

int run(const int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "replay-publisher FILE.dav WHIP_URL [--seconds N] [--loop]\n";
        return 2;
    }
    int seconds = 30;
    bool loop = false;
    for (int index = 3; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--loop") {
            loop = true;
        } else if (argument == "--seconds" && index + 1 < argc) {
            seconds = std::stoi(argv[++index]);
        }
    }

    const auto data = readFile(argv[1]);
    std::vector<StoredPacket> packets;
    dahua::DhavParser parser([&packets](const dahua::MediaPacket& packet) {
        StoredPacket stored;
        stored.packet = packet;
        stored.payload.assign(packet.payload.begin(), packet.payload.end());
        packets.push_back(std::move(stored));
    });
    parser.feed(data);
    for (auto& stored : packets) {
        stored.packet.payload = stored.payload;
    }

    dahua::VideoCodec codec = dahua::VideoCodec::Unknown;
    for (const auto& stored : packets) {
        if (stored.packet.kind == dahua::MediaKind::Video && stored.packet.video.codec != dahua::VideoCodec::Unknown) {
            codec = stored.packet.video.codec;
            break;
        }
    }
    std::cout << "packets=" << packets.size() << " codec=" << (codec == dahua::VideoCodec::H265 ? "h265" : "h264")
              << "\n";

    whip::PublisherConfig config;
    config.endpoint = argv[2];
    config.video = codec;
    auto publisher = whip::Publisher::connect(config);
    if (!publisher) {
        std::cerr << "connect failed: " << publisher.error().message << "\n";
        return 1;
    }
    std::cout << "connected\n";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::int64_t offsetMs = 0;
    do {
        const auto started = std::chrono::steady_clock::now();
        const std::int64_t first = packets.empty() ? 0 : packets.front().packet.timestampMs;
        std::int64_t last = first;
        for (auto& stored : packets) {
            if (std::chrono::steady_clock::now() >= deadline || !publisher.value()->connected()) {
                break;
            }
            const auto due = started + std::chrono::milliseconds(stored.packet.timestampMs - first);
            std::this_thread::sleep_until(due);
            auto shifted = stored.packet;
            shifted.timestampMs += offsetMs;
            publisher.value()->push(shifted);
            last = stored.packet.timestampMs;
        }
        offsetMs += last - first + 40;
    } while (loop && std::chrono::steady_clock::now() < deadline && publisher.value()->connected());

    const auto stats = publisher.value()->stats();
    std::cout << "video=" << stats.videoFrames << " audio=" << stats.audioFrames << " dropped=" << stats.droppedFrames
              << " connected=" << publisher.value()->connected() << "\n";
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& exception) {
        static_cast<void>(std::fputs("fatal: ", stderr));
        static_cast<void>(std::fputs(exception.what(), stderr));
        static_cast<void>(std::fputc('\n', stderr));
        return 1;
    }
}
