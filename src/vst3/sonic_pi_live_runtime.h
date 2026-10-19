// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "clockwork_client.h"
#include "sonic_pi_runtime_status.h"

#include <cstddef>
#include <memory>
#include <filesystem>
#include <string>

namespace SonicPiVst3 {

// Starts Sonic Pi's upstream Ruby Spider server as a helper process, then
// forwards editor Run/Stop commands to it. The Ruby process is never called
// from the VST audio callback.
class SonicPiLiveRuntime final {
public:
    SonicPiLiveRuntime();
    ~SonicPiLiveRuntime();

    SonicPiLiveRuntime(const SonicPiLiveRuntime&) = delete;
    SonicPiLiveRuntime& operator=(const SonicPiLiveRuntime&) = delete;

    // dataRoot is for isolated executable probes. Instruments leave it empty
    // and resolve RuntimeData from their actual portable bundle.
    bool start(ClockworkClient* client, double sampleRate,
               const std::filesystem::path& dataRoot = {}) noexcept;
    void stop() noexcept;

    bool runCode(const std::string& code) noexcept;
    bool stopAllJobs() noexcept;
    bool ready() const noexcept;
    bool failed() const noexcept;
    RuntimeStatus status() const noexcept;
    std::string error() const;
    std::string logTail(std::size_t maxBytes = 4096) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SonicPiVst3
