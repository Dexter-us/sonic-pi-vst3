// SPDX-License-Identifier: GPL-3.0-or-later
#include "socket_platform.h"
#include "clockwork_tcp_osc_bridge.h"
#include "clockwork_audio_file.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace SonicPiVst3 {
namespace {

constexpr std::uint32_t kMaxOscFrameBytes = 8u * 1024u * 1024u;
constexpr std::size_t kPollBatchSize = 64;

std::uint32_t readU32BE(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

void appendU32BE(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void appendU64BE(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    appendU32BE(bytes, static_cast<std::uint32_t>(value >> 32u));
    appendU32BE(bytes, static_cast<std::uint32_t>(value));
}

void appendOscString(std::vector<std::uint8_t>& packet,
                     std::string_view value) {
    packet.insert(packet.end(), value.begin(), value.end());
    packet.push_back(0);
    while (packet.size() % 4 != 0) packet.push_back(0);
}

std::vector<std::uint8_t> makeNotifyReply() {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, "/supersonic/notify.reply");
    appendOscString(packet, ",ss");
    appendOscString(packet, "clockwork-embed");
    appendOscString(packet, "0.91.0");
    return packet;
}

struct ParsedOscMessage {
    std::string_view address;
    std::vector<std::string_view> stringArguments;
    std::vector<std::int32_t> intArguments;
    std::vector<std::int64_t> int64Arguments;
    std::vector<float> floatArguments;
};

bool parseOscMessage(const std::uint8_t* packet, std::uint32_t bytes,
                     ParsedOscMessage& parsed) {
    std::size_t offset = 0;
    const auto readString = [&packet, bytes, &offset](std::string_view& value) {
        if (offset >= bytes) return false;
        const auto* start = packet + offset;
        const auto* end = static_cast<const std::uint8_t*>(
            std::memchr(start, 0, bytes - offset));
        if (!end) return false;
        value = std::string_view(
            reinterpret_cast<const char*>(start),
            static_cast<std::size_t>(end - start));
        offset = (static_cast<std::size_t>(end - packet) + 4u) & ~std::size_t{3u};
        return offset <= bytes;
    };

    std::string_view tags;
    if (!readString(parsed.address) || !readString(tags) ||
        tags.empty() || tags.front() != ',') {
        return false;
    }

    for (std::size_t i = 1; i < tags.size(); ++i) {
        switch (tags[i]) {
        case 's': {
            std::string_view value;
            if (!readString(value)) return false;
            parsed.stringArguments.push_back(value);
            break;
        }
        case 'b': {
            if (offset + 4u > bytes) return false;
            const auto blobBytes = readU32BE(packet + offset);
            offset += 4u;
            const auto paddedBytes =
                (static_cast<std::size_t>(blobBytes) + 3u) & ~std::size_t{3u};
            if (paddedBytes > bytes - offset) return false;
            offset += paddedBytes;
            break;
        }
        case 'i':
            if (offset + 4u > bytes) return false;
            parsed.intArguments.push_back(
                static_cast<std::int32_t>(readU32BE(packet + offset)));
            offset += 4u;
            break;
        case 'f':
            if (offset + 4u > bytes) return false;
            parsed.floatArguments.push_back(
                std::bit_cast<float>(readU32BE(packet + offset)));
            offset += 4u;
            break;
        case 'h': {
            if (offset + 8u > bytes) return false;
            const auto value =
                (static_cast<std::uint64_t>(readU32BE(packet + offset)) << 32u) |
                readU32BE(packet + offset + 4u);
            parsed.int64Arguments.push_back(std::bit_cast<std::int64_t>(value));
            offset += 8u;
            break;
        }
        case 'd':
        case 't':
            if (offset + 8u > bytes) return false;
            offset += 8u;
            break;
        case 'T':
        case 'F':
        case 'N':
        case 'I':
            break;
        default:
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> makeDRecvPacket(
    const std::vector<std::uint8_t>& synthdef) {
    std::vector<std::uint8_t> packet;
    packet.reserve(synthdef.size() + 24u);
    appendOscString(packet, "/d_recv");
    appendOscString(packet, ",b");
    appendU32BE(packet, static_cast<std::uint32_t>(synthdef.size()));
    packet.insert(packet.end(), synthdef.begin(), synthdef.end());
    while ((packet.size() & 3u) != 0u) packet.push_back(0);
    return packet;
}

std::vector<std::uint8_t> makeDonePacket(std::string_view command) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, "/done");
    appendOscString(packet, ",s");
    appendOscString(packet, command);
    return packet;
}

std::vector<std::uint8_t> makeDonePacket(std::string_view command,
                                         std::int32_t buffer) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, "/done");
    appendOscString(packet, ",si");
    appendOscString(packet, command);
    appendU32BE(packet, static_cast<std::uint32_t>(buffer));
    return packet;
}

std::vector<std::uint8_t> makeFailPacket(std::string_view command,
                                         std::string_view reason) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, "/fail");
    appendOscString(packet, ",ss");
    appendOscString(packet, command);
    appendOscString(packet, reason);
    return packet;
}

void appendFloatBE(std::vector<std::uint8_t>& packet, float value) {
    appendU32BE(packet, std::bit_cast<std::uint32_t>(value));
}

void appendInt64BE(std::vector<std::uint8_t>& packet, std::int64_t value) {
    appendU64BE(packet, std::bit_cast<std::uint64_t>(value));
}

std::vector<std::uint8_t> makeClockFloatReply(
    std::string_view address, float value, std::int32_t token) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, address);
    appendOscString(packet, ",fi");
    appendFloatBE(packet, value);
    appendU32BE(packet, static_cast<std::uint32_t>(token));
    return packet;
}

std::vector<std::uint8_t> makeClockTokenReply(
    std::string_view address, std::int32_t token) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, address);
    appendOscString(packet, ",i");
    appendU32BE(packet, static_cast<std::uint32_t>(token));
    return packet;
}

