// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin_ids.h"
#include "sonic_pi_controller.h"
#include "sonic_pi_processor.h"

#include "public.sdk/source/main/pluginfactory.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF(
    "Dexter U.S.",
    "https://github.com/Dexter-us/sonic-pi-vst3",
    "")

DEF_CLASS2(
    INLINE_UID_FROM_FUID(SonicPiVst3::kProcessorClassId),
    PClassInfo::kManyInstances,
    kVstAudioEffectClass,
    SonicPiVst3::kPluginName,
    Vst::kDistributable,
    PlugType::kInstrumentSynth,
    SonicPiVst3::kPluginVersion,
    kVstVersionString,
    SonicPiVst3::Processor::createInstance)

DEF_CLASS2(
    INLINE_UID_FROM_FUID(SonicPiVst3::kControllerClassId),
    PClassInfo::kManyInstances,
    kVstComponentControllerClass,
    "Dexter U.S. Sonic Pi Controller",
    0,
    "",
    SonicPiVst3::kPluginVersion,
    kVstVersionString,
    SonicPiVst3::Controller::createInstance)

END_FACTORY
