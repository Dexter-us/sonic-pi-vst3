// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "pluginterfaces/base/ibstream.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace SonicPiVst3 {

inline constexpr char kStarterCode[] =
    "use_bpm 110\n"
    "\n"
    "live_loop :pulse do\n"
    "  play 60, amp: 0.5\n"
    "  sleep 1\n"
    "end";

inline constexpr std::uint32_t kMaxCodeStateBytes = 1024 * 1024;

namespace CodeStateDetail {
inline bool transfer(Steinberg::IBStream* stream, void* bytes,
                     Steinberg::int32 size, bool writing) {
    if (!stream) return false;
    auto* cursor = static_cast<char*>(bytes);
    while (size > 0) {
        Steinberg::int32 transferred = 0;
        const auto result = writing
            ? stream->write(cursor, size, &transferred)
            : stream->read(cursor, size, &transferred);
        if (result != Steinberg::kResultOk ||
            transferred <= 0 || transferred > size) {
            return false;
        }
        cursor += transferred;
        size -= transferred;
    }
    return true;
}
} // namespace CodeStateDetail

// Versioned, explicitly little-endian UTF-8 state. Decode into a temporary so
// corrupt, truncated, or oversized host state cannot replace the current buffer.
inline bool writeCodeState(Steinberg::IBStream* stream, std::string_view code) {
    if (!stream || code.size() > kMaxCodeStateBytes) return false;
    std::array<unsigned char, 16> header{
        'D', 'U', 'S', 'P', 'C', 'O', 'D', 'E', 1, 0, 0, 0, 0, 0, 0, 0};
    const auto length = static_cast<std::uint32_t>(code.size());
    for (unsigned i = 0; i < 4; ++i) {
        header[12 + i] = static_cast<unsigned char>(length >> (i * 8));
    }
    return CodeStateDetail::transfer(stream, header.data(), header.size(), true) &&
           CodeStateDetail::transfer(
               stream, const_cast<char*>(code.data()),
               static_cast<Steinberg::int32>(code.size()), true);
}

inline bool readCodeState(Steinberg::IBStream* stream, std::string& code) {
    std::array<unsigned char, 16> header{};
    if (!CodeStateDetail::transfer(stream, header.data(), header.size(), false)) {
        return false;
    }
    constexpr std::array<unsigned char, 12> prefix{
        'D', 'U', 'S', 'P', 'C', 'O', 'D', 'E', 1, 0, 0, 0};
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (header[i] != prefix[i]) return false;
    }
    std::uint32_t length = 0;
    for (unsigned i = 0; i < 4; ++i) {
        length |= static_cast<std::uint32_t>(header[12 + i]) << (i * 8);
    }
    if (length > kMaxCodeStateBytes) return false;
    std::string restored(length, '\0');
    if (!CodeStateDetail::transfer(
            stream, restored.data(), static_cast<Steinberg::int32>(length), false)) {
        return false;
    }
    code = std::move(restored);
    return true;
}

} // namespace SonicPiVst3