std::vector<std::uint8_t> makeClockIntReply(
    std::string_view address, std::int32_t value, std::int32_t token) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, address);
    appendOscString(packet, ",ii");
    appendU32BE(packet, static_cast<std::uint32_t>(value));
    appendU32BE(packet, static_cast<std::uint32_t>(token));
    return packet;
}

std::vector<std::uint8_t> makeClockInt64Reply(
    std::string_view address, std::int64_t value, std::int32_t token) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, address);
    appendOscString(packet, ",hi");
    appendInt64BE(packet, value);
    appendU32BE(packet, static_cast<std::uint32_t>(token));
    return packet;
}

std::vector<std::uint8_t> makeClockNowReply(
    std::string_view address, std::int64_t nowMicros, float beat,
    float phase, std::int32_t token) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, address);
    appendOscString(packet, ",hffi");
    appendInt64BE(packet, nowMicros);
    appendFloatBE(packet, beat);
    appendFloatBE(packet, phase);
    appendU32BE(packet, static_cast<std::uint32_t>(token));
    return packet;
}

std::vector<std::uint8_t> makeClockNotification(
    std::string_view address, float value) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, address);
    appendOscString(packet, ",f");
    appendFloatBE(packet, value);
    return packet;
}

constexpr std::int64_t kNtpEpochOffsetMicros =
    2'208'988'800LL * 1'000'000LL;

std::int64_t wallClockNtpMicros() {
    const auto unixMicros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return unixMicros + kNtpEpochOffsetMicros;
}

std::vector<std::uint8_t> makeBufferAllocPacket(
    std::int32_t buffer, std::uint32_t frames, std::uint32_t channels) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, "/b_alloc");
    appendOscString(packet, ",iii");
    appendU32BE(packet, static_cast<std::uint32_t>(buffer));
    appendU32BE(packet, frames);
    appendU32BE(packet, channels);
    return packet;
}

std::vector<std::uint8_t> makeBufferSetnPacket(
    std::int32_t buffer, std::uint32_t start,
    const float* samples, std::uint32_t count) {
    std::vector<std::uint8_t> packet;
    packet.reserve(24u + static_cast<std::size_t>(count) * 8u);
    appendOscString(packet, "/b_setn");
    std::string tags = ",iii";
    tags.append(count, 'f');
    appendOscString(packet, tags);
    appendU32BE(packet, static_cast<std::uint32_t>(buffer));
    appendU32BE(packet, start);
    appendU32BE(packet, count);
    for (std::uint32_t i = 0; i < count; ++i) appendFloatBE(packet, samples[i]);
    return packet;
}

std::vector<std::uint8_t> makeBufferFreePacket(std::int32_t buffer) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, "/b_free");
    appendOscString(packet, ",i");
    appendU32BE(packet, static_cast<std::uint32_t>(buffer));
    return packet;
}

std::vector<std::uint8_t> makeBufferInfoPacket(
    std::int32_t buffer, std::uint32_t frames, std::uint32_t channels,
    float sampleRate) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, "/b_info");
    appendOscString(packet, ",iiif");
    appendU32BE(packet, static_cast<std::uint32_t>(buffer));
    appendU32BE(packet, frames);
    appendU32BE(packet, channels);
    appendFloatBE(packet, sampleRate);
    return packet;
}

std::vector<std::uint8_t> makeSyncPacket(std::int32_t syncId) {
    std::vector<std::uint8_t> packet;
    appendOscString(packet, "/sync");
    appendOscString(packet, ",i");
    appendU32BE(packet, static_cast<std::uint32_t>(syncId));
    return packet;
}

struct DecodedSamples {
    std::vector<float> interleaved;
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
};

bool decodeSamples(const std::string& path, std::int32_t requestedStart,
                   std::int32_t requestedFrames,
                   const std::vector<std::int32_t>& selectedChannels,
                   double outputSampleRate, DecodedSamples& output,
                   std::string& error) {
    ClockworkAudioInfo info{};
    info.struct_bytes = sizeof(info);
    float* decoded = nullptr;
    const auto status = clockwork_audio_decode_file(path.c_str(), &info, &decoded);
    if (status != CLOCKWORK_OK || !decoded) {
        error = "Audio file could not be decoded: " + path;
        return false;
    }

    const auto release = [&decoded] {
        if (decoded) clockwork_audio_free(decoded);
        decoded = nullptr;
    };
    if (info.frames == 0 || info.channels == 0 || info.sample_rate == 0 ||
        !(outputSampleRate > 0.0)) {
        release();
        error = "Audio file has invalid rate, channel, or frame metadata: " + path;
        return false;
    }

    const auto totalFrames = static_cast<std::int64_t>(info.frames);
    const auto start = std::clamp<std::int64_t>(
        requestedStart < 0 ? 0 : requestedStart, 0, totalFrames);
    auto sourceFrames = static_cast<std::int64_t>(requestedFrames);
    if (sourceFrames <= 0 || start + sourceFrames > totalFrames) {
        sourceFrames = totalFrames - start;
    }
    if (sourceFrames <= 0) {
        release();
        error = "Audio file contains no frames in the requested range: " + path;
        return false;
    }

    std::vector<std::uint32_t> channels;
    if (selectedChannels.empty()) {
        channels.reserve(info.channels);
        for (std::uint32_t channel = 0; channel < info.channels; ++channel) {
            channels.push_back(channel);
        }
    } else {
        channels.reserve(selectedChannels.size());
        for (const auto channel : selectedChannels) {
            if (channel < 0 || static_cast<std::uint32_t>(channel) >= info.channels) {
                release();
                error = "Requested audio channel is out of range: " + path;
                return false;
            }
            channels.push_back(static_cast<std::uint32_t>(channel));
        }
    }

    const long double scaledFrames =
        static_cast<long double>(sourceFrames) * outputSampleRate / info.sample_rate;
    if (!(scaledFrames > 0.0L) ||
        scaledFrames > std::numeric_limits<std::uint32_t>::max()) {
        release();
        error = "Audio file is too large to load into a Sonic Pi buffer: " + path;
        return false;
    }
    const auto outputFrames = static_cast<std::uint32_t>(std::llround(scaledFrames));
    const std::uint64_t outputSamples =
        static_cast<std::uint64_t>(outputFrames) * channels.size();
    if (outputFrames == 0 || outputSamples >
            static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        release();
        error = "Audio file is too large to load into a Sonic Pi buffer: " + path;
        return false;
    }

    output.frames = outputFrames;
    output.channels = static_cast<std::uint32_t>(channels.size());
    output.interleaved.resize(static_cast<std::size_t>(outputSamples));
    const double sourceRate = static_cast<double>(info.sample_rate);
    for (std::uint32_t frame = 0; frame < outputFrames; ++frame) {
        const double sourcePosition =
            static_cast<double>(frame) * sourceRate / outputSampleRate;
        const auto sourceOffset = std::min<std::int64_t>(
            static_cast<std::int64_t>(sourcePosition), sourceFrames - 1);
        const auto nextOffset = std::min(sourceOffset + 1, sourceFrames - 1);
        const float mix = static_cast<float>(sourcePosition - sourceOffset);
        for (std::size_t channel = 0; channel < channels.size(); ++channel) {
            const auto sourceChannel = channels[channel];
            const float first = decoded[
                static_cast<std::size_t>(start + sourceOffset) * info.channels +
                sourceChannel];
            const float second = decoded[
                static_cast<std::size_t>(start + nextOffset) * info.channels +
                sourceChannel];
            output.interleaved[static_cast<std::size_t>(frame) * channels.size() +
                               channel] = first + (second - first) * mix;
        }
    }
    release();
    return true;
}

} // namespace

