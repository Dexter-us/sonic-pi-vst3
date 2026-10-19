// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the same Ruby runtime and in-process engine used by the VST.
#include "clockwork_embed.h"
#include "sonic_pi_live_runtime.h"
#include "sonic_pi_portable.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <random>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace {

constexpr double kSampleRate = 48000.0;
constexpr std::uint32_t kCallbackFrames = 512;
constexpr auto kCallbackPeriod = std::chrono::duration<double>(
    static_cast<double>(kCallbackFrames) / kSampleRate);

struct StereoBuffers {
    std::array<float, kCallbackFrames> left{};
    std::array<float, kCallbackFrames> right{};
    std::array<float*, 2> outputs{{left.data(), right.data()}};
};

class AudioBlockPacer {
public:
    AudioBlockPacer()
        : nextBlock_(std::chrono::steady_clock::now()) {
#if defined(_WIN32)
        timer_ = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
#endif
    }

    ~AudioBlockPacer() {
#if defined(_WIN32)
        if (timer_) CloseHandle(timer_);
#endif
    }

    void wait() {
        nextBlock_ += std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(kCallbackPeriod);
        const auto remaining = nextBlock_ - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            nextBlock_ = std::chrono::steady_clock::now();
            return;
        }

#if defined(_WIN32)
        if (timer_) {
            using HundredNanoseconds =
                std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
            const auto ticks =
                std::chrono::duration_cast<HundredNanoseconds>(remaining).count();
            LARGE_INTEGER dueTime{};
            dueTime.QuadPart = -std::max<std::int64_t>(ticks, 1);
            if (SetWaitableTimer(timer_, &dueTime, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer_, INFINITE);
                return;
            }
        }
#endif
        std::this_thread::sleep_until(nextBlock_);
    }

private:
    std::chrono::steady_clock::time_point nextBlock_;
#if defined(_WIN32)
    HANDLE timer_ = nullptr;
#endif
};

void render(ClockworkEmbed* engine, StereoBuffers& buffers) {
    const auto rendered = clockwork_embed_render(
        engine, buffers.outputs.data(), 2, nullptr, 0, kCallbackFrames);
    if (rendered != kCallbackFrames) {
        throw std::runtime_error("SuperSonic returned a short host audio block");
    }
}

void printRuntimeLogTail(const SonicPiVst3::SonicPiLiveRuntime& runtime) {
    std::cerr << "--- Sonic Pi runtime log ---\n" << runtime.logTail(16000)
              << "\n--- end runtime log ---\n";
}

std::string htmlEscape(std::string_view text) {
    std::string escaped;
    for (char c : text) {
        switch (c) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        default: escaped += c;
        }
    }
    return escaped;
}

