// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace SonicPiVst3 {

inline constexpr std::size_t kMaxGeneratorSourceBytes = 65536;
inline constexpr std::size_t kMaxGeneratorOutputBytes = 32000;
inline constexpr char kStarterGenerator[] =
    "# TRUSTED Ruby only: scripts can read/write files. Not a sandbox.\n"
    "# The final expression must be a String containing Sonic Pi code.\n"
    "# puts/warn are diagnostics, not generated source.\n"
    "bpm = 110\n"
    "notes = [60, 64, 67, 72]\n"
    "steps = notes.map { |n| \"  play #{n}, amp: 0.5\\n  sleep 0.5\" }\n"
    "\"use_bpm #{bpm}\\n\\nlive_loop :generated do\\n#{steps.join(\"\\n\")}\\nend\\n\"\n";

struct GeneratorOptions {
    // Empty: use bundled Ruby on Windows, or ruby on PATH on Linux/macOS.
    std::filesystem::path rubyExecutable;
    std::chrono::milliseconds timeout{5000};
};

struct GeneratorResult {
    bool success = false;
    bool cancelled = false;
    std::string output;
    std::string error;
};

// For the worker/tests only: never call this blocking operation on the audio/UI
// thread. A separate process is isolation, NOT a security sandbox.
GeneratorResult runRubyGenerator(
    const std::string& source, const GeneratorOptions& options,
    const std::atomic<bool>& cancel);

class RubyGenerator final {
public:
    ~RubyGenerator();
    bool start(std::string source, GeneratorOptions options = {});
    void cancel() noexcept { cancel_.store(true); }
    bool running() const noexcept { return running_.load(); }
    std::optional<GeneratorResult> takeResult();

private:
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex resultMutex_;
    std::optional<GeneratorResult> result_;
};

} // namespace SonicPiVst3
