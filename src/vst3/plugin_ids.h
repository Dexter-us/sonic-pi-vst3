// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "pluginterfaces/base/funknown.h"

namespace SonicPiVst3 {

inline const Steinberg::FUID kProcessorClassId(
    0x7b9f6c43, 0xa92e4b17, 0x8dc05a31, 0xe6f22b08);
inline const Steinberg::FUID kControllerClassId(
    0x6c8d0f52, 0x37ab4e91, 0xa45d826c, 0x19f03b77);

inline constexpr char kPluginName[] = "Dexter U.S. Sonic Pi";
inline constexpr char kPluginVersion[] = "0.1.2";

} // namespace SonicPiVst3