bool checkSnips(ClockworkEmbed* engine, SonicPiVst3::SonicPiLiveRuntime& runtime,
                StereoBuffers& buffers, AudioBlockPacer& pacer,
                const std::filesystem::path& directory,
                const std::filesystem::path& report) {
    const auto files = SonicPiVst3::listSnipsDirectory(directory);
    if (files.empty()) throw std::runtime_error("No snippets found to validate");
    std::ostringstream html;
    html << "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         << "<title>Sonic Pi snippet checks</title><style>"
         << "body{font:16px/1.6 system-ui;max-width:900px;margin:40px auto;padding:20px}"
         << "pre{background:#f1f3f5;padding:18px;white-space:pre-wrap}"
         << "article{border-top:1px solid #bbb;margin-top:30px}.pass{color:#176235}"
         << ".fail{color:#a41414}</style><h1>Bundled snippet checks</h1>"
         << "<p>Plug-in " << SONIC_PI_PLUGIN_VERSION
         << "; Sonic Pi v5.0.0; 48000 Hz stereo host buffers. "
#if defined(_WIN32)
         << "Windows"
#else
         << "Linux"
#endif
         << " live-runtime execution, not just Ruby syntax checking.</p>"
         << "<p>This report covers the exact source below. Changed or newly added "
         << "snippets are unverified. Appending into another program can introduce "
         << "loop-name or other conflicts.</p>";
    bool allPassed = true;
    for (const auto& file : files) {
        if (std::filesystem::file_size(file) > 32000) {
            throw std::runtime_error("Snippet exceeds Run limit: " + file.string());
        }
        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read snippet: " + file.string());
        std::string source((std::istreambuf_iterator<char>(input)), {});
        runtime.stopAllJobs();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            render(engine, buffers); pacer.wait();
        }
        const auto before = runtime.logTail(64000);
        const bool sent = !source.empty() && runtime.runCode(source);
        float peak = 0;
        double energy = 0;
        std::uint64_t samples = 0;
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2400);
        while (std::chrono::steady_clock::now() < deadline) {
            render(engine, buffers);
            for (std::size_t i = 0; i < kCallbackFrames; ++i) {
                for (float sample : {buffers.left[i], buffers.right[i]}) {
                    peak = std::max(peak, std::abs(sample));
                    energy += static_cast<double>(sample) * sample;
                    ++samples;
                }
            }
            pacer.wait();
        }
        const auto after = runtime.logTail(64000);
        const auto delta = after.starts_with(before) ? after.substr(before.size()) : after;
        // Spider also logs transport exceptions when no standalone GUI listens.
        // Only its structured job-error events describe submitted Ruby source.
        const bool error = delta.find("[ruby-error] ") != std::string::npos;
        const auto rms = samples ? std::sqrt(energy / samples) : 0;
        const bool passed = sent && !error && peak > 1e-5f && rms > 1e-6;
        allPassed &= passed;
        const auto name = SonicPiVst3::pathUtf8(file.filename());
        std::cout << (passed ? "PASS" : "FAIL") << ": " << name
                  << "; peak " << peak << "; RMS " << rms << '\n';
        html << "<article><h2>" << htmlEscape(name) << "</h2><p class=\""
             << (passed ? "pass" : "fail") << "\"><strong>"
             << (passed ? "PASS" : "FAIL") << "</strong> · peak " << peak
             << " · RMS " << rms << "</p><pre>" << htmlEscape(source) << "</pre>";
        if (error || !passed) html << "<h3>Runtime output</h3><pre>"
                                  << htmlEscape(delta) << "</pre>";
        html << "</article>";
    }
    runtime.stopAllJobs();
    // Prove errors are observable; non-silent audio alone must never pass a
    // snippet that plays a note and then raises an exception.
    const auto beforeControl = runtime.logTail(64000);
    runtime.runCode("raise 'SNIPS_CHECK_EXPECTED_ERROR'");
    const auto controlDeadline = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < controlDeadline) {
        render(engine, buffers); pacer.wait();
    }
    const auto controlLog = runtime.logTail(64000);
    const auto controlDelta = controlLog.starts_with(beforeControl)
        ? controlLog.substr(beforeControl.size()) : controlLog;
    const bool controlPassed =
        controlDelta.find("[ruby-error] Exception") != std::string::npos &&
        controlDelta.find("SNIPS_CHECK_EXPECTED_ERROR") != std::string::npos;
    allPassed &= controlPassed;
    std::cout << (controlPassed ? "PASS" : "FAIL")
              << ": negative control detected an intentional Ruby exception\n";
    html << "<h2>Error-detection control</h2><p>"
         << (controlPassed
                ? "PASS: a deliberate failing program produced a structured Ruby error."
                : "FAIL: the deliberate Ruby error was not observed; results are not reliable.")
         << "</p>";
    runtime.stopAllJobs();
    html << "<p><strong>" << (allPassed ? "All snippets passed." : "Checks failed.")
         << "</strong> " << files.size() << " snippets executed.</p></html>";
    std::filesystem::create_directories(report.parent_path());
    std::ofstream output(report, std::ios::binary);
    output << html.str();
    output.close();
    if (!output) throw std::runtime_error("Cannot write snippet validation report");
    return allPassed;
}

struct ProbeData {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("sonic-pi-probe-" + std::to_string(std::random_device{}()));
    ~ProbeData() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 1 && (argc != 4 || std::string_view(argv[1]) != "--snips")) {
        std::cerr << "Usage: sonic_pi_live_probe [--snips Snips-directory report.html]\n";
        return 2;
    }
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

    ProbeData probeData;
    SonicPiVst3::SonicPiLiveRuntime runtime;
    try {
        auto* client = clockwork_embed_client(engine);
        if (!client || !runtime.start(client, kSampleRate, probeData.path)) {
            throw std::runtime_error(
                runtime.error().empty() ? "Could not start Sonic Pi runtime"
                                         : runtime.error());
        }

        StereoBuffers buffers;
        AudioBlockPacer pacer;
        const auto startupDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!runtime.ready() &&
               !runtime.failed() &&
               std::chrono::steady_clock::now() < startupDeadline) {
            render(engine, buffers);
            pacer.wait();
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
            pacer.wait();
        }

        const double rms = std::sqrt(energy / static_cast<double>(samples));
        if (peak < 1.0e-5f || rms < 1.0e-6) {
            throw std::runtime_error(
                "Sonic Pi code ran but the host-owned audio buffers stayed silent");
        }

        if (argc == 4 && !checkSnips(
                engine, runtime, buffers, pacer,
                std::filesystem::path(reinterpret_cast<const char8_t*>(argv[2])),
                std::filesystem::path(reinterpret_cast<const char8_t*>(argv[3])))) {
            throw std::runtime_error("One or more bundled snippets failed live-runtime checks");
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
        printRuntimeLogTail(runtime);
        return 1;
    }
}
