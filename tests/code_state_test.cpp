// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin_ids.h"
#include "sonic_pi_code_state.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace SonicPiVst3;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void rewind(MemoryStream& stream) {
    require(stream.seek(0, IBStream::kIBSeekSet, nullptr) == kResultOk,
            "Could not rewind state");
}

std::string read(MemoryStream& stream) {
    rewind(stream);
    std::string code;
    require(readCodeState(&stream, code), "Could not decode saved state");
    return code;
}

EditorState readFull(MemoryStream& stream) {
    rewind(stream);
    EditorState state;
    require(readEditorState(&stream, state), "Could not decode editor state");
    return state;
}

void testCodec() {
    for (const auto& code : std::vector<std::string>{
             kStarterCode, "", "# Unicode: café 音楽\nplay 72\n",
             std::string(65536, 'x'), std::string(kMaxCodeStateBytes, '#')}) {
        MemoryStream stream;
        require(writeCodeState(&stream, code), "Could not encode code");
        require(read(stream) == code, "Code did not survive state round trip");
    }
    MemoryStream valid;
    require(writeCodeState(&valid, kStarterCode), "Could not prepare state");
    const std::string bytes(valid.getData(), valid.getSize());
    // Every truncated prefix must fail without replacing the existing buffer.
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        MemoryStream truncated(const_cast<char*>(bytes.data()), size);
        std::string code = "keep this";
        require(!readCodeState(&truncated, code), "Truncated state was accepted");
        require(code == "keep this", "Bad state replaced the current code");
    }
    for (const auto offset : {0, 8, 15}) {
        auto bad = bytes;
        bad[offset] = static_cast<char>(0xff);
        MemoryStream stream(bad.data(), bad.size());
        std::string code = "keep this";
        require(!readCodeState(&stream, code), "Invalid state was accepted");
        require(code == "keep this", "Invalid state changed code");
    }
    MemoryStream oversized;
    require(!writeCodeState(
                &oversized, std::string(kMaxCodeStateBytes + 1, 'x')),
            "Oversized code was accepted");
    require(!writeCodeState(nullptr, ""), "Null output stream was accepted");
    std::string unchanged = "keep this";
    require(!readCodeState(nullptr, unchanged), "Null input stream was accepted");
    rewind(valid);
    const auto migrated = readFull(valid);
    require(migrated.code == kStarterCode && migrated.generator == kStarterGenerator,
            "Old project state did not migrate to the default generator");
    for (const auto& state : std::vector<EditorState>{
             {"play 72\n", "# café 音楽\n'play 65\\n'"},
             {"", ""}, {kStarterCode, kStarterGenerator}}) {
        MemoryStream stream;
        require(writeEditorState(&stream, state), "Could not save both buffers");
        require(readFull(stream) == state, "Generator source was lost from state");
        const std::string data(stream.getData(), stream.getSize());
        for (std::size_t size = 0; size < data.size(); ++size) {
            MemoryStream broken(const_cast<char*>(data.data()), size);
            EditorState kept{"keep code", "keep generator"};
            require(!readEditorState(&broken, kept) &&
                    kept == EditorState{"keep code", "keep generator"},
                    "Truncated generator state replaced existing buffers");
        }
    }
    MemoryStream tooLarge;
    require(!writeEditorState(
                &tooLarge, {"play 60", std::string(kMaxGeneratorSourceBytes + 1, ' ')}),
            "Oversized generator source was saved");
}

void testPlugin(const char* path) {
    std::string error;
    auto module = VST3::Hosting::Module::create(path, error);
    if (!module) throw std::runtime_error("Could not load plug-in: " + error);
    const auto& factory = module->getFactory();
    HostApplication host;
    auto processor = factory.createInstance<IComponent>(
        VST3::UID(kProcessorClassId.toTUID()));
    auto controller = factory.createInstance<IEditController>(
        VST3::UID(kControllerClassId.toTUID()));
    require(processor && controller, "Could not create plug-in components");
    require(processor->initialize(&host) == kResultOk, "Processor init failed");
    require(controller->initialize(&host) == kResultOk, "Controller init failed");
    FUnknownPtr<IConnectionPoint> processorPoint(processor);
    FUnknownPtr<IConnectionPoint> controllerPoint(controller);
    require(processorPoint && controllerPoint, "Missing connection points");
    processorPoint->connect(controllerPoint);
    controllerPoint->connect(processorPoint);

    for (const auto& state : std::vector<EditorState>{
             {"use_bpm 123\n# session-only code 音\nplay 67\n", "'play 67\\n'"},
             {"", ""}}) {
        MemoryStream source;
        require(writeEditorState(&source, state), "Could not prepare host state");
        rewind(source);
        require(controller->setState(&source) == kResultOk,
                "Controller refused host state");
        MemoryStream controllerSaved;
        require(controller->getState(&controllerSaved) == kResultOk,
                "Controller state save failed with editor closed");
        require(readFull(controllerSaved) == state, "Controller lost session buffers");
        MemoryStream processorSaved;
        require(processor->getState(&processorSaved) == kResultOk,
                "Processor state save failed");
        require(readFull(processorSaved) == state,
                "Editor state did not reach the processor");

        // Restore into a fresh processor/controller, without opening a GUI or
        // starting audio. This covers hosts which save only component state.
        auto restoredProcessor = factory.createInstance<IComponent>(
            VST3::UID(kProcessorClassId.toTUID()));
        auto restoredController = factory.createInstance<IEditController>(
            VST3::UID(kControllerClassId.toTUID()));
        require(restoredProcessor && restoredController, "Recreation failed");
        require(restoredProcessor->initialize(&host) == kResultOk &&
                    restoredController->initialize(&host) == kResultOk,
                "Restored component init failed");
        rewind(processorSaved);
        require(restoredProcessor->setState(&processorSaved) == kResultOk,
                "Processor restore failed");
        rewind(processorSaved);
        require(restoredController->setComponentState(&processorSaved) == kResultOk,
                "Controller component-state restore failed");
        MemoryStream restored;
        require(restoredController->getState(&restored) == kResultOk,
                "Restored controller save failed");
        require(readFull(restored) == state, "New controller lost restored buffers");
        MemoryStream restoredComponent;
        require(restoredProcessor->getState(&restoredComponent) == kResultOk &&
                    readFull(restoredComponent) == state,
                "New processor lost restored code");

        // Invalid state must not replace already-restored session contents.
        char invalid[] = "not a saved state";
        MemoryStream broken(invalid, sizeof(invalid));
        require(restoredController->setState(&broken) != kResultOk,
                "Controller accepted corrupt state");
        MemoryStream stillRestored;
        require(restoredController->getState(&stillRestored) == kResultOk &&
                    readFull(stillRestored) == state,
                "Corrupt state destroyed session contents");
        restoredController->terminate();
        restoredProcessor->terminate();
    }
    controllerPoint->disconnect(processorPoint);
    processorPoint->disconnect(controllerPoint);
    controller->terminate();
    processor->terminate();
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Pass the built .vst3 bundle path");
        testCodec();
        testPlugin(argv[1]);
        std::cout << "PASS: code-state codec, controller/processor synchronization, "
                     "fresh-instance restore, empty and Unicode code, invalid state\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
