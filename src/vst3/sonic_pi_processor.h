// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "clockwork_embed.h"
#include "sonic_pi_live_runtime.h"
#include "sonic_pi_code_state.h"
#include "public.sdk/source/vst/vstaudioeffect.h"

#include <memory>
#include <mutex>

namespace SonicPiVst3 {

class Processor final : public Steinberg::Vst::AudioEffect {
public:
    Processor();
    ~Processor() override;

    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IAudioProcessor*>(new Processor());
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API setupProcessing(
        Steinberg::Vst::ProcessSetup& setup) override;
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setBusArrangements(
        Steinberg::Vst::SpeakerArrangement* inputs,
        Steinberg::int32 numInputs,
        Steinberg::Vst::SpeakerArrangement* outputs,
        Steinberg::int32 numOutputs) override;
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(
        Steinberg::int32 symbolicSampleSize) override;
    Steinberg::tresult PLUGIN_API process(
        Steinberg::Vst::ProcessData& data) override;

private:
    // Never used in process(): host-state serialization stays off the audio path.
    std::mutex codeStateMutex_;
    std::string codeState_ = kStarterCode;
    bool startEngine() noexcept;
    void stopEngine() noexcept;

    ClockworkEmbed* engine_ = nullptr;
    ClockworkClient* client_ = nullptr;
    std::unique_ptr<SonicPiLiveRuntime> runtime_;
    double sampleRate_ = 48000.0;
    bool ownsProcessEngine_ = false;
    RuntimeStatus lastRuntimeStatus_ = RuntimeStatus::inactive;
};

} // namespace SonicPiVst3
