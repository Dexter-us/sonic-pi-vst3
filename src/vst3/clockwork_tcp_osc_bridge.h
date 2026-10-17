// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "clockwork_client.h"

#include <cstdint>
#include <memory>

namespace SonicPiVst3 {

// Adapts Sonic Pi's framed TCP OSC transport to Clockwork's in-process client.
// All Clockwork client calls are confined to this bridge's single I/O thread.
class ClockworkTcpOscBridge final {
public:
    ClockworkTcpOscBridge();
    ~ClockworkTcpOscBridge();

    ClockworkTcpOscBridge(const ClockworkTcpOscBridge&) = delete;
    ClockworkTcpOscBridge& operator=(const ClockworkTcpOscBridge&) = delete;

    bool start(ClockworkClient* client, double sampleRate) noexcept;
    void stop() noexcept;
    std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SonicPiVst3