struct ClockworkTcpOscBridge::Impl {
    struct Connection {
        SocketPlatform::Socket fd = SocketPlatform::kInvalidSocket;
        std::uint32_t origin = 0;
        std::vector<std::uint8_t> incoming;
        std::deque<std::vector<std::uint8_t>> outgoing;
        std::size_t outgoingOffset = 0;
    };

    struct PendingFileLoad {
        std::string command;
        std::uint32_t remainingDefinitions = 0;
    };

    struct PendingSampleLoad {
        enum class Stage { Allocating, Sending, WaitingSync };
        std::string command;
        std::vector<float> samples;
        std::uint32_t channels = 0;
        std::uint32_t nextSample = 0;
        std::int32_t syncId = 0;
        Stage stage = Stage::Allocating;
    };

    struct SampleBufferInfo {
        std::uint32_t frames = 0;
        std::uint32_t channels = 0;
        double sampleRate = 0.0;
    };

    ClockworkClient* client = nullptr;
    SocketPlatform::Socket listener = SocketPlatform::kInvalidSocket;
    std::uint16_t listenPort = 0;
    std::atomic<bool> running{false};
    std::thread ioThread;
    std::uint32_t nextOrigin = 1;
    std::map<SocketPlatform::Socket, Connection> connections;
    std::map<std::uint32_t, std::deque<PendingFileLoad>> pendingFileLoads;
    std::map<std::pair<std::uint32_t, std::int32_t>, PendingSampleLoad>
        pendingSampleLoads;
    std::map<std::pair<std::uint32_t, std::int32_t>, SampleBufferInfo>
        sampleBufferInfo;
    double sampleRate = 48000.0;
    double clockTempo = 60.0;
    double clockEpochBeat = 0.0;
    std::int64_t clockEpochNtpMicros = 0;
    std::int32_t nextSyncId = 1;

