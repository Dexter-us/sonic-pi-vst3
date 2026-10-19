// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonic_pi_controller.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/main/pluginfactory.h"

#include <iostream>
#include <stdexcept>

using namespace SonicPiVst3;
using namespace Steinberg;
using namespace std::chrono_literals;

// Standalone model test: no host window or platform plug-in entry point.
void* moduleHandle = nullptr;
BEGIN_FACTORY("Dexter U.S. test", "", "", 0)
END_FACTORY

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
GeneratorResult wait(Controller& controller) {
    const auto deadline = std::chrono::steady_clock::now() + 7s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = controller.pollGenerator()) return *result;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Generator result was never delivered");
}
void saveAndRestore(Controller& controller, const EditorState& expected) {
    MemoryStream stream;
    require(controller.getState(&stream) == kResultOk, "Could not save editor");
    stream.seek(0, IBStream::kIBSeekSet, nullptr);
    Controller restored;
    require(restored.setState(&stream) == kResultOk, "Could not restore editor");
    require(restored.currentCode() == expected.code &&
            restored.generatorSource() == expected.generator &&
            !restored.generatorRunning() && !restored.hasGeneratorPreview() &&
            restored.editorMode() == EditorMode::code,
            "Project restore lost source or auto-started the generator");
}
void test(GeneratorOptions options) {
    Steinberg::Vst::HostApplication host;
    Controller controller;
    require(controller.initialize(&host) == kResultOk, "Controller init failed");
    const auto original = controller.currentCode();
    require(controller.setEditorMode(EditorMode::generator), "Could not open Script");
    require(controller.replaceActiveText("\"play 75\\n\""), "Could not edit script");
    require(controller.currentCode() == original, "Script edit changed Sonic Pi code");
    require(!controller.sendCurrentCode() && !controller.sendSelectedCode(),
            "Script mode allowed Sonic Pi execution");
    require(!controller.applyGeneratorPreview(false), "Stale/missing preview was applied");
    require(controller.startGenerator(options), "Could not start generation");
    require(wait(controller).success, "Generation failed");
    require(controller.currentCode() == original &&
            controller.editorMode() == EditorMode::preview,
            "Generate modified the code instead of previewing it");
    require(!controller.replaceActiveText("replace preview") &&
            !controller.sendCurrentCode(), "Preview was editable/executable");
    require(controller.applyGeneratorPreview(false), "Append failed");
    const auto appended = controller.currentCode();
    require(appended.find(original) == 0 && appended.find("play 75\n") != std::string::npos,
            "Append destroyed the original program");
    require(controller.undoGeneratorApply() && controller.currentCode() == original,
            "Undo generator change failed");
    require(controller.undoGeneratorApply() && controller.currentCode() == appended,
            "Redo generator change failed");
    require(controller.replaceActiveText("play 42\n"), "Manual code edit failed");
    require(!controller.undoGeneratorApply() && controller.currentCode() == "play 42\n",
            "Undo generator deleted later manual edits");
    require(controller.applyGeneratorPreview(true) &&
            controller.currentCode() == "play 75\n", "Replace failed");
    saveAndRestore(controller, {"play 75\n", "\"play 75\\n\""});

    controller.setEditorMode(EditorMode::generator);
    controller.replaceActiveText("sleep 0.2; 'play 80'");
    controller.startGenerator(options);
    controller.replaceActiveText("'play 81'");
    require(!wait(controller).success && !controller.hasGeneratorPreview() &&
            controller.currentCode() == "play 75\n",
            "Changing a script did not invalidate its in-flight result");
    controller.setEditorMode(EditorMode::generator);
    controller.replaceActiveText("raise 'expected error'");
    controller.startGenerator(options);
    require(!wait(controller).success && !controller.applyGeneratorPreview(true) &&
            controller.currentCode() == "play 75\n",
            "Ruby failure replaced code");
    controller.setEditorMode(EditorMode::generator);
    controller.replaceActiveText("''");
    controller.startGenerator(options);
    require(wait(controller).success && !controller.applyGeneratorPreview(true) &&
            controller.currentCode() == "play 75\n",
            "Empty output deleted existing code");
    controller.setEditorMode(EditorMode::generator);
    controller.replaceActiveText("sleep 5; 'play 80'");
    controller.startGenerator(options);
    controller.cancelGenerator();
    require(wait(controller).cancelled &&
            controller.currentCode() == "play 75\n", "Cancel modified code");
    controller.setEditorMode(EditorMode::code);
    controller.replaceActiveText(std::string(kMaxGeneratorOutputBytes, '#'));
    controller.setEditorMode(EditorMode::generator);
    controller.replaceActiveText("'play 81'");
    controller.startGenerator(options);
    require(wait(controller).success && !controller.applyGeneratorPreview(false) &&
            controller.currentCode().size() == kMaxGeneratorOutputBytes,
            "Oversized append damaged code");
    controller.setEditorMode(EditorMode::generator);
    controller.replaceActiveText("sleep 5; 'play 90'");
    controller.startGenerator(options);
    MemoryStream restored;
    require(writeEditorState(&restored, {"play 44\n", "'play 45'"}),
            "Could not prepare project restore");
    restored.seek(0, IBStream::kIBSeekSet, nullptr);
    require(controller.setState(&restored) == kResultOk &&
            !wait(controller).success && controller.currentCode() == "play 44\n" &&
            controller.generatorSource() == "'play 45'" &&
            controller.editorMode() == EditorMode::code &&
            !controller.hasGeneratorPreview(),
            "Project restoration allowed an old generator result to overwrite source");
    controller.terminate();
}
} // namespace

int main(int argc, char** argv) {
    try {
        GeneratorOptions options;
        if (argc > 1) options.rubyExecutable =
            std::filesystem::path(reinterpret_cast<const char8_t*>(argv[1]));
        test(options);
        std::cout << "PASS: separate buffers, preview, append/replace/undo, "
                     "later-edit protection, state restore, stale results, errors, cancel\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
