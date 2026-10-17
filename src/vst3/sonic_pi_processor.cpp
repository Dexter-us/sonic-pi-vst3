// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal VST3 processor that attaches SuperSonic without opening an audio
// device, then renders into the buffers supplied by the VST3 host.
#include "sonic_pi_processor.h"

#include "plugin_ids.h"

#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace SonicPiVst3 {
namespace {

constexpr std::uint32_t kMaxRenderFrames = 4096;

std::atomic_flag gEngineClaimed = ATOMIC_FLAG_INIT;

void clearOutput(Steinberg::Vst::ProcessData& data) {
    if (!data.outputs || data.numOutputs < 1 || data.numSamples <= 0) return;
    auto& bus = data.outputs[0];
    for (Steinberg::int32 channel = 0; channel < bus.numChannels; ++channel) {
        if (bus.channelBuffers32 && bus.channelBuffers32[channel]) {
            std::fill_n(bus.channelBuffers32[channel], data.numSamples, 0.0f);
        }
    }
}

} // namespace

Processor::Processor() {
    setControllerClass(kControllerClassId);
}

Processor::~Processor() {
    stopEngine();
}

Steinberg::tresult PLUGIN_API Processor::initialize(Steinberg::FUnknown* context) {
    const auto result = AudioEffect::initialize(context);
    if (result != Steinberg::kResultOk) return result;

    addAudioOutput(STR16("Stereo Out"), Steinberg::Vst::SpeakerArr::kStereo);
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Processor::terminate() {
    stopEngine();
    return AudioEffect::terminate();
}

Steinberg::tresult PLUGIN_API Processor::setupProcessing(
    Steinberg::Vst::ProcessSetup& setup) {
    const auto result = AudioEffect::setupProcessing(setup);
    if (result == Steinberg::kResultOk && setup.sampleRate > 0.0) {
        sampleRate_ = setup.sampleRate;
    }
    return result;
}

Steinberg::tresult PLUGIN_API Processor::setActive(Steinberg::TBool state) {
    if (state) {
        const auto result = AudioEffect::setActive(state);
        if (result != Steinberg::kResultOk) return result;
        if (!startEngine()) {
            AudioEffect::setActive(false);
            return Steinberg::kResultFalse;
        }
        return Steinberg::kResultTrue;
    }

    stopEngine();
    return AudioEffect::setActive(false);
}

Steinberg::tresult PLUGIN_API Processor::setBusArrangements(
    Steinberg::Vst::SpeakerArrangement* inputs,
    Steinberg::int32 numInputs,
    Steinberg::Vst::SpeakerArrangement* outputs,
    Steinberg::int32 numOutputs) {
    if (numInputs != 0 || numOutputs != 1 || !outputs ||
        outputs[0] != Steinberg::Vst::SpeakerArr::kStereo) {
        return Steinberg::kResultFalse;
    }
    return AudioEffect::setBusArrangements(inputs, numInputs, outputs, numOutputs);
}

Steinberg::tresult PLUGIN_API Processor::notify(
    Steinberg::Vst::IMessage* message) {
    if (!message) return Steinberg::kResultFalse;
    const auto* messageId = message->getMessageID();
    if (!messageId) return Steinberg::kResultFalse;

    if (std::strcmp(messageId, "SonicPi.RunCode") == 0) {
        auto* attributes = message->getAttributes();
        const void* bytes = nullptr;
        Steinberg::uint32 size = 0;
        if (!attributes ||
            attributes->getBinary("code", bytes, size) != Steinberg::kResultOk ||
            !bytes || size == 0) {
            return Steinberg::kResultFalse;
        }
        const std::string code(static_cast<const char*>(bytes), size);
        return runtime_ && runtime_->runCode(code)
            ? Steinberg::kResultOk
            : Steinberg::kResultFalse;
    }

    if (std::strcmp(messageId, "SonicPi.StopAll") == 0) {
        return runtime_ && runtime_->stopAllJobs()
            ? Steinberg::kResultOk
            : Steinberg::kResultFalse;
    }
    return Steinberg::kNotImplemented;
}

Steinberg::tresult PLUGIN_API Processor::canProcessSampleSize(
    Steinberg::int32 symbolicSampleSize) {
    return symbolicSampleSize == Steinberg::Vst::kSample32
        ? Steinberg::kResultTrue
        : Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Processor::process(
    Steinberg::Vst::ProcessData& data) {
    if (!engine_ || data.symbolicSampleSize != Steinberg::Vst::kSample32 ||
        !data.outputs || data.numOutputs < 1 || data.numSamples <= 0) {
        clearOutput(data);
        return Steinberg::kResultOk;
    }

    auto& bus = data.outputs[0];
    if (!bus.channelBuffers32 || bus.numChannels < 2 ||
        !bus.channelBuffers32[0] || !bus.channelBuffers32[1]) {
        clearOutput(data);
        return Steinberg::kResultOk;
    }

    Steinberg::int32 offset = 0;
    while (offset < data.numSamples) {
        const auto remaining = static_cast<std::uint32_t>(data.numSamples - offset);
        const auto frames = std::min(remaining, kMaxRenderFrames);
        std::array<float*, 2> outputs{{
            bus.channelBuffers32[0] + offset,
            bus.channelBuffers32[1] + offset,
        }};
        const auto rendered = clockwork_embed_render(
            engine_, outputs.data(), static_cast<std::uint32_t>(outputs.size()),
            nullptr, 0, frames);
        if (rendered != frames) {
            std::fill_n(outputs[0], frames, 0.0f);
            std::fill_n(outputs[1], frames, 0.0f);
        }
        offset += static_cast<Steinberg::int32>(frames);
    }

    for (Steinberg::int32 channel = 2; channel < bus.numChannels; ++channel) {
        if (bus.channelBuffers32[channel]) {
            std::fill_n(bus.channelBuffers32[channel], data.numSamples, 0.0f);
        }
    }
    return Steinberg::kResultOk;
}

bool Processor::startEngine() noexcept {
    if (engine_) return true;
    if (gEngineClaimed.test_and_set(std::memory_order_acquire)) return false;
    ownsProcessEngine_ = true;

    try {
        ClockworkEmbedConfig config{};
        config.struct_bytes = sizeof(config);
        config.sample_rate = sampleRate_;
        config.block_size = 64;
        config.input_channels = 0;
        config.output_channels = 2;
        config.max_render_frames = kMaxRenderFrames;
        config.app_name = "Sonic Pi VST3 Audio Prototype";

        ClockworkStatus status = CLOCKWORK_OK;
        engine_ = clockwork_embed_attach(&config, &status);
        if (!engine_) {
            stopEngine();
            return false;
        }

        client_ = clockwork_embed_client(engine_);
        if (!client_) {
            stopEngine();
            return false;
        }

        runtime_ = std::make_unique<SonicPiLiveRuntime>();
        if (!runtime_->start(client_, sampleRate_)) {
            std::cerr << "Sonic Pi VST runtime startup failed: "
                      << runtime_->error() << '\n';
            stopEngine();
            return false;
        }
        return true;
    } catch (...) {
        stopEngine();
        return false;
    }
}

void Processor::stopEngine() noexcept {
    if (runtime_) {
        runtime_->stop();
        runtime_.reset();
    }
    if (engine_) {
        clockwork_embed_close(engine_);
        engine_ = nullptr;
        client_ = nullptr;
    }
    if (ownsProcessEngine_) {
        gEngineClaimed.clear(std::memory_order_release);
        ownsProcessEngine_ = false;
    }
}

} // namespace SonicPiVst3
