// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace SonicPiVst3 {

enum class RuntimeStatus : std::int64_t {
    inactive = 0,
    starting = 1,
    ready = 2,
    failed = 3,
    queued = 4,
    submitted = 5,
    stopped = 6,
    error = 7,
};

} // namespace SonicPiVst3
