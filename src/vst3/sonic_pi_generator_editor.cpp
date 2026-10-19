// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonic_pi_controller.h"
#include "pluginterfaces/vst/ivstmessage.h"

namespace SonicPiVst3 {

Steinberg::tresult PLUGIN_API Controller::getState(Steinberg::IBStream* state) {
    return writeEditorState(state, {codeState_, generatorSource_})
        ? Steinberg::kResultOk : Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Controller::setState(Steinberg::IBStream* state) {
    EditorState decoded;
    if (!readEditorState(state, decoded)) return Steinberg::kResultFalse;
    generator_.cancel();
    ++generatorRevision_;
    generatorPreviewValid_ = false;
    generatorPreview_.clear();
    codeBeforeGenerator_.reset();
    codeState_ = decoded.code;
    generatorSource_ = decoded.generator;
    restoredStateForUi_ = std::move(decoded);
    generatorMessage_ = "Project restored. Generators run only on request.";
    bufferWarning_.clear();
    if (!setEditorMode(EditorMode::code)) return Steinberg::kResultFalse;
    syncCodeState();
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Controller::setComponentState(Steinberg::IBStream* state) {
    return setState(state);
}

bool Controller::takeRestoredState(EditorState& state) {
    if (!restoredStateForUi_) return false;
    state = std::move(*restoredStateForUi_);
    restoredStateForUi_.reset();
    return true;
}

bool Controller::syncCodeState() {
    if (codeState_.size() > kMaxCodeStateBytes ||
        generatorSource_.size() > kMaxGeneratorSourceBytes) return false;
    auto message = Steinberg::owned(allocateMessage());
    if (!message) return false;
    message->setMessageID("SonicPi.EditorState");
    auto* attributes = message->getAttributes();
    if (!attributes ||
        attributes->setBinary("code", codeState_.data(), codeState_.size()) !=
            Steinberg::kResultOk ||
        attributes->setBinary(
            "generator", generatorSource_.data(), generatorSource_.size()) !=
            Steinberg::kResultOk) return false;
    return sendMessage(message) == Steinberg::kResultOk;
}

std::string Controller::currentEditorText() const {
    switch (editorMode_) {
    case EditorMode::generator: return generatorSource_;
    case EditorMode::preview: return generatorPreview_;
    default: return codeState_;
    }
}

void Controller::configureEditorStyle() {
    if (!codeEditor_) return;
    VSTGUI::ITextEditor::Style style;
    style.font = VSTGUI::makeOwned<VSTGUI::CFontDesc>("Consolas", 16);
    style.textColor = VSTGUI::CColor(237, 240, 247, 255);
    style.backColor = VSTGUI::CColor(16, 19, 25, 255);
    style.cursorColor = VSTGUI::CColor(237, 240, 247, 255);
    style.frameColor = VSTGUI::CColor(52, 58, 70, 255);
    if (editorMode_ == EditorMode::preview) {
        using Flags = VSTGUI::ITextEditor::Style::Flags;
        style.flags = {{Flags::ShowLineNumbers, Flags::ReadOnlyMode}};
    }
    codeEditor_->setStyle(style);
}

bool Controller::setEditorMode(EditorMode mode) {
    const auto previous = editorMode_;
    editorMode_ = mode;
    if (codeEditor_) {
        restoringCode_ = true;
        const auto text = currentEditorText();
        const bool set = codeEditor_->setPlainText(text, true);
        restoringCode_ = false;
        if (!set) {
            editorMode_ = previous;
            return false;
        }
        configureEditorStyle();
    }
    return true;
}

bool Controller::replaceActiveText(const std::string& text) {
    if (editorMode_ == EditorMode::preview) return false;
    if (editorMode_ == EditorMode::generator) {
        if (text.size() > kMaxGeneratorSourceBytes) return false;
        generatorSource_ = text;
        ++generatorRevision_;
        generatorPreviewValid_ = false;
    } else {
        if (text.size() > kMaxCodeStateBytes) return false;
        codeState_ = text;
    }
    if (!setEditorMode(editorMode_)) return false;
    bufferWarning_.clear();
    setDirty(true);
    syncCodeState();
    return true;
}

bool Controller::startGenerator(GeneratorOptions options) {
    if (!generator_.start(generatorSource_, options)) return false;
    generatorRequestRevision_ = generatorRevision_;
    generatorPreviewValid_ = false;
    generatorMessage_ = "Generating in a separate Ruby process (5 second limit)...";
    return true;
}

std::optional<GeneratorResult> Controller::pollGenerator() {
    auto result = generator_.takeResult();
    if (!result) return {};
    if (generatorRequestRevision_ != generatorRevision_) {
        result->success = false;
        result->output.clear();
        result->error = "Script/project changed during generation; generate again.";
    }
    if (result->success) {
        generatorPreview_ = result->output;
        generatorPreviewValid_ = true;
        generatorMessage_ = generatorPreview_.empty()
            ? "Generator returned an empty String; no code will be changed."
            : "Preview ready. Append or Replace changes code; Run is separate.";
        if (!result->error.empty()) generatorMessage_ += "\nRuby: " + result->error;
        if (editorMode_ != EditorMode::code) setEditorMode(EditorMode::preview);
    } else {
        generatorMessage_ = result->error;
        generatorPreview_ = "Generator error:\n" + result->error;
        generatorPreviewValid_ = false;
        if (editorMode_ != EditorMode::code) setEditorMode(EditorMode::preview);
    }
    return result;
}

bool Controller::applyGeneratorPreview(bool replace) {
    if (!generatorPreviewValid_ || generatorPreview_.empty() || generator_.running()) {
        return false;
    }
    auto next = replace ? std::string{} : codeState_;
    if (!next.empty() && next.back() != '\n') next += '\n';
    if (!next.empty()) next += '\n';
    next += generatorPreview_;
    if (next.size() > kMaxGeneratorOutputBytes) {
        generatorMessage_ = "Combined source exceeds 32000 bytes; shorten it or use Replace.";
        return false;
    }
    const auto previous = codeState_;
    codeState_ = std::move(next);
    if (!setEditorMode(EditorMode::code)) {
        codeState_ = previous;
        return false;
    }
    codeBeforeGenerator_ = previous;
    codeAfterGenerator_ = codeState_;
    setDirty(true);
    syncCodeState();
    return true;
}

bool Controller::undoGeneratorApply() {
    if (!codeBeforeGenerator_ || codeState_ != codeAfterGenerator_) return false;
    // Swap retains a way back if Undo is pressed by mistake.
    codeState_.swap(*codeBeforeGenerator_);
    codeAfterGenerator_ = codeState_;
    setEditorMode(EditorMode::code);
    setDirty(true);
    syncCodeState();
    return true;
}

} // namespace SonicPiVst3
