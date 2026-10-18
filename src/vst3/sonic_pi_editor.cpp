// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonic_pi_controller.h"

#include "pluginterfaces/vst/ivstmessage.h"
#include "vstgui/uidescription/uiattributes.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

namespace SonicPiVst3 {
namespace {

constexpr Steinberg::int32 kRunCodeTag = 1000;
constexpr Steinberg::int32 kStopAllTag = 1001;
constexpr Steinberg::int32 kStatusTag = 1002;

const VSTGUI::CColor kEditorTextColor(237, 240, 247, 255);
const VSTGUI::CColor kKeywordColor(198, 120, 221, 255);
const VSTGUI::CColor kBuiltinColor(86, 182, 194, 255);
const VSTGUI::CColor kSymbolColor(224, 108, 117, 255);
const VSTGUI::CColor kStringColor(152, 195, 121, 255);
const VSTGUI::CColor kNumberColor(209, 154, 102, 255);
const VSTGUI::CColor kCommentColor(125, 133, 148, 255);

constexpr std::u32string_view kRubyKeywords[] = {
    U"and", U"begin", U"break", U"case", U"class", U"def", U"do", U"else",
    U"elsif", U"end", U"ensure", U"for", U"if", U"in", U"module", U"next",
    U"not", U"or", U"redo", U"rescue", U"retry", U"return", U"then",
    U"unless", U"until", U"when", U"while", U"yield",
};

constexpr std::u32string_view kRubyLiterals[] = {
    U"false", U"nil", U"true",
};

constexpr std::u32string_view kSonicPiBuiltins[] = {
    U"at", U"choose", U"control", U"cue", U"dice", U"in_thread",
    U"live_loop", U"look", U"play", U"play_chord", U"play_pattern",
    U"play_pattern_timed", U"ring", U"rrand", U"rrand_i", U"sample",
    U"set", U"sleep", U"sound", U"spread", U"stop", U"stop_all_jobs",
    U"sync", U"tick", U"time_warp", U"use_bpm", U"use_cue_logging",
    U"use_debug", U"use_osc_logging", U"use_random_seed", U"use_synth",
    U"use_synth_defaults", U"with_bpm", U"with_fx", U"with_synth",
};

template <std::size_t N>
bool isOneOf(std::u32string_view value,
             const std::u32string_view (&choices)[N]) {
    return std::find(std::begin(choices), std::end(choices), value) !=
           std::end(choices);
}

bool isIdentifierStart(char32_t character) {
    return (character >= U'a' && character <= U'z') ||
           (character >= U'A' && character <= U'Z') || character == U'_';
}

bool isIdentifierContinue(char32_t character) {
    return isIdentifierStart(character) ||
           (character >= U'0' && character <= U'9');
}

bool isDigit(char32_t character) {
    return character >= U'0' && character <= U'9';
}

using HighlightStyles =
    VSTGUI::TextEditorColorization::IStyleProvider::Styles;

void appendStyle(HighlightStyles& styles, std::size_t start,
                 std::size_t end, const VSTGUI::CColor& color) {
    if (end > start) {
        styles.push_back({start, end - start, color});
    }
}

HighlightStyles tokenizeSonicPi(std::u32string_view text) {
    HighlightStyles styles;
    for (std::size_t index = 0; index < text.size();) {
        const char32_t character = text[index];

        if (character == U'#') {
            const std::size_t start = index;
            while (index < text.size() && text[index] != U'\n') ++index;
            appendStyle(styles, start, index, kCommentColor);
            continue;
        }

        if (character == U'\'' || character == U'"') {
            const std::size_t start = index;
            const char32_t quote = character;
            ++index;
            while (index < text.size()) {
                if (text[index] == U'\\') {
                    index += std::min<std::size_t>(2, text.size() - index);
                } else if (text[index++] == quote) {
                    break;
                }
            }
            appendStyle(styles, start, index, kStringColor);
            continue;
        }

        if (character == U':' && index + 1 < text.size() &&
            text[index + 1] != U':' &&
            isIdentifierStart(text[index + 1])) {
            const std::size_t start = index++;
            while (index < text.size() &&
                   isIdentifierContinue(text[index])) {
                ++index;
            }
            if (index < text.size() &&
                (text[index] == U'!' || text[index] == U'?')) {
                ++index;
            }
            appendStyle(styles, start, index, kSymbolColor);
            continue;
        }

        if (isDigit(character)) {
            const std::size_t start = index++;
            while (index < text.size() &&
                   (isDigit(text[index]) || text[index] == U'_')) {
                ++index;
            }
            if (index + 1 < text.size() && text[index] == U'.' &&
                isDigit(text[index + 1])) {
                ++index;
                while (index < text.size() &&
                       (isDigit(text[index]) || text[index] == U'_')) {
                    ++index;
                }
            }
            appendStyle(styles, start, index, kNumberColor);
            continue;
        }

        if (isIdentifierStart(character)) {
            const std::size_t start = index++;
            while (index < text.size() &&
                   isIdentifierContinue(text[index])) {
                ++index;
            }
            if (index < text.size() &&
                (text[index] == U'!' || text[index] == U'?')) {
                ++index;
            }
            const auto identifier = text.substr(start, index - start);
            if (isOneOf(identifier, kRubyKeywords) ||
                isOneOf(identifier, kRubyLiterals)) {
                appendStyle(styles, start, index, kKeywordColor);
            } else if (isOneOf(identifier, kSonicPiBuiltins)) {
                appendStyle(styles, start, index, kBuiltinColor);
            }
            continue;
        }

        ++index;
    }
    return styles;
}

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
        detach(codeEditorView_);
    }

