// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonic_pi_controller.h"

#include "pluginterfaces/vst/ivstmessage.h"

#include <cstring>
#include <string>

namespace SonicPiVst3 {
namespace {

constexpr Steinberg::int32 kRunCodeTag = 1000;
constexpr Steinberg::int32 kStopAllTag = 1001;
constexpr Steinberg::int32 kStatusTag = 1002;

constexpr char kStarterCode[] =
    "use_bpm 110\n"
    "\n"
    "live_loop :pulse do\n"
    "  play 60, amp: 0.5\n"
    "  sleep 1\n"
    "end";

class LiveCodeSubController final : public VSTGUI::IController,
                                    public VSTGUI::ViewListenerAdapter {
public:
    explicit LiveCodeSubController(Controller* owner) : owner_(owner) {}

    ~LiveCodeSubController() override {
        detach(statusLabel_);
    }

    void valueChanged(VSTGUI::CControl*) override {}
    void controlBeginEdit(VSTGUI::CControl*) override {}

    void controlEndEdit(VSTGUI::CControl* control) override {
        if (!control) return;
        if (control->getTag() == kRunCodeTag &&
            control->getValueNormalized() > 0.5f) {
            const bool sent = owner_->sendCurrentCode();
            showStatus(sent ? "Code sent to Sonic Pi"
                            : "Runtime is starting or inactive; try again shortly");
            control->setValue(0.0f);
            control->invalid();
        } else if (control->getTag() == kStopAllTag &&
                   control->getValueNormalized() > 0.5f) {
            const bool sent = owner_->sendStopAll();
            showStatus(sent ? "Stop requested"
                            : "Runtime is starting or inactive; try again shortly");
            control->setValue(0.0f);
            control->invalid();
        }
    }

    VSTGUI::CView* verifyView(
        VSTGUI::CView* view,
        const VSTGUI::UIAttributes&,
        const VSTGUI::IUIDescription*) override {
        if (auto* label = dynamic_cast<VSTGUI::CTextLabel*>(view)) {
            if (label->getTag() == kStatusTag) {
                statusLabel_ = label;
                statusLabel_->registerViewListener(this);
                statusLabel_->setText("Runtime starts when the VST is activated");
            }
        }
        return view;
    }

    void viewWillDelete(VSTGUI::CView* view) override {
        if (view == statusLabel_) {
            statusLabel_->unregisterViewListener(this);
            statusLabel_ = nullptr;
        }
    }

private:
    void detach(VSTGUI::CView* view) {
        if (view) {
            view->unregisterViewListener(this);
        }
    }

    void showStatus(const char* text) {
        if (statusLabel_) statusLabel_->setText(text);
    }

    Controller* owner_ = nullptr;
    VSTGUI::CTextLabel* statusLabel_ = nullptr;
};

} // namespace

Controller::~Controller() {
    if (codeEditor_) codeEditor_->resetController();
}

Steinberg::IPlugView* PLUGIN_API Controller::createView(const char* name) {
    if (name && std::strcmp(name, Steinberg::Vst::ViewType::kEditor) == 0) {
        return new VSTGUI::VST3Editor(this, "view", "sonic_pi.uidesc");
    }
    return nullptr;
}

VSTGUI::CView* Controller::createCustomView(
    VSTGUI::UTF8StringPtr name,
    const VSTGUI::UIAttributes&,
    const VSTGUI::IUIDescription*,
    VSTGUI::VST3Editor*) {
    if (name && std::strcmp(name, "SonicPiTextEditor") == 0) {
        return VSTGUI::createNewTextEditor(
            VSTGUI::CRect(0, 0, 672, 324), this);
    }
    return nullptr;
}

VSTGUI::IController* Controller::createSubController(
    VSTGUI::UTF8StringPtr name,
    const VSTGUI::IUIDescription*,
    VSTGUI::VST3Editor*) {
    if (name && std::strcmp(name, "LiveCodeController") == 0) {
        return new LiveCodeSubController(this);
    }
    return nullptr;
}

bool Controller::sendRunCode(const std::string& code) {
    if (code.empty() || code.size() > 32000) return false;
    auto message = Steinberg::owned(allocateMessage());
    if (!message) return false;
    message->setMessageID("SonicPi.RunCode");
    const auto result = message->getAttributes()->setBinary(
        "code", code.data(), static_cast<Steinberg::uint32>(code.size()));
    return result == Steinberg::kResultOk && sendMessage(message) == Steinberg::kResultOk;
}

bool Controller::sendCurrentCode() {
    return codeEditor_ && sendRunCode(codeEditor_->getPlainText());
}

bool Controller::sendStopAll() {
    auto message = Steinberg::owned(allocateMessage());
    if (!message) return false;
    message->setMessageID("SonicPi.StopAll");
    return sendMessage(message) == Steinberg::kResultOk;
}

void Controller::onTextEditorCreated(const VSTGUI::ITextEditor& editor) {
    codeEditor_ = &editor;
    VSTGUI::ITextEditor::Style style;
    style.font = VSTGUI::makeOwned<VSTGUI::CFontDesc>("monospace", 14);
    style.textColor = VSTGUI::CColor(237, 240, 247, 255);
    style.backColor = VSTGUI::CColor(16, 19, 25, 255);
    style.cursorColor = VSTGUI::CColor(237, 240, 247, 255);
    style.frameColor = VSTGUI::CColor(52, 58, 70, 255);
    editor.setStyle(style);
    editor.setPlainText(kStarterCode, true);
}

void Controller::onTextEditorDestroyed(const VSTGUI::ITextEditor& editor) {
    if (codeEditor_ == &editor) codeEditor_ = nullptr;
}

Steinberg::tresult PLUGIN_API Controller::queryInterface(
    const char* iid, void** obj) {
    return EditController::queryInterface(iid, obj);
}

} // namespace SonicPiVst3
