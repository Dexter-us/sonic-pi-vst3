// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonic_pi_generator.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace SonicPiVst3;
using namespace std::chrono_literals;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void test(const GeneratorOptions& options) {
    std::atomic<bool> cancel{false};
    auto run = [&](const std::string& script) {
        return runRubyGenerator(script, options, cancel);
    };
    auto result = run(kStarterGenerator);
    require(result.success && result.output.find("use_bpm 110") != std::string::npos &&
            result.output.find("play 72") != std::string::npos,
            "Starter generator did not generate Sonic Pi source");
    result = run("puts 'debug only'; \"# café 音楽\\nplay 67\\n\"");
    require(result.success && result.output == "# café 音楽\nplay 67\n" &&
            result.error.find("debug only") != std::string::npos,
            "Unicode/diagnostic separation failed");
    result = run("''");
    require(result.success && result.output.empty(), "Empty String was not supported");
    result = run("'#' * 32000");
    require(result.success && result.output.size() == 32000, "Valid size limit failed");
    for (const auto& script : std::vector<std::string>{
             "raise 'example failure'", "this is not ( valid ruby",
             "nil", "'x' * 32001", "\"\\xFF\".force_encoding('UTF-8')",
             "STDOUT.binmode; STDOUT.write([255].pack('C')); ''",
             "STDOUT.write(\"\\0\"); ''",
             "STDOUT.write('x' * 32001); ''",
             "warn('x' * 20000); ''"}) {
        result = run(script);
        require(!result.success && !result.error.empty(),
                "Invalid/oversized generator result was accepted");
    }
    result = run(std::string(kMaxGeneratorSourceBytes + 1, ' '));
    require(!result.success, "Oversized script was accepted");
    auto shortOptions = options;
    shortOptions.timeout = 150ms;
    auto before = std::chrono::steady_clock::now();
    result = runRubyGenerator("loop { sleep 0.01 }", shortOptions, cancel);
    require(!result.success && result.error.find("timed out") != std::string::npos &&
            std::chrono::steady_clock::now() - before < 2s,
            "Infinite loop did not time out promptly");
    auto missing = options;
    missing.rubyExecutable = std::filesystem::temp_directory_path() /
                             "no-such-dexter-ruby-executable";
    require(!runRubyGenerator("''", missing, cancel).success,
            "Missing interpreter was silently accepted");
    cancel.store(true);
    require(runRubyGenerator("''", options, cancel).cancelled,
            "Pre-launch cancellation was ignored");
    cancel.store(false);
    RubyGenerator worker;
    require(worker.start("sleep 5; 'play 60'", options), "Async launch failed");
    require(!worker.start("'play 72'", options), "Concurrent launch was accepted");
    before = std::chrono::steady_clock::now();
    worker.cancel();
    std::optional<GeneratorResult> completed;
    while (!(completed = worker.takeResult()) &&
           std::chrono::steady_clock::now() - before < 2s) {
        std::this_thread::sleep_for(10ms);
    }
    require(completed && completed->cancelled && !completed->success,
            "Async cancellation failed");
    require(worker.start("'play 72'", options), "Worker could not restart after cancellation");
    before = std::chrono::steady_clock::now();
    while (!(completed = worker.takeResult()) &&
           std::chrono::steady_clock::now() - before < 6s) {
        std::this_thread::sleep_for(10ms);
    }
    require(completed && completed->success && completed->output == "play 72",
            "Restarted worker failed");

    // A child created by a trusted script must not outlive the generator job.
    auto marker = std::filesystem::temp_directory_path() /
        ("dexter-generator-child-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    auto path = marker.generic_string();
    // Ruby single-quoted strings need only these two escapes.
    std::string quoted;
    for (char c : path) { if (c == '\\' || c == '\'') quoted += '\\'; quoted += c; }
    const std::string script =
        "require 'rbconfig'\n"
        "Process.spawn(RbConfig.ruby, '-e', "
        "\"sleep 0.8; File.write(ARGV[0], 'leaked')\", '" + quoted + "')\n"
        "'play 60'\n";
    result = run(script);
    require(result.success, "Child-process cleanup probe did not run");
    std::this_thread::sleep_for(1100ms);
    require(!std::filesystem::exists(marker), "Generator leaked a background child process");
}
} // namespace

int main(int argc, char** argv) {
    try {
        GeneratorOptions options;
        if (argc > 1) options.rubyExecutable =
            std::filesystem::path(reinterpret_cast<const char8_t*>(argv[1]));
        test(options);
        std::cout << "PASS: Ruby generation, Unicode, empty output, diagnostics, "
                     "errors, limits, timeout, cancel/restart, child cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