    VSTGUI::CView* createView(
        const VSTGUI::UIAttributes& attributes,
        const VSTGUI::IUIDescription*) override {
        const auto* customViewName =
            attributes.getAttributeValue(VSTGUI::IUIDescription::kCustomViewName);
        if (customViewName && *customViewName == "SonicPiTextEditor") {
            codeEditorView_ =
                VSTGUI::createNewTextEditor(VSTGUI::CRect(), owner_);
            if (codeEditorView_) {
                codeEditorView_->registerViewListener(this);
            }
            return codeEditorView_;
        }
        return nullptr;
    }

    void valueChanged(VSTGUI::CControl* control) override {
        // CTextButton's default kick style resets to its minimum value before
        // controlEndEdit, so dispatch actions while the clicked value is high.
        if (!control || control->getValueNormalized() <= 0.5f) return;

        if (control->getTag() == kRunCodeTag) {
            const bool sent = owner_->sendCurrentCode();
            showStatus(sent ? "Code sent to Sonic Pi"
                            : "Runtime is starting or inactive; try again shortly");
        } else if (control->getTag() == kStopAllTag) {
            const bool sent = owner_->sendStopAll();
            showStatus(sent ? "Stop requested"
                            : "Runtime is starting or inactive; try again shortly");
        }
    }
    void controlBeginEdit(VSTGUI::CControl*) override {}
    void controlEndEdit(VSTGUI::CControl*) override {}

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
        if (view == codeEditorView_) {
            codeEditorView_->unregisterViewListener(this);
            codeEditorView_ = nullptr;
        }
    }

    void viewAttached(VSTGUI::CView* view) override {
        if (view == codeEditorView_) {
            // The starter text is set during construction, before the host has
            // attached this view. Some hosts miss that first invalidation.
            view->invalid();
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
    VSTGUI::CView* codeEditorView_ = nullptr;
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
            VSTGUI::CRect(0, 0, 512, 250), this);
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
    style.font = VSTGUI::makeOwned<VSTGUI::CFontDesc>("Consolas", 16);
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

void Controller::beginDraw(
    const VSTGUI::TextEditorColorization::IEditorExt& editor) {
    syntaxStyles_.clear();
    const auto textLength = editor.getTextLength();
    if (textLength == 0) return;

    editor.readText(0, textLength, [this](std::u32string_view text) {
        syntaxStyles_ = tokenizeSonicPi(text);
    });
}

VSTGUI::TextEditorColorization::IStyleProvider::Styles
Controller::getStyles(
    const VSTGUI::TextEditorColorization::IEditorExt& editor,
    size_t beginOffset, size_t length) {
    using Style = VSTGUI::TextEditorColorization::IStyleProvider::Style;
    VSTGUI::TextEditorColorization::IStyleProvider::Styles result;
    const auto textLength = editor.getTextLength();
    if (beginOffset >= textLength || length == 0) return result;
    const auto endOffset =
        beginOffset + std::min(length, textLength - beginOffset);

    std::size_t cursor = beginOffset;
    for (const auto& tokenStyle : syntaxStyles_) {
        const auto tokenEnd = tokenStyle.start + tokenStyle.length;
        if (tokenEnd <= beginOffset) continue;
        if (tokenStyle.start >= endOffset) break;

        const auto overlapStart = std::max(beginOffset, tokenStyle.start);
        const auto overlapEnd = std::min(endOffset, tokenEnd);
        if (cursor < overlapStart) {
            result.push_back(
                Style{cursor, overlapStart - cursor, kEditorTextColor});
        }
        const auto styleStart = std::max(cursor, overlapStart);
        if (styleStart < overlapEnd) {
            result.push_back(
                Style{styleStart, overlapEnd - styleStart, tokenStyle.color});
            cursor = overlapEnd;
        }
    }

    // VSTGUI's renderer draws the text up to the end of the final style range.
    // Return a default-colored tail as well so trailing plain code is painted.
    if (cursor < endOffset) {
        result.push_back(
            Style{cursor, endOffset - cursor, kEditorTextColor});
    }
    return result;
}

void Controller::endDraw(
    const VSTGUI::TextEditorColorization::IEditorExt&) {}

} // namespace SonicPiVst3
