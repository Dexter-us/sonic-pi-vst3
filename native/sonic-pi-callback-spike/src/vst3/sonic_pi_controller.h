// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"
#include "sonic_pi_runtime_status.h"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "vstgui/lib/ctexteditor.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>

namespace SonicPiVst3 {

struct RuntimeDiagnostic {
    int editorLine = 0;
    int submittedLine = 0;
    bool lineMapped = false;
    std::string message;
};

class Controller final : public Steinberg::Vst::EditController,
                         public VSTGUI::VST3EditorDelegate,
                         public VSTGUI::TextEditorControllerAdapter,
                         public VSTGUI::TextEditorColorization::IStyleProvider {
public:
    ~Controller() override;

    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IEditController*>(new Controller());
    }

    Steinberg::IPlugView* PLUGIN_API createView(const char* name) override;
    VSTGUI::CView* createCustomView(
        VSTGUI::UTF8StringPtr name,
        const VSTGUI::UIAttributes& attributes,
        const VSTGUI::IUIDescription* description,
        VSTGUI::VST3Editor* editor) override;
    VSTGUI::IController* createSubController(
        VSTGUI::UTF8StringPtr name,
        const VSTGUI::IUIDescription* description,
        VSTGUI::VST3Editor* editor) override;

    bool sendRunCode(const std::string& code);
    std::string currentCode() const;
    std::string selectedText() const;
    bool replaceCurrentCode(const std::string& code);
    bool insertTextAtCursor(std::string_view text);
    bool sendCurrentCode();
    bool sendSelectedCode();
    bool goToRuntimeError();
    bool sendStopAll();
    bool requestRuntimeStatus();
    bool requestRuntimeLog();
    RuntimeStatus runtimeStatus() const noexcept;
    std::string runtimeLog() const;
    RuntimeDiagnostic runtimeDiagnostic() const;
    Steinberg::tresult PLUGIN_API notify(
        Steinberg::Vst::IMessage* message) override;

    void onTextEditorCreated(const VSTGUI::ITextEditor& editor) override;
    void onTextEditorDestroyed(const VSTGUI::ITextEditor& editor) override;
    void onTextEditorTextChanged(const VSTGUI::ITextEditor&) override;

    void beginDraw(
        const VSTGUI::TextEditorColorization::IEditorExt& editor) override;
    VSTGUI::TextEditorColorization::IStyleProvider::Styles getStyles(
        const VSTGUI::TextEditorColorization::IEditorExt& editor,
        size_t beginOffset, size_t length) override;
    void endDraw(
        const VSTGUI::TextEditorColorization::IEditorExt& editor) override;

    DELEGATE_REFCOUNT(EditController)
    Steinberg::tresult PLUGIN_API queryInterface(
        const char* iid, void** obj) override;

private:
    const VSTGUI::ITextEditor* codeEditor_ = nullptr;
    VSTGUI::TextEditorColorization::IStyleProvider::Styles syntaxStyles_;
    std::atomic<RuntimeStatus> runtimeStatus_{RuntimeStatus::inactive};
    mutable std::mutex runtimeLogMutex_;
    std::string runtimeLog_;
    RuntimeDiagnostic runtimeDiagnostic_;
    std::size_t runSourceLineOffset_ = 0;
    std::size_t runSourceLineCount_ = 0;
    bool runSourceMapped_ = false;
    std::size_t errorStyleStart_ = 0;
    std::size_t errorStyleEnd_ = 0;
};

} // namespace SonicPiVst3
