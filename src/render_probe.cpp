// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype: render the real SuperSonic/scsynth engine into host-owned buffers.
#include "clockwork_embed.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kBlockSize = 64;
constexpr uint32_t kMaxCallbackFrames = 2048;
constexpr uint32_t kOrigin = 1;

void appendU32BE(std::vector<uint8_t>& packet, uint32_t value) {
    packet.push_back(static_cast<uint8_t>(value >> 24));
    packet.push_back(static_cast<uint8_t>(value >> 16));
    packet.push_back(static_cast<uint8_t>(value >> 8));
    packet.push_back(static_cast<uint8_t>(value));
}

void appendString(std::vector<uint8_t>& packet, const std::string& value) {
    packet.insert(packet.end(), value.begin(), value.end());
    packet.push_back(0);
    while (packet.size() % 4 != 0) packet.push_back(0);
}

void appendFloatBE(std::vector<uint8_t>& packet, float value) {
    appendU32BE(packet, std::bit_cast<uint32_t>(value));
}

std::vector<uint8_t> makeBlobMessage(const std::string& address,
                                    const std::vector<uint8_t>& blob) {
    std::vector<uint8_t> packet;
    appendString(packet, address);
    appendString(packet, ",b");
    appendU32BE(packet, static_cast<uint32_t>(blob.size()));
    packet.insert(packet.end(), blob.begin(), blob.end());
    while (packet.size() % 4 != 0) packet.push_back(0);
    return packet;
}

std::vector<uint8_t> makeSyncMessage(int32_t syncId) {
    std::vector<uint8_t> packet;
    appendString(packet, "/sync");
    appendString(packet, ",i");
    appendU32BE(packet, static_cast<uint32_t>(syncId));
    return packet;
}

std::vector<uint8_t> makeSNewMessage() {
    std::vector<uint8_t> packet;
    appendString(packet, "/s_new");
    appendString(packet, ",siii" "sfsfsfsfsf");
    appendString(packet, "sonic-pi-beep");
    appendU32BE(packet, 1000); // node ID
    appendU32BE(packet, 0);    // add to head
    appendU32BE(packet, 0);    // root group
    const std::array<std::pair<const char*, float>, 5> controls{{
        {"note", 60.0f},
        {"amp", 0.18f},
        {"attack", 0.01f},
        {"sustain", 0.35f},
        {"release", 0.25f},
    }};
    for (const auto& [name, value] : controls) {
        appendString(packet, name);
        appendFloatBE(packet, value);
    }
    return packet;
}

void sendMessage(ClockworkClient* client, const std::vector<uint8_t>& packet) {
    const auto status = clockwork_client_send(
        client, packet.data(), static_cast<uint32_t>(packet.size()), kOrigin);
    if (status != CLOCKWORK_OK) {
        throw std::runtime_error(std::string("Could not queue OSC message: ") +
                                 clockwork_client_status_text(status));
    }
}

bool isSyncedReply(const ClockworkClientMessage& message, int32_t wantedId) {
    if (!message.bytes || message.length < 12) return false;
    constexpr char address[] = "/synced";
    if (message.length < sizeof(address) ||
        !std::equal(address, address + sizeof(address) - 1,
                    reinterpret_cast<const char*>(message.bytes))) {
        return false;
    }
    // OSC address is 8 bytes including its terminator/padding; then ",i\0".
    const size_t idOffset = 12;
    if (message.length < idOffset + 4) return false;
    const uint32_t id =
        (static_cast<uint32_t>(message.bytes[idOffset]) << 24) |
        (static_cast<uint32_t>(message.bytes[idOffset + 1]) << 16) |
        (static_cast<uint32_t>(message.bytes[idOffset + 2]) << 8) |
        static_cast<uint32_t>(message.bytes[idOffset + 3]);
    return static_cast<int32_t>(id) == wantedId;
}

void pollReplies(ClockworkClient* client, bool& synced, int32_t syncId) {
    std::array<ClockworkClientMessage, 32> messages{};
    const uint32_t count = clockwork_client_poll(
        client, messages.data(), static_cast<uint32_t>(messages.size()));
    for (uint32_t i = 0; i < count; ++i) {
        if (isSyncedReply(messages[i], syncId)) synced = true;
    }
}

struct RenderedAudio {
    std::vector<float> interleaved;
    std::vector<float> left = std::vector<float>(kMaxCallbackFrames);
    std::vector<float> right = std::vector<float>(kMaxCallbackFrames);
    std::array<float*, 2> outputs{{left.data(), right.data()}};
    size_t cursor = 0;

    void render(ClockworkEmbed* engine, uint32_t frames, bool keepOutput) {
        const uint32_t count = clockwork_embed_render(
            engine, outputs.data(), 2, nullptr, 0, frames);
        if (count != frames) {
            throw std::runtime_error("Engine returned fewer frames than the host requested");
        }
        if (!keepOutput) return;
        for (uint32_t i = 0; i < frames; ++i) {
            interleaved[cursor++] = left[i];
            interleaved[cursor++] = right[i];
        }
    }
};