    bool openListener() {
        listener = SocketPlatform::create(AF_INET, SOCK_STREAM, 0);
        if (listener == SocketPlatform::kInvalidSocket) return false;

        int reuse = 1;
        SocketPlatform::setOption(listener, SOL_SOCKET, SO_REUSEADDR,
                                  &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address),
                   static_cast<SocketPlatform::SocketLength>(sizeof(address))) != 0 ||
            ::listen(listener, 8) != 0 ||
            !SocketPlatform::setNonBlocking(listener)) {
            SocketPlatform::close(listener);
            listener = SocketPlatform::kInvalidSocket;
            return false;
        }

        SocketPlatform::SocketLength size = sizeof(address);
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            SocketPlatform::close(listener);
            listener = SocketPlatform::kInvalidSocket;
            return false;
        }
        listenPort = ntohs(address.sin_port);
        return listenPort != 0;
    }

    bool start(ClockworkClient* clockworkClient,
               double engineSampleRate) noexcept {
        if (!clockworkClient || running.load()) return false;
        if (!(engineSampleRate > 0.0)) return false;
        if (!SocketPlatform::initialize()) return false;
        sampleRate = engineSampleRate;
        clockTempo = 60.0;
        clockEpochBeat = 0.0;
        clockEpochNtpMicros = wallClockNtpMicros();
        client = clockworkClient;
        if (!openListener()) return false;

        running.store(true);
        try {
            ioThread = std::thread([this] { run(); });
        } catch (...) {
            running.store(false);
            SocketPlatform::close(listener);
            listener = SocketPlatform::kInvalidSocket;
            listenPort = 0;
            return false;
        }
        return true;
    }

    void stop() noexcept {
        running.store(false);
        if (ioThread.joinable()) ioThread.join();
        SocketPlatform::close(listener);
        listener = SocketPlatform::kInvalidSocket;
        listenPort = 0;
        client = nullptr;
        connections.clear();
        pendingFileLoads.clear();
        pendingSampleLoads.clear();
        sampleBufferInfo.clear();
        nextOrigin = 1;
    }

    void acceptConnections() {
        for (;;) {
            sockaddr_in peer{};
            SocketPlatform::SocketLength peerSize = sizeof(peer);
            const auto fd = SocketPlatform::accept(
                listener, reinterpret_cast<sockaddr*>(&peer), &peerSize);
            if (fd == SocketPlatform::kInvalidSocket) {
                const int error = SocketPlatform::lastError();
                if (SocketPlatform::interrupted(error)) continue;
                return;
            }
            if (!SocketPlatform::setNonBlocking(fd)) {
                SocketPlatform::close(fd);
                continue;
            }
#ifdef SO_NOSIGPIPE
            int noSigPipe = 1;
            SocketPlatform::setOption(fd, SOL_SOCKET, SO_NOSIGPIPE,
                                      &noSigPipe, sizeof(noSigPipe));
#endif
            int noDelay = 1;
            SocketPlatform::setOption(fd, IPPROTO_TCP, TCP_NODELAY,
                                      &noDelay, sizeof(noDelay));

            auto origin = nextOrigin++;
            if (origin == 0) origin = nextOrigin++;
            Connection connection;
            connection.fd = fd;
            connection.origin = origin;
            connections.emplace(fd, std::move(connection));
        }
    }

    bool sendToClockwork(const std::uint8_t* packet, std::uint32_t bytes,
                         std::uint32_t origin) {
        for (int attempt = 0; attempt < 30; ++attempt) {
            const auto status = clockwork_client_send(client, packet, bytes, origin);
            if (status == CLOCKWORK_OK) return true;
            if (status != CLOCKWORK_E_FULL) {
                std::fprintf(stderr, "Sonic Pi VST: Clockwork rejected OSC: %s\n",
                             clockwork_client_status_text(status));
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::fprintf(stderr, "Sonic Pi VST: Clockwork input queue stayed full\n");
        return false;
    }

    void enqueuePacket(Connection& connection,
                       const std::vector<std::uint8_t>& packet) {
        if (packet.empty() || packet.size() > kMaxOscFrameBytes) return;
        std::vector<std::uint8_t> frame;
        frame.reserve(packet.size() + 4u);
        appendU32BE(frame, static_cast<std::uint32_t>(packet.size()));
        frame.insert(frame.end(), packet.begin(), packet.end());
        connection.outgoing.push_back(std::move(frame));
    }

    bool handleSuperSonicNotify(Connection& connection,
                                const std::uint8_t* packet,
                                std::uint32_t bytes) {
        const auto* addressEnd = static_cast<const std::uint8_t*>(
            std::memchr(packet, 0, bytes));
        if (!addressEnd) return false;
        const std::string_view address(
            reinterpret_cast<const char*>(packet),
            static_cast<std::size_t>(addressEnd - packet));
        if (address == "/supersonic/notify") {
            // Spider requires this process-level handshake. ClockworkEmbed
            // exposes scsynth OSC, but not SuperSonic's daemon notification
            // registry, so the bridge supplies a stable engine/version reply.
            const auto reply = makeNotifyReply();
            enqueuePacket(connection, reply);
            return true;
        }
        if (address == "/supersonic/notify/unregister") return true;
        return false;
    }

    bool handleFileLoad(Connection& connection,
                        const std::uint8_t* packet,
                        std::uint32_t bytes) {
        ParsedOscMessage parsed;
        if (!parseOscMessage(packet, bytes, parsed) ||
            (parsed.address != "/d_loadDir" && parsed.address != "/d_load")) {
            return false;
        }

        const std::string command(parsed.address);
        const auto fail = [this, &connection, &command](const std::string& reason) {
            enqueuePacket(connection, makeFailPacket(command, reason));
        };
        if (parsed.stringArguments.empty() || parsed.stringArguments[0].empty()) {
            fail("Missing synth definition path");
            return true;
        }

        const std::filesystem::path path(parsed.stringArguments[0]);
        std::vector<std::filesystem::path> files;
        std::error_code error;
        if (command == "/d_loadDir") {
            if (!std::filesystem::is_directory(path, error) || error) {
                fail("Synth definition directory is unavailable: " + path.string());
                return true;
            }
            for (std::filesystem::directory_iterator it(path, error), end;
                 !error && it != end; it.increment(error)) {
                std::error_code fileError;
                if (it->path().extension() == ".scsyndef" &&
                    it->is_regular_file(fileError) && !fileError) {
                    // This GUI-only synth writes to SuperSonic's separate
                    // audio-output lane; the VST returns Clockwork's render
                    // buffers directly to its host instead.
                    if (it->path().filename() ==
                        "supersonic-audio-out.scsyndef") {
                        continue;
                    }
                    files.push_back(it->path());
                }
            }
            if (error) {
                fail("Could not enumerate synth definitions: " + error.message());
                return true;
            }
            std::sort(files.begin(), files.end());
        } else {
            if (!std::filesystem::is_regular_file(path, error) || error) {
                fail("Synth definition file is unavailable: " + path.string());
                return true;
            }
            files.push_back(path);
        }

        if (files.empty()) {
            fail("No .scsyndef files found at: " + path.string());
            return true;
        }

        auto& pending = pendingFileLoads[connection.origin];
        pending.push_back({command, static_cast<std::uint32_t>(files.size())});
        for (const auto& file : files) {
            std::ifstream input(file, std::ios::binary);
            if (!input) {
                pending.pop_back();
                if (pending.empty()) pendingFileLoads.erase(connection.origin);
                fail("Could not read synth definition: " + file.string());
                return true;
            }
            std::vector<std::uint8_t> bytesOnDisk;
            bytesOnDisk.assign(std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>());
            if (bytesOnDisk.empty() || bytesOnDisk.size() > kMaxOscFrameBytes) {
                pending.pop_back();
                if (pending.empty()) pendingFileLoads.erase(connection.origin);
                fail("Synth definition has an unsupported size: " + file.string());
                return true;
            }
            const auto synthdefPacket = makeDRecvPacket(bytesOnDisk);
            if (!sendToClockwork(synthdefPacket.data(),
                                 static_cast<std::uint32_t>(synthdefPacket.size()),
                                 connection.origin)) {
                pending.pop_back();
                if (pending.empty()) pendingFileLoads.erase(connection.origin);
                fail("Embedded engine could not queue synth definition: " +
                     file.filename().string());
                return true;
            }
        }
        return true;
    }

    bool handleBufferFileLoad(Connection& connection,
                              const std::uint8_t* packet,
                              std::uint32_t bytes) {
        ParsedOscMessage parsed;
        if (!parseOscMessage(packet, bytes, parsed) ||
            (parsed.address != "/b_allocRead" &&
             parsed.address != "/b_allocReadChannel")) {
            return false;
        }
        const std::string command(parsed.address);
        const auto fail = [this, &connection, &command](const std::string& reason) {
            enqueuePacket(connection, makeFailPacket(command, reason));
        };
        if (parsed.stringArguments.empty() ||
            parsed.intArguments.empty()) {
            fail("Missing audio buffer number or file path");
            return true;
        }

        const std::int32_t buffer = parsed.intArguments[0];
        const std::int32_t start =
            parsed.intArguments.size() > 1 ? parsed.intArguments[1] : 0;
        const std::int32_t frames =
            parsed.intArguments.size() > 2 ? parsed.intArguments[2] : 0;
        std::vector<std::int32_t> channels;
        if (command == "/b_allocReadChannel") {
            if (parsed.intArguments.size() < 4) {
                fail("Missing channel selection for /b_allocReadChannel");
                return true;
            }
            const auto channelCount = parsed.intArguments[3];
            if (channelCount < 0 ||
                parsed.intArguments.size() <
                    static_cast<std::size_t>(4 + channelCount)) {
                fail("Invalid channel selection for /b_allocReadChannel");
                return true;
            }
            channels.assign(parsed.intArguments.begin() + 4,
                            parsed.intArguments.begin() + 4 + channelCount);
        }

        DecodedSamples decoded;
        std::string reason;
        if (!decodeSamples(std::string(parsed.stringArguments[0]), start, frames,
                           channels, sampleRate, decoded, reason)) {
            fail(reason);
            return true;
        }

        const auto key = std::make_pair(connection.origin, buffer);
        if (pendingSampleLoads.find(key) != pendingSampleLoads.end()) {
            fail("A load for this Sonic Pi buffer is already in progress");
            return true;
        }
        PendingSampleLoad load;
        load.command = command;
        load.samples = std::move(decoded.interleaved);
        load.channels = decoded.channels;
        auto [it, inserted] = pendingSampleLoads.emplace(key, std::move(load));
        if (!inserted) {
            fail("Could not track Sonic Pi audio buffer load");
            return true;
        }

        const auto freePacket = makeBufferFreePacket(buffer);
        const auto allocationPacket = makeBufferAllocPacket(
            buffer, decoded.frames, decoded.channels);
        if (!sendToClockwork(freePacket.data(),
                             static_cast<std::uint32_t>(freePacket.size()),
                             connection.origin) ||
            !sendToClockwork(allocationPacket.data(),
                             static_cast<std::uint32_t>(allocationPacket.size()),
                             connection.origin)) {
            enqueuePacket(connection, makeFailPacket(
                command, "Embedded engine could not allocate the audio buffer"));
            pendingSampleLoads.erase(it);
        }
        return true;
    }

    bool handleBufferQuery(Connection& connection,
                           const std::uint8_t* packet,
                           std::uint32_t bytes) {
        ParsedOscMessage parsed;
        if (!parseOscMessage(packet, bytes, parsed) ||
            parsed.address != "/b_query" || parsed.intArguments.empty()) {
            return false;
        }
        const auto it = sampleBufferInfo.find(
            std::make_pair(connection.origin, parsed.intArguments[0]));
        if (it == sampleBufferInfo.end()) return false;
        enqueuePacket(connection, makeBufferInfoPacket(
            parsed.intArguments[0], it->second.frames, it->second.channels,
            static_cast<float>(it->second.sampleRate)));
        return true;
    }

    double clockBeatAt(std::int64_t timeMicros) const {
        const double tempo = clockTempo > 0.0 ? clockTempo : 60.0;
        return clockEpochBeat +
            (static_cast<double>(timeMicros - clockEpochNtpMicros) *
             tempo / 60'000'000.0);
    }

    std::int64_t clockTimeAt(double beat) const {
        const double tempo = clockTempo > 0.0 ? clockTempo : 60.0;
        const double deltaMicros =
            (beat - clockEpochBeat) * 60'000'000.0 / tempo;
        return clockEpochNtpMicros +
            static_cast<std::int64_t>(std::llround(deltaMicros));
    }

    double clockPhaseAt(double beat, double quantum) const {
        if (!(quantum > 0.0)) quantum = 4.0;
        double phase = std::fmod(beat, quantum);
        if (phase < 0.0) phase += quantum;
        return phase;
    }

    bool handleClockMessage(Connection& connection,
                            const std::uint8_t* packet,
                            std::uint32_t bytes) {
        ParsedOscMessage parsed;
        if (!parseOscMessage(packet, bytes, parsed) ||
            !parsed.address.starts_with("/clock/")) {
            return false;
        }

        const std::string address(parsed.address);
        const bool hasToken = !parsed.intArguments.empty();
        const std::int32_t token = hasToken ? parsed.intArguments.back() : 0;
        const double quantum = parsed.floatArguments.empty()
            ? 4.0 : parsed.floatArguments.front();

        if (parsed.address == "/clock/notify/subscribe") {
            if (hasToken) {
                enqueuePacket(connection, makeClockTokenReply(
                    address + ".reply", token));
            }
            return true;
        }
        if (parsed.address == "/clock/notify/unsubscribe" ||
            parsed.address == "/clock/start_stop_sync/set") {
            return true;
        }
        if (parsed.address == "/clock/tempo/set") {
            const double newTempo = parsed.floatArguments.empty()
                ? clockTempo : parsed.floatArguments.front();
            if (newTempo > 0.0 && std::isfinite(newTempo)) {
                const auto now = wallClockNtpMicros();
                clockEpochBeat = clockBeatAt(now);
                clockEpochNtpMicros = now;
                clockTempo = newTempo;
                enqueuePacket(connection, makeClockNotification(
                    "/clock/notify/tempo", static_cast<float>(clockTempo)));
            }
            return true;
        }

        const auto replyForGet = [&address] {
            std::string reply = address;
            if (reply.size() >= 4 &&
                reply.compare(reply.size() - 4, 4, "/get") == 0) {
                reply.replace(reply.size() - 4, 4, ".reply");
            }
            return reply;
        };
        if (parsed.address == "/clock/tempo/get" && hasToken) {
            enqueuePacket(connection, makeClockFloatReply(
                replyForGet(), static_cast<float>(clockTempo), token));
            return true;
        }
        if (parsed.address == "/clock/start_stop_sync/get" && hasToken) {
            enqueuePacket(connection, makeClockIntReply(replyForGet(), 0, token));
            return true;
        }
        if (parsed.address == "/clock/peers/count/get" && hasToken) {
            enqueuePacket(connection, makeClockIntReply(replyForGet(), 0, token));
            return true;
        }
        if (parsed.address == "/clock/timelines/get" && hasToken) {
            enqueuePacket(connection, makeClockTokenReply(replyForGet(), token));
            return true;
        }
        if ((parsed.address == "/clock/time/now/get" ||
             parsed.address == "/clock/transport/time/get") && hasToken) {
            enqueuePacket(connection, makeClockInt64Reply(
                replyForGet(), wallClockNtpMicros(), token));
            return true;
        }

        if (parsed.address == "/clock/rpc/beat_at_time" &&
            !parsed.int64Arguments.empty() && hasToken) {
            enqueuePacket(connection, makeClockFloatReply(
                address + ".reply",
                static_cast<float>(clockBeatAt(parsed.int64Arguments[0])),
                token));
            return true;
        }
        if (parsed.address == "/clock/rpc/phase_at_time" &&
            !parsed.int64Arguments.empty() && hasToken) {
            const double beat = clockBeatAt(parsed.int64Arguments[0]);
            enqueuePacket(connection, makeClockFloatReply(
                address + ".reply",
                static_cast<float>(clockPhaseAt(beat, quantum)), token));
            return true;
        }
        if (parsed.address == "/clock/rpc/time_at_beat" &&
            !parsed.int64Arguments.empty() && hasToken) {
            const double beat =
                static_cast<double>(parsed.int64Arguments[0]) / 1'000'000.0;
            enqueuePacket(connection, makeClockInt64Reply(
                address + ".reply", clockTimeAt(beat), token));
            return true;
        }
        if (parsed.address == "/clock/rpc/beat_phase_now" && hasToken) {
            const auto now = wallClockNtpMicros();
            const double beat = clockBeatAt(now);
            std::vector<std::uint8_t> reply;
            appendOscString(reply, address + ".reply");
            appendOscString(reply, ",hffi");
            appendInt64BE(reply, now);
            appendFloatBE(reply, static_cast<float>(beat));
            appendFloatBE(reply,
                          static_cast<float>(clockPhaseAt(beat, quantum)));
            appendU32BE(reply, static_cast<std::uint32_t>(token));
            enqueuePacket(connection, reply);
            return true;
        }
        if (parsed.address == "/clock/rpc/beat_phase_at_time" &&
            !parsed.int64Arguments.empty() && hasToken) {
            const double beat = clockBeatAt(parsed.int64Arguments[0]);
            std::vector<std::uint8_t> reply;
            appendOscString(reply, address + ".reply");
            appendOscString(reply, ",ffi");
            appendFloatBE(reply, static_cast<float>(beat));
            appendFloatBE(reply,
                          static_cast<float>(clockPhaseAt(beat, quantum)));
            appendU32BE(reply, static_cast<std::uint32_t>(token));
            enqueuePacket(connection, reply);
            return true;
        }
        return false;
    }

    bool readConnection(Connection& connection) {
        std::array<std::uint8_t, 16384> chunk{};
        for (;;) {
            const auto count = SocketPlatform::receive(
                connection.fd, chunk.data(), chunk.size());
            if (count > 0) {
                connection.incoming.insert(connection.incoming.end(),
                                            chunk.begin(), chunk.begin() + count);
                if (connection.incoming.size() > kMaxOscFrameBytes + 4u) return false;
                continue;
            }
            if (count == 0) return false;
            const int error = SocketPlatform::lastError();
            if (SocketPlatform::interrupted(error)) continue;
            if (SocketPlatform::wouldBlock(error)) break;
            return false;
        }

        std::size_t consumed = 0;
        while (connection.incoming.size() - consumed >= 4) {
            const auto* frame = connection.incoming.data() + consumed;
            const auto payloadSize = readU32BE(frame);
            if (payloadSize == 0 || payloadSize > kMaxOscFrameBytes) return false;
            if (connection.incoming.size() - consumed < payloadSize + 4u) break;
            if (!handleSuperSonicNotify(connection, frame + 4, payloadSize) &&
                !handleFileLoad(connection, frame + 4, payloadSize) &&
                !handleBufferFileLoad(connection, frame + 4, payloadSize) &&
                !handleBufferQuery(connection, frame + 4, payloadSize) &&
                !handleClockMessage(connection, frame + 4, payloadSize) &&
                !sendToClockwork(frame + 4, payloadSize, connection.origin)) {
                return false;
            }
            consumed += payloadSize + 4u;
        }
        if (consumed != 0) {
            connection.incoming.erase(connection.incoming.begin(),
                                       connection.incoming.begin() +
                                           static_cast<std::ptrdiff_t>(consumed));
        }
        return true;
    }

    bool flushConnection(Connection& connection) {
        while (!connection.outgoing.empty()) {
            auto& frame = connection.outgoing.front();
            const auto remaining = frame.size() - connection.outgoingOffset;
#if defined(_WIN32)
            constexpr int kSendFlags = 0;
#elif defined(MSG_NOSIGNAL)
            constexpr int kSendFlags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
            constexpr int kSendFlags = MSG_DONTWAIT;
#endif
            const auto count = SocketPlatform::send(
                connection.fd, frame.data() + connection.outgoingOffset,
                remaining, kSendFlags);
            if (count > 0) {
                connection.outgoingOffset += static_cast<std::size_t>(count);
                if (connection.outgoingOffset == frame.size()) {
                    connection.outgoing.pop_front();
                    connection.outgoingOffset = 0;
                }
                continue;
            }
            if (count < 0) {
                const int error = SocketPlatform::lastError();
                if (SocketPlatform::interrupted(error)) continue;
                if (SocketPlatform::wouldBlock(error)) return true;
            }
            return false;
        }
        return true;
    }

    void enqueue(Connection& connection, const ClockworkClientMessage& message) {
        if (!message.bytes || message.length == 0 ||
            message.length > kMaxOscFrameBytes) {
            return;
        }
        std::vector<std::uint8_t> frame;
        frame.reserve(static_cast<std::size_t>(message.length) + 4u);
        appendU32BE(frame, message.length);
        frame.insert(frame.end(), message.bytes, message.bytes + message.length);
        connection.outgoing.push_back(std::move(frame));
    }

    bool completeFileLoad(const ClockworkClientMessage& message) {
        auto pendingIt = pendingFileLoads.find(message.origin);
        if (pendingIt == pendingFileLoads.end() || pendingIt->second.empty()) {
            return false;
        }
        ParsedOscMessage parsed;
        if (!parseOscMessage(message.bytes, message.length, parsed) ||
            (parsed.address != "/done" && parsed.address != "/fail") ||
            parsed.stringArguments.empty() ||
            parsed.stringArguments[0] != "/d_recv") {
            return false;
        }

        auto connectionIt = std::find_if(
            connections.begin(), connections.end(),
            [&message](const auto& entry) {
                return entry.second.origin == message.origin;
            });
        auto& jobs = pendingIt->second;
        auto& job = jobs.front();
        if (parsed.address == "/fail") {
            const std::string reason = parsed.stringArguments.size() > 1
                ? std::string(parsed.stringArguments[1])
                : "Embedded engine rejected a synth definition";
            if (connectionIt != connections.end()) {
                enqueuePacket(connectionIt->second,
                              makeFailPacket(job.command, reason));
            }
            jobs.pop_front();
        } else if (--job.remainingDefinitions == 0) {
            if (connectionIt != connections.end()) {
                enqueuePacket(connectionIt->second, makeDonePacket(job.command));
            }
            jobs.pop_front();
        }
        if (jobs.empty()) pendingFileLoads.erase(pendingIt);
        return true;
    }

    bool completeBufferLoad(const ClockworkClientMessage& message) {
        ParsedOscMessage parsed;
        if (!parseOscMessage(message.bytes, message.length, parsed)) return false;

        if (parsed.address == "/done" &&
            !parsed.stringArguments.empty() &&
            parsed.stringArguments[0] == "/b_alloc" &&
            !parsed.intArguments.empty()) {
            const auto key = std::make_pair(message.origin,
                                            parsed.intArguments[0]);
            const auto it = pendingSampleLoads.find(key);
            if (it == pendingSampleLoads.end()) return false;
            it->second.stage = PendingSampleLoad::Stage::Sending;
            return true;
        }

        if (parsed.address == "/synced" && !parsed.intArguments.empty()) {
            const auto syncId = parsed.intArguments[0];
            const auto it = std::find_if(
                pendingSampleLoads.begin(), pendingSampleLoads.end(),
                [&message, syncId](const auto& entry) {
                    return entry.first.first == message.origin &&
                           entry.second.stage ==
                               PendingSampleLoad::Stage::WaitingSync &&
                           entry.second.syncId == syncId;
                });
            if (it == pendingSampleLoads.end()) return false;
            const auto connectionIt = std::find_if(
                connections.begin(), connections.end(),
                [&message](const auto& entry) {
                    return entry.second.origin == message.origin;
                });
            if (connectionIt != connections.end()) {
                sampleBufferInfo[it->first] = SampleBufferInfo{
                    static_cast<std::uint32_t>(
                        it->second.samples.size() / it->second.channels),
                    it->second.channels, sampleRate};
                enqueuePacket(connectionIt->second,
                              makeDonePacket(it->second.command,
                                             it->first.second));
            }
            pendingSampleLoads.erase(it);
            return true;
        }

        if (parsed.address == "/fail" &&
            !parsed.stringArguments.empty() &&
            (parsed.stringArguments[0] == "/b_alloc" ||
             parsed.stringArguments[0] == "/b_setn" ||
             parsed.stringArguments[0] == "/sync")) {
            const auto it = std::find_if(
                pendingSampleLoads.begin(), pendingSampleLoads.end(),
                [&message](const auto& entry) {
                    return entry.first.first == message.origin;
                });
            if (it == pendingSampleLoads.end()) return false;
            const auto connectionIt = std::find_if(
                connections.begin(), connections.end(),
                [&message](const auto& entry) {
                    return entry.second.origin == message.origin;
                });
            const std::string reason = parsed.stringArguments.size() > 1
                ? std::string(parsed.stringArguments[1])
                : "Embedded engine could not load the audio buffer";
            if (connectionIt != connections.end()) {
                enqueuePacket(connectionIt->second,
                              makeFailPacket(it->second.command, reason));
            }
            pendingSampleLoads.erase(it);
            return true;
        }
        return false;
    }

    void pumpBufferLoads() {
        constexpr std::uint32_t kSamplesPerPacket = 128;
        constexpr std::uint32_t kPacketsPerPoll = 16;
        for (auto it = pendingSampleLoads.begin();
             it != pendingSampleLoads.end();) {
            auto& load = it->second;
            if (load.stage != PendingSampleLoad::Stage::Sending) {
                ++it;
                continue;
            }

            bool failed = false;
            for (std::uint32_t packetIndex = 0;
                 packetIndex < kPacketsPerPoll &&
                 load.nextSample < load.samples.size(); ++packetIndex) {
                const auto remaining = static_cast<std::uint32_t>(
                    load.samples.size() - load.nextSample);
                const auto count = std::min(kSamplesPerPacket, remaining);
                const auto packet = makeBufferSetnPacket(
                    it->first.second, load.nextSample,
                    load.samples.data() + load.nextSample, count);
                if (!sendToClockwork(packet.data(),
                                    static_cast<std::uint32_t>(packet.size()),
                                    it->first.first)) {
                    const auto connectionIt = std::find_if(
                        connections.begin(), connections.end(),
                        [&it](const auto& entry) {
                            return entry.second.origin == it->first.first;
                        });
                    if (connectionIt != connections.end()) {
                        enqueuePacket(connectionIt->second, makeFailPacket(
                            load.command,
                            "Embedded engine could not queue audio buffer data"));
                    }
                    it = pendingSampleLoads.erase(it);
                    failed = true;
                    break;
                }
                load.nextSample += count;
            }
            if (failed) continue;

            if (load.nextSample == load.samples.size()) {
                auto syncId = nextSyncId++;
                if (nextSyncId <= 0) nextSyncId = 1;
                const auto packet = makeSyncPacket(syncId);
                if (!sendToClockwork(packet.data(),
                                     static_cast<std::uint32_t>(packet.size()),
                                     it->first.first)) {
                    const auto connectionIt = std::find_if(
                        connections.begin(), connections.end(),
                        [&it](const auto& entry) {
                            return entry.second.origin == it->first.first;
                        });
                    if (connectionIt != connections.end()) {
                        enqueuePacket(connectionIt->second, makeFailPacket(
                            load.command,
                            "Embedded engine could not synchronize audio buffer data"));
                    }
                    it = pendingSampleLoads.erase(it);
                    continue;
                }
                load.syncId = syncId;
                load.stage = PendingSampleLoad::Stage::WaitingSync;
            }
            ++it;
        }
    }

    void forwardClockworkReplies() {
        std::array<ClockworkClientMessage, kPollBatchSize> messages{};
        const auto count = clockwork_client_poll(
            client, messages.data(), static_cast<std::uint32_t>(messages.size()));
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& message = messages[i];
            if (completeBufferLoad(message) || completeFileLoad(message)) continue;
            if (message.route == CLOCKWORK_ROUTE_NOTIFY) {
                for (auto& [fd, connection] : connections) {
                    (void)fd;
                    enqueue(connection, message);
                }
            } else if (message.route == CLOCKWORK_ROUTE_REPLY) {
                const auto it = std::find_if(
                    connections.begin(), connections.end(),
                    [&message](const auto& entry) {
                        return entry.second.origin == message.origin;
                    });
                if (it != connections.end()) enqueue(it->second, message);
            }
        }
    }

    void run() noexcept {
        while (running.load()) {
            std::vector<SocketPlatform::PollDescriptor> pollDescriptors;
            pollDescriptors.reserve(connections.size() + 1);
            pollDescriptors.push_back(
                {listener, SocketPlatform::kPollRead, 0});
            for (const auto& [fd, connection] : connections) {
                short events = SocketPlatform::kPollRead;
                if (!connection.outgoing.empty()) {
                    events |= SocketPlatform::kPollWrite;
                }
                pollDescriptors.push_back({fd, events, 0});
            }

            const int result = SocketPlatform::poll(
                pollDescriptors.data(), pollDescriptors.size(), 4);
            if (result < 0 &&
                !SocketPlatform::interrupted(SocketPlatform::lastError())) {
                break;
            }

            if (!pollDescriptors.empty() &&
                (pollDescriptors.front().revents & SocketPlatform::kPollRead) != 0) {
                acceptConnections();
            }

            std::vector<SocketPlatform::Socket> closed;
            for (std::size_t i = 1; i < pollDescriptors.size(); ++i) {
                const auto events = pollDescriptors[i].revents;
                if (events == 0) continue;
                const auto fd = pollDescriptors[i].fd;
                auto it = connections.find(fd);
                if (it == connections.end()) continue;
                bool keep = (events & (SocketPlatform::kPollError |
                                       SocketPlatform::kPollHangup |
                                       SocketPlatform::kPollInvalid)) == 0;
                if (keep && (events & SocketPlatform::kPollRead) != 0) {
                    keep = readConnection(it->second);
                }
                if (keep && (events & SocketPlatform::kPollWrite) != 0) {
                    keep = flushConnection(it->second);
                }
                if (!keep) closed.push_back(fd);
            }
            for (const auto fd : closed) {
                const auto it = connections.find(fd);
                if (it != connections.end()) {
                    pendingFileLoads.erase(it->second.origin);
                }
                SocketPlatform::close(fd);
                connections.erase(fd);
            }

            forwardClockworkReplies();
            pumpBufferLoads();
        }

        for (auto& [fd, connection] : connections) {
            (void)connection;
            SocketPlatform::close(fd);
        }
        connections.clear();
    }
};

ClockworkTcpOscBridge::ClockworkTcpOscBridge()
    : impl_(std::make_unique<Impl>()) {}

ClockworkTcpOscBridge::~ClockworkTcpOscBridge() {
    stop();
}

bool ClockworkTcpOscBridge::start(ClockworkClient* client,
                                  double sampleRate) noexcept {
    return impl_->start(client, sampleRate);
}

void ClockworkTcpOscBridge::stop() noexcept {
    impl_->stop();
}

std::uint16_t ClockworkTcpOscBridge::port() const noexcept {
    return impl_->listenPort;
}

} // namespace SonicPiVst3
