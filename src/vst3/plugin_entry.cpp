// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin_ids.h"
#include "sonic_pi_controller.h"
#include "sonic_pi_processor.h"

#include "public.sdk/source/main/pluginfactory.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF(
    "Independent Audio Prototype",
    "https://example.org",
    "prototype@example.org")

DEF_CLASS2(
    INLINE_UID_FROM_FUID(SonicPiVst3::kProcessorClassId),
    PClassInfo::kManyInstances,
    kVstAudioEffectClass,
    SonicPiVst3::kPluginName,
    Vst::kDistributable,
    PlugType::kInstrumentSynth,
    "0.1.0",
    kVstVersionString,
    SonicPiVst3::Processor::createInstance)

DEF_CLASS2(
    INLINE_UID_FROM_FUID(SonicPiVst3::kControllerClassId),
    PClassInfo::kManyInstances,
    kVstComponentControllerClass,
    "Sonic Pi Audio Prototype Controller",
    0,
    "",
    "0.1.0",
    kVstVersionString,
    SonicPiVst3::Controller::createInstance)

END_FACTORY
