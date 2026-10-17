// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the same Ruby runtime and in-process engine used by the VST.
#include "clockwork_embed.h"
#include "sonic_pi_live_runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr std::uint32_t kCallbackFrames = 512;

struct StereoBuffers {
    std::array<float, kCallbackFrames> left{};
    std::array<float, kCallbackFrames> right{};
    std::array<float*, 2> outputs{{left.data(), right.data()}};
};

void render(ClockworkEmbed* engine, StereoBuffers& buffers) {
    const auto rendered = clockwork_embed_render(
        engine, buffers.outputs.data(), 2, nullptr, 0, kCallbackFrames);
    if (rendered != kCallbackFrames) {
        throw std::runtime_error("SuperSonic returned a short host audio block");
    }
}

} // namespace

int main() {
    ClockworkEmbedConfig config{};
    config.struct_bytes = sizeof(config);
    config.sample_rate = kSampleRate;
    config.block_size = 64;
    config.input_channels = 0;
    config.output_channels = 2;
    config.max_render_frames = kCallbackFrames;
    config.app_name = "Sonic Pi Live Runtime Probe";

    ClockworkStatus status = CLOCKWORK_OK;
    ClockworkEmbed* engine = clockwork_embed_attach(&config, &status);
    if (!engine) {
        std::cerr << "Engine attach failed: "
                  << clockwork_client_status_text(status) << '\n';
        return 1;
    }

    SonicPiVst3::SonicPiLiveRuntime runtime;
    try {
        auto* client = clockwork_embed_client(engine);
        if (!client || !runtime.start(client, kSampleRate)) {
            throw std::runtime_error(
                runtime.error().empty() ? "Could not start Sonic Pi runtime"
                                         : runtime.error());
        }

        StereoBuffers buffers;
        const auto startupDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!runtime.ready() &&
               !runtime.failed() &&
               std::chrono::steady_clock::now() < startupDeadline) {
            render(engine, buffers);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!runtime.ready()) {
            throw std::runtime_error(runtime.error().empty()
                ? "Ruby runtime did not become ready; inspect ~/.sonic-pi-vst/spider-runtime.log"
                : runtime.error());
        }

        const std::string code = "play 60, sustain: 0.4, amp: 0.8";
        if (!runtime.runCode(code)) {
            throw std::runtime_error("Sonic Pi rejected the Run command");
        }

        double energy = 0.0;
        float peak = 0.0f;
        std::uint64_t samples = 0;
        const auto audioDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < audioDeadline) {
            render(engine, buffers);
            for (std::size_t i = 0; i < kCallbackFrames; ++i) {
                for (const float sample : {buffers.left[i], buffers.right[i]}) {
                    energy += static_cast<double>(sample) * sample;
                    peak = std::max(peak, std::abs(sample));
                    ++samples;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        const double rms = std::sqrt(energy / static_cast<double>(samples));
        if (peak < 1.0e-5f || rms < 1.0e-6) {
            throw std::runtime_error(
                "Sonic Pi code ran but the host-owned audio buffers stayed silent");
        }

        runtime.stop();
        clockwork_embed_close(engine);
        std::cout << "Sonic Pi Ruby code rendered through host-owned buffers.\n"
                  << "Rate: " << kSampleRate << " Hz; block request: "
                  << kCallbackFrames << " frames; peak: " << peak
                  << "; RMS: " << rms << '\n';
        return 0;
    } catch (const std::exception& error) {
        runtime.stop();
        clockwork_embed_close(engine);
        std::cerr << "Live runtime probe failed: " << error.what() << '\n';
        return 1;
    }
}
