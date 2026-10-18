// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "vstgui/lib/ctexteditor.h"

#include <string>

namespace SonicPiVst3 {

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
    bool sendCurrentCode();
    bool sendStopAll();

    void onTextEditorCreated(const VSTGUI::ITextEditor& editor) override;
    void onTextEditorDestroyed(const VSTGUI::ITextEditor& editor) override;
    void onTextEditorTextChanged(const VSTGUI::ITextEditor&) override {}

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
};

} // namespace SonicPiVst3