void writeWav(const std::filesystem::path& path,
              const std::vector<float>& samples,
              uint32_t sampleRate) {
    if (samples.size() % 2 != 0) throw std::runtime_error("Expected stereo samples");
    const uint32_t frames = static_cast<uint32_t>(samples.size() / 2);
    const uint32_t dataBytes = frames * 2u * sizeof(int16_t);
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Could not create output WAV: " + path.string());

    const auto putU16 = [&out](uint16_t n) {
        out.put(static_cast<char>(n));
        out.put(static_cast<char>(n >> 8));
    };
    const auto putU32 = [&out](uint32_t n) {
        out.put(static_cast<char>(n));
        out.put(static_cast<char>(n >> 8));
        out.put(static_cast<char>(n >> 16));
        out.put(static_cast<char>(n >> 24));
    };
    out.write("RIFF", 4);
    putU32(36u + dataBytes);
    out.write("WAVEfmt ", 8);
    putU32(16);
    putU16(1); // PCM
    putU16(2); // stereo
    putU32(sampleRate);
    putU32(sampleRate * 2u * sizeof(int16_t));
    putU16(2u * sizeof(int16_t));
    putU16(16);
    out.write("data", 4);
    putU32(dataBytes);
    for (float sample : samples) {
        const float limited = std::clamp(sample, -1.0f, 1.0f);
        const auto pcm = static_cast<int16_t>(std::lrint(limited * 32767.0f));
        putU16(static_cast<uint16_t>(pcm));
    }
    if (!out) throw std::runtime_error("Failed while writing WAV data");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path synthdef = SONIC_PI_BEEP_SYNTHDEF;
        const std::filesystem::path wavPath =
            argc > 1 ? argv[1] : "sonic-pi-host-buffer-test.wav";
        const double seconds = argc > 2 ? std::stod(argv[2]) : 2.0;
        if (!(seconds > 0.1 && seconds <= 30.0)) {
            throw std::runtime_error("Duration must be between 0.1 and 30 seconds");
        }

        std::ifstream synthdefFile(synthdef, std::ios::binary);
        if (!synthdefFile) {
            throw std::runtime_error("Could not open Sonic Pi synthdef: " + synthdef.string());
        }
        const std::vector<uint8_t> synthdefBytes{
            std::istreambuf_iterator<char>(synthdefFile), std::istreambuf_iterator<char>()};
        if (synthdefBytes.empty()) throw std::runtime_error("Sonic Pi synthdef is empty");

        ClockworkEmbedConfig config{};
        config.struct_bytes = sizeof(config);
        config.sample_rate = kSampleRate;
        config.block_size = kBlockSize;
        config.input_channels = 0;
        config.output_channels = 2;
        config.max_render_frames = kMaxCallbackFrames;
        config.app_name = "Sonic Pi Host Buffer Spike";

        ClockworkStatus status = CLOCKWORK_OK;
        ClockworkEmbed* engine = clockwork_embed_attach(&config, &status);
        if (!engine) {
            throw std::runtime_error(std::string("Engine attach failed: ") +
                                     clockwork_client_status_text(status));
        }
        const auto closeEngine = [&engine] {
            if (engine) clockwork_embed_close(engine);
            engine = nullptr;
        };

        try {
            ClockworkClient* client = clockwork_embed_client(engine);
            if (!client) throw std::runtime_error("Engine did not provide a control client");

            sendMessage(client, makeBlobMessage("/d_recv", synthdefBytes));
            constexpr int32_t syncId = 41;
            sendMessage(client, makeSyncMessage(syncId));

            RenderedAudio renderer;
            bool synced = false;
            const std::array<uint32_t, 5> callbackSizes{{127, 511, 256, 1024, 64}};
            uint64_t waitedFrames = 0;
            size_t callbackIndex = 0;
            while (!synced && waitedFrames < static_cast<uint64_t>(kSampleRate * 4)) {
                const uint32_t frames = callbackSizes[callbackIndex++ % callbackSizes.size()];
                renderer.render(engine, frames, false);
                waitedFrames += frames;
                pollReplies(client, synced, syncId);
            }
            if (!synced) {
                throw std::runtime_error("Timed out waiting for the engine to load sonic-pi-beep");
            }

            const uint32_t engineBlockSize = clockwork_embed_block_size(engine);
            sendMessage(client, makeSNewMessage());
            const uint32_t totalFrames = static_cast<uint32_t>(seconds * kSampleRate);
            renderer.interleaved.resize(static_cast<size_t>(totalFrames) * 2);
            renderer.cursor = 0;
            uint32_t rendered = 0;
            while (rendered < totalFrames) {
                const uint32_t ask = callbackSizes[callbackIndex++ % callbackSizes.size()];
                const uint32_t frames = std::min(ask, totalFrames - rendered);
                renderer.render(engine, frames, true);
                rendered += frames;
            }

            double energy = 0.0;
            float peak = 0.0f;
            for (float sample : renderer.interleaved) {
                energy += static_cast<double>(sample) * sample;
                peak = std::max(peak, std::abs(sample));
            }
            const double rms = std::sqrt(energy / renderer.interleaved.size());
            if (peak < 1.0e-5f || rms < 1.0e-6) {
                throw std::runtime_error("Rendered output is silent; expected audible Sonic Pi synth output");
            }

            if (!wavPath.parent_path().empty()) {
                std::filesystem::create_directories(wavPath.parent_path());
            }
            writeWav(wavPath, renderer.interleaved, static_cast<uint32_t>(kSampleRate));
            closeEngine();
            std::cout << "Rendered Sonic Pi sonic-pi-beep through host-owned buffers.\n"
                      << "Engine geometry: " << kSampleRate << " Hz, "
                      << engineBlockSize << " frames/block\n"
                      << "Host callback sizes tested: 127, 511, 256, 1024, 64 frames\n"
                      << "Output: " << wavPath << " (" << seconds << " s stereo PCM)\n"
                      << "Peak: " << peak << ", RMS: " << rms << '\n';
        } catch (...) {
            closeEngine();
            throw;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Prototype failed: " << error.what() << '\n';
        return 1;
    }
}

