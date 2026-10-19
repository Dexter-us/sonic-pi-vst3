// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonic_pi_controller.h"
#include "sonic_pi_editor_help.h"
#include "sonic_pi_portable.h"
#include "plugin_ids.h"

#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/base/ustring.h"
#include "vstgui/lib/cdropsource.h"
#include "vstgui/lib/cframe.h"
#include "vstgui/lib/cfileselector.h"
#include "vstgui/lib/controls/ctextlabel.h"
#include "vstgui/lib/cvstguitimer.h"
#include "vstgui/lib/idatapackage.h"
#include "vstgui/lib/controls/coptionmenu.h"
#include "vstgui/uidescription/uiattributes.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace SonicPiVst3 {
namespace {

constexpr Steinberg::int32 kRunCodeTag = 1000;
constexpr Steinberg::int32 kStopAllTag = 1001;
constexpr Steinberg::int32 kStatusTag = 1002;
constexpr Steinberg::int32 kOpenCodeTag = 1003;
constexpr Steinberg::int32 kSaveCodeTag = 1004;
constexpr Steinberg::int32 kDocumentTitleTag = 1005;
constexpr Steinberg::int32 kCompleteTag = 1006;
constexpr Steinberg::int32 kSnippetsTag = 1007;
constexpr Steinberg::int32 kRunSelectionTag = 1008;
constexpr Steinberg::int32 kLiveLoopsTag = 1009;
constexpr Steinberg::int32 kRuntimeLogTag = 1010;
constexpr Steinberg::int32 kGoToErrorTag = 1011;
constexpr Steinberg::int32 kCodeHelpLabelTag = 1012;
constexpr Steinberg::int32 kCodeHelpTag = 1013;
constexpr Steinberg::int32 kCodeViewTag = 1014;
constexpr Steinberg::int32 kGeneratorViewTag = 1015;
constexpr Steinberg::int32 kPreviewViewTag = 1016;
constexpr Steinberg::int32 kGenerateTag = 1017;
constexpr Steinberg::int32 kAppendGeneratedTag = 1018;
constexpr Steinberg::int32 kReplaceGeneratedTag = 1019;
constexpr Steinberg::int32 kUndoGeneratedTag = 1020;
constexpr Steinberg::int32 kEditorModeTag = 1021;
constexpr Steinberg::int32 kEditorHelpTag = 1022;

const char* statusText(RuntimeStatus status) {
    switch (status) {
        case RuntimeStatus::starting:
            return "Sonic Pi runtime starting...";
        case RuntimeStatus::ready:
            return "Sonic Pi ready - press Run";
        case RuntimeStatus::failed:
            return "Startup failed - check spider-runtime.log";
        case RuntimeStatus::queued:
            return "Code queued until runtime is ready";
        case RuntimeStatus::submitted:
            return "Code sent to Sonic Pi";
        case RuntimeStatus::stopped:
            return "Sonic Pi jobs stopped";
        case RuntimeStatus::error:
            return "Command failed - check spider-runtime.log";
        case RuntimeStatus::inactive:
        default:
            return "Runtime inactive - activate the plug-in";
    }
}

std::string fileNameFromUtf8Path(const std::string& path) {
    const auto separator = path.find_last_of("/\\");
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

std::string normalizeLineEndings(std::string text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            normalized.push_back('\n');
            if (index + 1 < text.size() && text[index + 1] == '\n') ++index;
        } else {
            normalized.push_back(text[index]);
        }
    }
    return normalized;
}

std::filesystem::path pathFromUtf8(const std::string& path) {
    const auto* begin = reinterpret_cast<const char8_t*>(path.data());
    return std::filesystem::path(std::u8string(begin, begin + path.size()));
}

bool readCodeFile(const std::string& utf8Path, std::string& code) {
    std::ifstream input(pathFromUtf8(utf8Path), std::ios::binary);
    if (!input) return false;

    code.resize(kMaxCodeStateBytes + 1);
    input.read(code.data(), static_cast<std::streamsize>(code.size()));
    code.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad() || code.size() > kMaxCodeStateBytes ||
        !isUtf8Source(code)) {
        code.clear();
        return false;
    }

    // Remove an optional UTF-8 BOM and normalize Windows/Mac line endings for
    // the editor's newline-based line numbering and syntax highlighting.
    if (code.size() >= 3 &&
        static_cast<unsigned char>(code[0]) == 0xEF &&
        static_cast<unsigned char>(code[1]) == 0xBB &&
        static_cast<unsigned char>(code[2]) == 0xBF) {
        code.erase(0, 3);
    }
    code = normalizeLineEndings(std::move(code));
    return true;
}

struct LiveLoopBlock {
    std::string label;
    std::string code;
};

bool isRubyWordStart(char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') || character == '_';
}

bool isRubyWordCharacter(char character) {
    return isRubyWordStart(character) ||
           (character >= '0' && character <= '9');
}

std::vector<std::string_view> rubyTokens(std::string_view line) {
    std::vector<std::string_view> tokens;
    char quote = 0;
    bool escaped = false;
    for (std::size_t index = 0; index < line.size();) {
        const char character = line[index];
        if (quote) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == quote) {
                quote = 0;
            }
            ++index;
            continue;
        }
        if (character == '#') break;
        if (character == '\'' || character == '"' || character == '`') {
            quote = character;
            ++index;
            continue;
        }
        if (!isRubyWordStart(character)) {
            ++index;
            continue;
        }
        const std::size_t start = index++;
        while (index < line.size() && isRubyWordCharacter(line[index])) {
            ++index;
        }
        if (index < line.size() &&
            (line[index] == '!' || line[index] == '?')) {
            ++index;
        }
        tokens.push_back(line.substr(start, index - start));
    }
    return tokens;
}

bool isRubyBlockStarter(std::string_view token) {
    return token == "begin" || token == "case" || token == "class" ||
           token == "def" || token == "for" || token == "if" ||
           token == "module" || token == "unless" || token == "until" ||
           token == "while";
}

bool isRubyLoopKeyword(std::string_view token) {
    return token == "for" || token == "until" || token == "while";
}

bool findRubyBlockEnd(std::string_view source,
                      std::size_t firstLineStart,
                      std::size_t& blockEnd) {
    std::size_t depth = 0;
    std::size_t lineStart = firstLineStart;
    while (lineStart <= source.size()) {
        const auto newline = source.find('\n', lineStart);
        const auto lineEnd = newline == std::string_view::npos
            ? source.size()
            : newline;
        const auto tokens = rubyTokens(
            source.substr(lineStart, lineEnd - lineStart));
        const bool startsBlock = !tokens.empty() &&
                                 isRubyBlockStarter(tokens.front());
        const bool loopKeyword = startsBlock &&
                                 isRubyLoopKeyword(tokens.front());
        if (startsBlock) ++depth;

        for (std::size_t index = startsBlock ? 1u : 0u;
             index < tokens.size(); ++index) {
            if (tokens[index] == "do") {
                // A while/for/until block may spell its opener with "do";
                // it is the same block, not a second nesting level.
                if (!loopKeyword) ++depth;
            } else if (tokens[index] == "end") {
                if (depth == 0) return false;
                --depth;
                if (depth == 0) {
                    blockEnd = lineEnd;
                    return true;
                }
            }
        }

        if (newline == std::string_view::npos) break;
        lineStart = newline + 1;
    }
    return false;
}

std::vector<LiveLoopBlock> findLiveLoopBlocks(std::string_view source) {
    std::vector<LiveLoopBlock> loops;
    std::size_t lineStart = 0;
    std::size_t lineNumber = 1;
    while (lineStart < source.size()) {
        const auto newline = source.find('\n', lineStart);
        const auto lineEnd = newline == std::string_view::npos
            ? source.size()
            : newline;
        const auto line = source.substr(lineStart, lineEnd - lineStart);
        const auto tokens = rubyTokens(line);
        if (!tokens.empty() && tokens.front() == "live_loop") {
            std::size_t blockEnd = lineEnd;
            if (findRubyBlockEnd(source, lineStart, blockEnd)) {
                std::string header(line);
                const auto firstCharacter = header.find_first_not_of(" \t");
                if (firstCharacter != std::string::npos) {
                    header.erase(0, firstCharacter);
                }
                constexpr std::size_t kMaxHeaderLength = 52;
                if (header.size() > kMaxHeaderLength) {
                    header.resize(kMaxHeaderLength - 3);
                    header += "...";
                }
                loops.push_back({
                    "Line " + std::to_string(lineNumber) + ": " + header,
                    std::string(source.substr(lineStart, blockEnd - lineStart))
                });
            }
        }
        if (newline == std::string_view::npos) break;
        lineStart = newline + 1;
        ++lineNumber;
    }
    return loops;
}

std::string formatRuntimeLog(std::string log) {
    log = normalizeLineEndings(std::move(log));
    std::vector<std::string_view> lines;
    std::size_t lineStart = 0;
    while (lineStart < log.size()) {
        const auto newline = log.find('\n', lineStart);
        const auto lineEnd = newline == std::string::npos ? log.size() : newline;
        auto line = std::string_view(log).substr(lineStart, lineEnd - lineStart);
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            line.remove_prefix(first);
            while (!line.empty() &&
                   (line.back() == ' ' || line.back() == '\t')) {
                line.remove_suffix(1);
            }
            lines.push_back(line);
        }
        if (newline == std::string::npos) break;
        lineStart = newline + 1;
    }

    if (lines.empty()) return "Waiting for Sonic Pi runtime output...";
    std::string display;
    const auto firstLine = lines.size() > 3 ? lines.size() - 3 : 0;
    for (std::size_t index = firstLine; index < lines.size(); ++index) {
        if (!display.empty()) display.push_back('\n');
        const auto line = lines[index];
        constexpr std::size_t kMaxDisplayLineLength = 82;
        if (line.size() > kMaxDisplayLineLength) {
            display.append(line.substr(0, kMaxDisplayLineLength - 3));
            display += "...";
        } else {
            display.append(line);
        }
    }
    return display;
}

struct RuntimeErrorEvent {
    int line = 0;
    std::string message;
};

std::string appendedLogText(std::string_view previous,
                            std::string_view current) {
    if (previous.empty()) return std::string(current);
    if (current.size() >= previous.size() &&
        current.substr(0, previous.size()) == previous) {
        return std::string(current.substr(previous.size()));
    }

    const auto maxOverlap = std::min(previous.size(), current.size());
    for (std::size_t overlap = maxOverlap; overlap > 0; --overlap) {
        if (previous.substr(previous.size() - overlap, overlap) ==
            current.substr(0, overlap)) {
            return std::string(current.substr(overlap));
        }
    }
    return std::string(current);
}

std::vector<RuntimeErrorEvent> parseRuntimeErrorEvents(
    std::string_view appended) {
    std::vector<RuntimeErrorEvent> events;
    constexpr std::string_view kPrefix = "[ruby-error] ";
    std::size_t lineStart = 0;
    while (lineStart < appended.size()) {
        const auto newline = appended.find('\n', lineStart);
        const auto lineEnd =
            newline == std::string_view::npos ? appended.size() : newline;
        auto line = appended.substr(lineStart, lineEnd - lineStart);
        if (line.starts_with(kPrefix) &&
            (line.find("SyntaxError") != std::string_view::npos ||
             line.find("Exception") != std::string_view::npos)) {
            const auto marker = line.find(" line=");
            if (marker != std::string_view::npos) {
                const auto numberStart = marker + 6;
                const auto colon = line.find(':', numberStart);
                if (colon != std::string_view::npos) {
                    int reportedLine = 0;
                    const auto parseResult = std::from_chars(
                        line.data() + numberStart, line.data() + colon,
                        reportedLine);
                    if (parseResult.ec == std::errc{} &&
                        parseResult.ptr == line.data() + colon &&
                        reportedLine != 0) {
                        auto message = line.substr(colon + 1);
                        const auto first =
                            message.find_first_not_of(" \t");
                        if (first != std::string_view::npos) {
                            message.remove_prefix(first);
                        }
                        events.push_back(
                            {reportedLine, std::string(message)});
                    }
                }
            }
        }
        if (newline == std::string_view::npos) break;
        lineStart = newline + 1;
    }
    return events;
}

bool findUniqueSourceLineOffset(std::string_view source,
                                std::string_view submitted,
                                std::size_t& lineOffset,
                                std::size_t& sourceLineCount) {
    if (source.empty() || submitted.empty()) return false;
    const auto first = source.find(submitted);
    if (first == std::string_view::npos ||
        source.find(submitted, first + 1) != std::string_view::npos) {
        return false;
    }

    lineOffset = static_cast<std::size_t>(
        std::count(source.begin(), source.begin() +
                   static_cast<std::ptrdiff_t>(first), '\n'));
    sourceLineCount =
        1 + static_cast<std::size_t>(std::count(source.begin(), source.end(), '\n'));
    return true;
}

std::string_view sourceLineAt(std::string_view source, std::size_t lineNumber) {
    if (lineNumber == 0) return {};
    std::size_t lineStart = 0;
    for (std::size_t line = 1; line < lineNumber; ++line) {
        const auto newline = source.find('\n', lineStart);
        if (newline == std::string_view::npos) return {};
        lineStart = newline + 1;
    }
    const auto lineEnd = source.find('\n', lineStart);
    auto result = source.substr(
        lineStart, lineEnd == std::string_view::npos
                       ? source.size() - lineStart
                       : lineEnd - lineStart);
    if (!result.empty() && result.back() == '\r') result.remove_suffix(1);
    return result;
}

std::string chooseCodeFile(VSTGUI::CControl* control, bool save,
                           const std::filesystem::path& directory, bool generator) {
    auto* selector = VSTGUI::CNewFileSelector::create(
        control ? control->getFrame() : nullptr,
        save ? VSTGUI::CNewFileSelector::kSelectSaveFile
             : VSTGUI::CNewFileSelector::kSelectFile);
    if (!selector) return {};

    const VSTGUI::CFileExtension rubySource(
        "Sonic Pi code (*.rb)", "rb", "text/x-ruby");
    selector->setTitle(generator
        ? (save ? "Save Ruby generator" : "Open Ruby generator")
        : (save ? "Save Sonic Pi code" : "Open Sonic Pi code"));
    const auto initialDirectory = pathUtf8(directory);
    selector->setInitialDirectory(initialDirectory.c_str());
    selector->addFileExtension(rubySource);
    selector->addFileExtension(
        VSTGUI::CFileExtension("Text files (*.txt)", "txt", "text/plain"));
    selector->addFileExtension(VSTGUI::CNewFileSelector::getAllFilesExtension());
    if (save) {
        selector->setDefaultExtension(rubySource);
        selector->setDefaultSaveName("untitled.rb");
    }

    std::string selectedPath;
    if (selector->runModal() && selector->getNumSelectedFiles() > 0) {
        if (const auto* selected = selector->getSelectedFile(0)) {
            selectedPath = selected;
        }
    }
    selector->forget();
    return selectedPath;
}

const VSTGUI::CColor kEditorTextColor(237, 240, 247, 255);
const VSTGUI::CColor kKeywordColor(198, 120, 221, 255);
const VSTGUI::CColor kBuiltinColor(86, 182, 194, 255);
const VSTGUI::CColor kSymbolColor(224, 108, 117, 255);
const VSTGUI::CColor kStringColor(152, 195, 121, 255);
const VSTGUI::CColor kNumberColor(209, 154, 102, 255);
const VSTGUI::CColor kCommentColor(125, 133, 148, 255);
const VSTGUI::CColor kErrorColor(255, 105, 105, 255);

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
    U"at", U"choose", U"control", U"cue", U"dice", U"define",
    U"in_thread", U"live_loop", U"look", U"play", U"play_chord",
    U"play_pattern", U"play_pattern_timed", U"ring", U"rrand",
    U"rrand_i", U"sample", U"set", U"sleep", U"sound", U"spread",
    U"stop", U"stop_all_jobs", U"sync", U"tick", U"time_warp",
    U"use_bpm", U"use_cue_logging", U"use_debug", U"use_osc_logging",
    U"use_random_seed", U"use_synth", U"use_synth_defaults", U"with_bpm",
    U"with_fx", U"with_synth",
};

struct EditorChoice {
    std::string_view label;
    std::string_view insertion;
};

constexpr EditorChoice kCompletionChoices[] = {
    {"play - play a note", "play"},
    {"sample - play a sample", "sample"},
    {"live_loop - repeating loop", "live_loop"},
    {"with_fx - apply an effect", "with_fx"},
    {"use_synth - select a synth", "use_synth"},
    {"use_bpm - set tempo", "use_bpm"},
    {"sleep - wait between events", "sleep"},
    {"sync - wait for a cue", "sync"},
    {"cue - send a cue", "cue"},
    {"play_chord - play notes together", "play_chord"},
    {"play_pattern_timed - timed note pattern", "play_pattern_timed"},
    {"in_thread - start a thread", "in_thread"},
    {"stop - stop a live loop", "stop"},
    {"stop_all_jobs - stop all code", "stop_all_jobs"},
    {"use_random_seed - repeat randomness", "use_random_seed"},
    {":prophet - synth", ":prophet"},
    {":tb303 - synth", ":tb303"},
    {":saw - synth", ":saw"},
    {":pulse - synth", ":pulse"},
    {":fm - synth", ":fm"},
    {":reverb - effect", ":reverb"},
    {":echo - effect", ":echo"},
    {":slicer - effect", ":slicer"},
    {":wobble - effect", ":wobble"},
    {":distortion - effect", ":distortion"},
    {":bd_haus - sample", ":bd_haus"},
    {":bd_tek - sample", ":bd_tek"},
    {":drum_heavy_kick - sample", ":drum_heavy_kick"},
    {":drum_snare_hard - sample", ":drum_snare_hard"},
    {":drum_cymbal_closed - sample", ":drum_cymbal_closed"},
    {":loop_amen - sample", ":loop_amen"},
    {":ambi_lunar_land - sample", ":ambi_lunar_land"},
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

class LiveCodeSubController final : public VSTGUI::IController,
                                    public VSTGUI::ViewListenerAdapter,
                                    public VSTGUI::IKeyboardHook {
public:
    explicit LiveCodeSubController(Controller* owner) : owner_(owner) {}

    ~LiveCodeSubController() override {
        statusTimer_ = nullptr;
        if (keyboardFrame_) {
            keyboardFrame_->unregisterKeyboardHook(this);
            keyboardFrame_ = nullptr;
        }
        detach(statusLabel_);
        detach(documentTitleLabel_);
        detach(runtimeLogLabel_);
        detach(editorModeLabel_);
        detach(helpLabel_);
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

        if (control->getTag() == kEditorHelpTag) {
            try {
                if (!openPortablePath(portablePaths().help() / "editor.html")) {
                    showNotice("Cannot open Help/editor.html; keep the full portable bundle.");
                }
            } catch (const std::exception& e) { showNotice(e.what()); }
        } else if (control->getTag() == kCodeViewTag) {
            switchEditor(EditorMode::code);
        } else if (control->getTag() == kGeneratorViewTag) {
            switchEditor(EditorMode::generator);
        } else if (control->getTag() == kPreviewViewTag) {
            switchEditor(EditorMode::preview);
        } else if (control->getTag() == kGenerateTag) {
            generateOrCancel(control);
        } else if (control->getTag() == kAppendGeneratedTag) {
            showNotice(owner_->applyGeneratorPreview(false)
                ? "Generated source appended. Press Run when ready."
                : "Generate a non-empty preview first; combined code must fit 32000 bytes.");
            refreshGeneratorUi();
        } else if (control->getTag() == kReplaceGeneratedTag) {
            showReplaceMenu(control);
        } else if (control->getTag() == kUndoGeneratedTag) {
            showNotice(owner_->undoGeneratorApply()
                ? "Generator change undone (press again to redo)."
                : "No unchanged generator edit to undo; later edits are protected.");
            refreshGeneratorUi();
        } else if (control->getTag() == kOpenCodeTag) {
            openCodeFile(control);
        } else if (control->getTag() == kSaveCodeTag) {
            saveCodeFile(control);
        } else if (control->getTag() == kCompleteTag) {
            showInsertMenu(control, kCompletionChoices,
                           "Inserted completion");
        } else if (control->getTag() == kSnippetsTag) {
            showSnipsMenu(control);
        } else if (control->getTag() == kRunSelectionTag) {
            if (owner_->editorMode() != EditorMode::code) {
                showNotice("Switch to Code before running a selection.");
                return;
            }
            const bool sent = owner_->sendSelectedCode();
            showStatus(sent ? "Selected code sent to Sonic Pi"
                            : "Select code in the editor first");
        } else if (control->getTag() == kLiveLoopsTag) {
            showLiveLoopMenu(control);
        } else if (control->getTag() == kGoToErrorTag) {
            showNotice(owner_->goToRuntimeError()
                           ? "Moved to the Sonic Pi error line"
                           : "No mapped error line to navigate to");
        } else if (control->getTag() == kCodeHelpTag) {
            showCodeHelpMenu(control);
        } else if (control->getTag() == kRunCodeTag) {
            if (owner_->editorMode() != EditorMode::code) {
                showNotice("Switch to Code before running Sonic Pi.");
                return;
            }
            const bool sent = owner_->sendCurrentCode();
            showStatus(sent ? "Run request submitted..."
                            : "Run request failed; checking runtime status...");
        } else if (control->getTag() == kStopAllTag) {
            const bool sent = owner_->sendStopAll();
            showStatus(sent ? "Stop requested..."
                            : "Stop request failed; checking runtime status...");
        }
    }
    void controlBeginEdit(VSTGUI::CControl*) override {}
    void controlEndEdit(VSTGUI::CControl*) override {}

    VSTGUI::CView* verifyView(
        VSTGUI::CView* view,
        const VSTGUI::UIAttributes&,
        const VSTGUI::IUIDescription*) override {
        if (auto* label = dynamic_cast<VSTGUI::CTextLabel*>(view)) {
            if (label->getTag() == kEditorModeTag) {
                editorModeLabel_ = label;
                label->registerViewListener(this);
            } else if (label->getTag() == kStatusTag) {
                statusLabel_ = label;
                statusLabel_->registerViewListener(this);
                statusLabel_->setText("Waiting for VST activation...");
            } else if (label->getTag() == kDocumentTitleTag) {
                documentTitleLabel_ = label;
                documentTitleLabel_->registerViewListener(this);
                refreshDocumentTitle();
            } else if (label->getTag() == kRuntimeLogTag) {
                runtimeLogLabel_ = label;
                runtimeLogLabel_->registerViewListener(this);
                runtimeLogLabel_->setText(
                    "Waiting for Sonic Pi runtime output...");
            } else if (label->getTag() == kCodeHelpLabelTag) {
                helpLabel_ = label;
                helpLabel_->registerViewListener(this);
                helpLabel_->setText(
                    "Select a Sonic Pi name and choose Docs for its help.");
            }
        }
        return view;
    }

    void viewWillDelete(VSTGUI::CView* view) override {
        if (view == editorModeLabel_) {
            editorModeLabel_->unregisterViewListener(this);
            editorModeLabel_ = nullptr;
        }
        if (view == statusLabel_) {
            statusTimer_ = nullptr;
            statusLabel_->unregisterViewListener(this);
            statusLabel_ = nullptr;
        }
        if (view == documentTitleLabel_) {
            documentTitleLabel_->unregisterViewListener(this);
            documentTitleLabel_ = nullptr;
        }
        if (view == runtimeLogLabel_) {
            runtimeLogLabel_->unregisterViewListener(this);
            runtimeLogLabel_ = nullptr;
        }
        if (view == helpLabel_) {
            helpLabel_->unregisterViewListener(this);
            helpLabel_ = nullptr;
        }
        if (view == codeEditorView_) {
            if (keyboardFrame_) {
                keyboardFrame_->unregisterKeyboardHook(this);
                keyboardFrame_ = nullptr;
            }
            codeEditorView_->unregisterViewListener(this);
            codeEditorView_ = nullptr;
        }
    }

    void viewAttached(VSTGUI::CView* view) override {
        if (view == statusLabel_) {
            refreshStatus();
            statusTimer_ = VSTGUI::makeOwned<VSTGUI::CVSTGUITimer>(
                [this](VSTGUI::CVSTGUITimer*) { refreshStatus(); },
                500, true);
        }
        if (view == codeEditorView_) {
            auto* frame = view->getFrame();
            if (frame && frame != keyboardFrame_) {
                if (keyboardFrame_) {
                    keyboardFrame_->unregisterKeyboardHook(this);
                }
                keyboardFrame_ = frame;
                keyboardFrame_->registerKeyboardHook(this);
            }
            // The starter text is set during construction, before the host has
            // attached this view. Some hosts miss that first invalidation.
            view->invalid();
        }
    }

    void onKeyboardEvent(VSTGUI::KeyboardEvent& event,
                        VSTGUI::CFrame* frame) override {
        if (event.consumed || event.type != VSTGUI::EventType::KeyDown ||
            !frame || frame != keyboardFrame_ ||
            frame->getFocusView() != codeEditorView_ ||
            !event.modifiers.has(VSTGUI::ModifierKey::Control)) {
            return;
        }

        if (event.virt == VSTGUI::VirtualKey::Space &&
            event.modifiers.is({VSTGUI::ModifierKey::Control,
                                VSTGUI::ModifierKey::Shift})) {
            event.consumed = true;
            showCodeHelpMenu(codeEditorView_);
            return;
        }

        if (event.virt == VSTGUI::VirtualKey::Space &&
            event.modifiers.is(VSTGUI::ModifierKey::Control)) {
            event.consumed = true;
            showInsertMenu(codeEditorView_, kCompletionChoices,
                           "Inserted completion");
            return;
        }

        const bool enterKey = event.virt == VSTGUI::VirtualKey::Return ||
                              event.virt == VSTGUI::VirtualKey::Enter;
        if (!enterKey) return;

        const bool selectionShortcut = event.modifiers.is(
            {VSTGUI::ModifierKey::Control, VSTGUI::ModifierKey::Alt});
        const bool stopShortcut = event.modifiers.is(
            {VSTGUI::ModifierKey::Control, VSTGUI::ModifierKey::Shift});
        const bool runShortcut =
            event.modifiers.is(VSTGUI::ModifierKey::Control);
        if (!selectionShortcut && !runShortcut && !stopShortcut) return;

        event.consumed = true;
        if (!stopShortcut && owner_->editorMode() != EditorMode::code) {
            if (runShortcut && owner_->editorMode() == EditorMode::generator) {
                generateOrCancel(codeEditorView_);
            } else {
                showNotice("Switch to Code before running Sonic Pi.");
            }
            return;
        }
        if (selectionShortcut) {
            const bool sent = owner_->sendSelectedCode();
            showStatus(sent ? "Selected code sent to Sonic Pi"
                            : "Select code in the editor first");
        } else if (stopShortcut) {
            const bool sent = owner_->sendStopAll();
            showStatus(sent ? "Stop requested..."
                            : "Stop request failed; checking runtime status...");
        } else {
            const bool sent = owner_->sendCurrentCode();
            showStatus(sent ? "Run request submitted..."
                            : "Run request failed; checking runtime status...");
        }
    }

private:
    void showSnipsMenu(VSTGUI::CView* anchor) {
        if (!anchor || !anchor->getFrame()) return;
        try {
            const auto paths = portablePaths();
            std::string error;
            if (!ensurePortableDirectories(paths, error)) {
                showNotice(error);
                return;
            }
            const auto files = listSnips(paths);
            auto menu = VSTGUI::makeOwned<VSTGUI::COptionMenu>();
            menu->setStyle(VSTGUI::COptionMenu::kPopupStyle);
            menu->addEntry("Open portable Snips folder");
            menu->addEntry("Undo last snip append");
            menu->addEntry("Open bundled snippet check report");
            for (const auto& file : files) {
                auto title = "Append: " + pathUtf8(file.filename());
                std::replace(title.begin(), title.end(), '\n', ' ');
                std::replace(title.begin(), title.end(), '\r', ' ');
                menu->addEntry(title.c_str());
            }
            if (files.empty()) showNotice("No snips: add .rb or .txt files to the Snips folder.");
            VSTGUI::CPoint location(0, anchor->getViewSize().getHeight());
            anchor->localToFrame(location);
            menu->popup(anchor->getFrame(), location,
                [this, paths, files](VSTGUI::COptionMenu* selected) {
                    if (!selected) return;
                    const auto index = selected->getLastResult();
                    if (index == 0) {
                        if (!openPortablePath(paths.snips())) {
                            showNotice("Could not open the Snips folder.");
                        }
                    } else if (index == 1) {
                        showNotice(owner_->undoSnippetAppend()
                            ? "Last snip append undone."
                            : "No unchanged snip append to undo; later edits are protected.");
                    } else if (index == 2) {
                        if (!openPortablePath(paths.help() / "snippet-checks.html")) {
                            showNotice("Snippet check report is missing from this package.");
                        }
                    } else if (index >= 3 &&
                               static_cast<std::size_t>(index - 3) < files.size()) {
                        if (owner_->editorMode() != EditorMode::code) {
                            showNotice("Switch to Code before appending a Sonic Pi snip.");
                            return;
                        }
                        std::string source;
                        if (!readCodeFile(pathUtf8(files[index - 3]), source)) {
                            showNotice("Could not read that snip as source text.");
                        } else {
                            showNotice(owner_->appendSnippet(source)
                                ? "Snip appended; existing code kept. Press Run when ready."
                                : "Snip is empty or combined code exceeds 32000 bytes.");
                        }
                    }
                });
        } catch (const std::exception& e) { showNotice(e.what()); }
    }

    void switchEditor(EditorMode mode) {
        if (!owner_->setEditorMode(mode)) {
            showNotice("Could not switch editor buffers");
            return;
        }
        refreshGeneratorUi();
        refreshDocumentTitle();
        refreshRuntimeLog();
    }

    void refreshGeneratorUi() {
        const auto mode = owner_->editorMode();
        if (editorModeLabel_) {
            const char* text = owner_->generatorRunning() ? "Working" :
                mode == EditorMode::code ? "CODE" :
                mode == EditorMode::generator ? "SCRIPT" :
                owner_->hasGeneratorPreview() ? "OUTPUT" : "PREVIEW";
            editorModeLabel_->setText(text);
        }
        if (lastMode_ != mode) {
            lastMode_ = mode;
            if (helpLabel_) helpLabel_->setText(
                mode == EditorMode::code
                    ? "Select a Sonic Pi name and choose Docs for its help."
                    : mode == EditorMode::generator
                    ? "Trusted Ruby only: full file access, NOT a sandbox.\nFinal expression: String. Ctrl+Enter generates; Gen/Cancel stops."
                    : "Read-only output. Append/Replace edits Code only; Run is separate.\nUndo gen is available until you make later code edits.");
        }
    }

    void generateOrCancel(VSTGUI::CView* anchor) {
        // Consume pending project restoration before consulting session consent.
        refreshDocumentTitle();
        if (owner_->generatorRunning()) {
            owner_->cancelGenerator();
            showNotice("Cancelling Ruby generator...");
            return;
        }
        if (!anchor || !anchor->getFrame()) return;
        const auto script = owner_->generatorSource();
        if (trustedScript_ && script == *trustedScript_) {
            showNotice(owner_->startGenerator()
                ? "Generating; Gen/Cancel stops the worker."
                : "Could not start the generator worker");
            return;
        }
        if (helpLabel_) helpLabel_->setText(
            "WARNING: Ruby can read/write files and run commands.\nNot a sandbox. Run only scripts you trust.");
        auto menu = VSTGUI::makeOwned<VSTGUI::COptionMenu>();
        menu->setStyle(VSTGUI::COptionMenu::kPopupStyle);
        menu->addEntry("Run this trusted Ruby script (full file access)");
        menu->addEntry("Cancel");
        VSTGUI::CPoint location(0, anchor->getViewSize().getHeight());
        anchor->localToFrame(location);
        menu->popup(anchor->getFrame(), location,
            [this, script](VSTGUI::COptionMenu* selected) {
                if (!selected || selected->getLastResult() != 0) return;
                if (script != owner_->generatorSource()) {
                    showNotice("Script changed; review it before generating.");
                    return;
                }
                trustedScript_ = script;
                showNotice(owner_->startGenerator()
                    ? "Generating; Gen/Cancel stops the worker."
                    : "Could not start the generator worker");
            });
    }

    void showReplaceMenu(VSTGUI::CView* anchor) {
        if (!owner_->hasGeneratorPreview() || owner_->generatorRunning()) {
            showNotice("Generate a successful non-empty preview first.");
            return;
        }
        if (!anchor || !anchor->getFrame()) return;
        auto menu = VSTGUI::makeOwned<VSTGUI::COptionMenu>();
        menu->setStyle(VSTGUI::COptionMenu::kPopupStyle);
        menu->addEntry("Replace Sonic Pi code with this preview");
        menu->addEntry("Cancel");
        VSTGUI::CPoint location(0, anchor->getViewSize().getHeight());
        anchor->localToFrame(location);
        menu->popup(anchor->getFrame(), location,
            [this](VSTGUI::COptionMenu* selected) {
                if (!selected || selected->getLastResult() != 0) return;
                showNotice(owner_->applyGeneratorPreview(true)
                    ? "Code replaced. Undo gen restores it; Run is separate."
                    : "Preview is empty/stale or exceeds the source limit.");
                refreshGeneratorUi();
            });
    }

    void showCodeHelpMenu(VSTGUI::CView* anchor) {
        if (!anchor || !anchor->getFrame()) return;

        const auto selected = owner_->selectedText();
        if (const auto* topic = findCodeHelpTopic(selected)) {
            displayCodeHelp(*topic);
            return;
        }

        auto menu = VSTGUI::makeOwned<VSTGUI::COptionMenu>();
        menu->setStyle(VSTGUI::COptionMenu::kPopupStyle);
        for (const auto& topic : kCodeHelpTopics) {
            const std::string label(topic.menuLabel);
            menu->addEntry(label.c_str());
        }

        VSTGUI::CPoint location(0, anchor->getViewSize().getHeight());
        anchor->localToFrame(location);
        menu->popup(anchor->getFrame(), location,
                    [this](VSTGUI::COptionMenu* selectedTopic) {
                        if (!selectedTopic) return;
                        const auto index = selectedTopic->getLastResult();
                        if (index < 0 ||
                            static_cast<std::size_t>(index) >=
                                kCodeHelpTopics.size()) {
                            return;
                        }
                        displayCodeHelp(
                            kCodeHelpTopics[static_cast<std::size_t>(index)]);
                    });
    }

    void displayCodeHelp(const CodeHelpTopic& topic) {
        if (!helpLabel_) return;
        std::string text(topic.menuLabel);
        text += "\n";
        constexpr std::size_t kMaxHelpSummaryLength = 96;
        text += topic.summary.substr(0, kMaxHelpSummaryLength);
        if (topic.summary.size() > kMaxHelpSummaryLength) text += "...";
        text += "\nExample: ";
        text += topic.example;
        helpLabel_->setText(text.c_str());
    }

    template <std::size_t N>
    void showInsertMenu(VSTGUI::CView* anchor,
                        const EditorChoice (&choices)[N],
                        const char* insertedNotice) {
        if (!anchor || !anchor->getFrame()) return;

        auto menu = VSTGUI::makeOwned<VSTGUI::COptionMenu>();
        menu->setStyle(VSTGUI::COptionMenu::kPopupStyle);
        for (const auto& choice : choices) {
            const std::string label(choice.label);
            menu->addEntry(label.c_str());
        }

        VSTGUI::CPoint location(0, anchor->getViewSize().getHeight());
        anchor->localToFrame(location);
        std::array<std::string_view, N> insertions{};
        for (std::size_t i = 0; i < N; ++i) {
            insertions[i] = choices[i].insertion;
        }
        menu->popup(anchor->getFrame(), location,
                    [this, insertions,
                     insertedNotice = std::string(insertedNotice)](
                        VSTGUI::COptionMenu* selected) {
                        if (!selected) return;
                        const auto index = selected->getLastResult();
                        if (index < 0 ||
                            static_cast<std::size_t>(index) >= N) {
                            return;
                        }
                        if (owner_->insertTextAtCursor(
                                insertions[static_cast<std::size_t>(index)])) {
                            showNotice(insertedNotice);
                        } else {
                            showNotice("Could not insert text at the cursor");
                        }
                    });
    }

    void showLiveLoopMenu(VSTGUI::CView* anchor) {
        if (!anchor || !anchor->getFrame()) return;
        auto loops = findLiveLoopBlocks(owner_->currentCode());
        if (loops.empty()) {
            showNotice("No complete live_loop blocks found");
            return;
        }

        auto menu = VSTGUI::makeOwned<VSTGUI::COptionMenu>();
        menu->setStyle(VSTGUI::COptionMenu::kPopupStyle);
        for (const auto& loop : loops) {
            menu->addEntry(loop.label.c_str());
        }

        VSTGUI::CPoint location(0, anchor->getViewSize().getHeight());
        anchor->localToFrame(location);
        menu->popup(anchor->getFrame(), location,
                    [this, loops = std::move(loops)](
                        VSTGUI::COptionMenu* selected) {
                        if (!selected) return;
                        const auto index = selected->getLastResult();
                        if (index < 0 ||
                            static_cast<std::size_t>(index) >= loops.size()) {
                            return;
                        }
                        if (owner_->sendRunCode(
                                loops[static_cast<std::size_t>(index)].code)) {
                            showNotice("Live loop sent to Sonic Pi");
                        } else {
                            showNotice("Could not send live_loop code");
                        }
                    });
    }

    void refreshStatus() {
        if (!statusLabel_) return;
        if (const auto result = owner_->pollGenerator()) {
            showNotice(result->success
                ? (result->output.empty() ? "Empty output: no code changed."
                                         : "Generated preview ready; code has not changed.")
                : result->cancelled ? "Generation cancelled; code unchanged."
                                    : "Generator failed; Preview shows the error.");
        }
        refreshGeneratorUi();
        owner_->requestRuntimeStatus();
        owner_->requestRuntimeLog();
        refreshDocumentTitle();
        refreshRuntimeLog();
        if (std::chrono::steady_clock::now() < noticeUntil_) {
            showStatus(noticeText_.c_str());
            return;
        }
        if (!owner_->bufferWarning().empty()) {
            showStatus(owner_->bufferWarning().c_str());
            return;
        }
        if (owner_->editorMode() != EditorMode::code) {
            showStatus(owner_->generatorRunning()
                ? "Generating in Ruby; Gen/Cancel stops it (5 second limit)."
                : owner_->generatorMessage().substr(0, 92).c_str());
            return;
        }
        const auto diagnostic = owner_->runtimeDiagnostic();
        if (!diagnostic.message.empty()) {
            std::string text = "Sonic Pi error";
            if (diagnostic.lineMapped) {
                text += " on line " + std::to_string(diagnostic.editorLine);
            } else if (diagnostic.submittedLine > 0) {
                text += " in submitted code, line " +
                        std::to_string(diagnostic.submittedLine);
            }
            text += ": ";
            constexpr std::size_t kMaxStatusMessage = 72;
            text += diagnostic.message.substr(0, kMaxStatusMessage);
            if (diagnostic.message.size() > kMaxStatusMessage) text += "...";
            showStatus(text.c_str());
            return;
        }
        showStatus(statusText(owner_->runtimeStatus()));
    }

    void refreshRuntimeLog() {
        if (owner_->editorMode() != EditorMode::code) {
            if (runtimeLogLabel_) {
                runtimeLogLabel_->setText(owner_->generatorMessage().c_str());
            }
            runtimeLogText_.clear();
            return;
        }
        const auto diagnostic = owner_->runtimeDiagnostic();
        std::string diagnosticKey = std::to_string(diagnostic.editorLine);
        diagnosticKey += "/";
        diagnosticKey += std::to_string(diagnostic.submittedLine);
        diagnosticKey += "/";
        diagnosticKey += diagnostic.message;
        if (diagnosticKey != runtimeDiagnosticKey_) {
            runtimeDiagnosticKey_ = std::move(diagnosticKey);
            if (codeEditorView_) codeEditorView_->invalid();
        }

        if (!runtimeLogLabel_) return;
        auto text = formatRuntimeLog(owner_->runtimeLog());
        if (text == runtimeLogText_) return;
        runtimeLogText_ = std::move(text);
        runtimeLogLabel_->setText(runtimeLogText_.c_str());
        if (codeEditorView_) codeEditorView_->invalid();
    }

    void refreshDocumentTitle() {
        if (!documentTitleLabel_) return;
        EditorState restored;
        if (owner_->takeRestoredState(restored)) {
            savedCode_ = std::move(restored.code);
            savedGenerator_ = std::move(restored.generator);
            // A DAW project is portable; never silently overwrite an external
            // file at an absolute path saved on another computer.
            currentFilePath_.clear();
            generatorFilePath_.clear();
            trustedScript_.reset();
        }
        const bool script = owner_->editorMode() == EditorMode::generator;
        const bool preview = owner_->editorMode() == EditorMode::preview;
        const auto code = owner_->currentEditorText();
        const bool dirty = !preview && code != (script ? savedGenerator_ : savedCode_);
        std::string title = "Dexter U.S. Sonic Pi v";
        title += kPluginVersion;
        title += preview ? " - Preview (read-only)" :
                 script ? " - Generator: " : " - Code: ";
        if (dirty) title += "*";
        const auto& path = script ? generatorFilePath_ : currentFilePath_;
        if (!preview) title += path.empty()
                     ? "Untitled"
                     : fileNameFromUtf8Path(path);
        documentTitleLabel_->setText(title.c_str());
    }

    void showNotice(std::string text) {
        noticeText_ = std::move(text);
        noticeUntil_ = std::chrono::steady_clock::now() +
                       std::chrono::seconds(4);
        showStatus(noticeText_.c_str());
    }

    bool hasUnsavedChanges() const {
        return owner_->currentEditorText() !=
            (owner_->editorMode() == EditorMode::generator ? savedGenerator_ : savedCode_);
    }

    bool saveCodeFile(VSTGUI::CControl* control) {
        refreshDocumentTitle();
        if (owner_->editorMode() == EditorMode::preview) {
            showNotice("Append or Replace first, then save from Code.");
            return false;
        }
        const bool script = owner_->editorMode() == EditorMode::generator;
        const auto code = owner_->currentEditorText();
        auto& activePath = script ? generatorFilePath_ : currentFilePath_;
        auto& saved = script ? savedGenerator_ : savedCode_;
        std::string path = activePath;
        PortablePaths paths;
        std::string fileError;
        try { paths = portablePaths(); }
        catch (const std::exception& e) { showNotice(e.what()); return false; }
        if (!ensurePortableDirectories(paths, fileError)) {
            showNotice(fileError); return false;
        }
        const auto directory = script ? paths.generators() : paths.code();
        if (path.empty()) path = chooseCodeFile(control, true, directory, script);
        if (path.empty()) return false;
        if (!savePortableSource(pathFromUtf8(path), directory, code, fileError)) {
            showNotice("Save failed: " + fileError);
            return false;
        }

        activePath = std::move(path);
        saved = code;
        refreshDocumentTitle();
        showNotice("Saved " + fileNameFromUtf8Path(activePath));
        return true;
    }

    void openCodeFile(VSTGUI::CControl* control) {
        refreshDocumentTitle();
        if (owner_->editorMode() == EditorMode::preview) {
            showNotice("Switch to Code or Script before opening a file.");
            return;
        }
        const bool script = owner_->editorMode() == EditorMode::generator;
        PortablePaths paths;
        std::string fileError;
        try { paths = portablePaths(); }
        catch (const std::exception& e) { showNotice(e.what()); return; }
        if (!ensurePortableDirectories(paths, fileError)) {
            showNotice(fileError); return;
        }
        const auto directory = script ? paths.generators() : paths.code();
        const auto path = chooseCodeFile(control, false, directory, script);
        if (path.empty()) return;
        if (!pathIsInside(pathFromUtf8(path), directory)) {
            showNotice(script ? "Copy that file into Files/Generators first."
                              : "Copy that file into Files/Code first.");
            return;
        }

        if (hasUnsavedChanges()) {
#if defined(_WIN32)
            const auto choice = ::MessageBoxW(
                nullptr,
                L"This code has unsaved changes. Save before opening another file?",
                L"Dexter U.S. Sonic Pi",
                MB_YESNOCANCEL | MB_ICONWARNING | MB_SETFOREGROUND);
            if (choice == IDYES) {
                if (!saveCodeFile(control)) return;
            } else if (choice != IDNO) {
                return;
            }
#else
            showNotice("Save current code before opening another file");
            return;
#endif
        }

        std::string code;
        if (!readCodeFile(path, code)) {
            showNotice("Could not read code file");
            return;
        }
        if (!owner_->replaceActiveText(code)) {
            showNotice("Could not load code into editor");
            return;
        }

        (script ? generatorFilePath_ : currentFilePath_) = path;
        (script ? savedGenerator_ : savedCode_) = std::move(code);
        refreshDocumentTitle();
        showNotice("Loaded " + fileNameFromUtf8Path(path));
    }

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
    VSTGUI::CTextLabel* documentTitleLabel_ = nullptr;
    VSTGUI::CTextLabel* runtimeLogLabel_ = nullptr;
    VSTGUI::CTextLabel* helpLabel_ = nullptr;
    VSTGUI::CTextLabel* editorModeLabel_ = nullptr;
    VSTGUI::CView* codeEditorView_ = nullptr;
    VSTGUI::CFrame* keyboardFrame_ = nullptr;
    VSTGUI::SharedPointer<VSTGUI::CVSTGUITimer> statusTimer_;
    std::string currentFilePath_;
    std::string savedCode_ = kStarterCode;
    std::string generatorFilePath_;
    std::string savedGenerator_ = kStarterGenerator;
    std::optional<std::string> trustedScript_;
    std::optional<EditorMode> lastMode_;
    std::string runtimeLogText_;
    std::string runtimeDiagnosticKey_;
    std::string noticeText_;
    std::chrono::steady_clock::time_point noticeUntil_{};
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
            VSTGUI::CRect(0, 0, 512, 160), this);
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
    if (editorMode_ != EditorMode::code ||
        code.empty() || code.size() > kMaxGeneratorOutputBytes) return false;

    const auto source = currentCode();
    std::size_t lineOffset = 0;
    std::size_t sourceLineCount = 0;
    const bool lineMapped = findUniqueSourceLineOffset(
        source, code, lineOffset, sourceLineCount);
    {
        std::lock_guard<std::mutex> lock(runtimeLogMutex_);
        runtimeDiagnostic_ = {};
        runSourceLineOffset_ = lineOffset;
        runSourceLineCount_ = sourceLineCount;
        runSourceMapped_ = lineMapped;
    }
    auto* editorView = dynamic_cast<VSTGUI::CView*>(
        const_cast<VSTGUI::ITextEditor*>(codeEditor_));
    if (editorView) editorView->invalid();

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

std::string Controller::selectedText() const {
    if (!codeEditor_) return {};
    auto* editorView = dynamic_cast<VSTGUI::CView*>(
        const_cast<VSTGUI::ITextEditor*>(codeEditor_));
    auto* frame = editorView ? editorView->getFrame() : nullptr;
    if (!frame) return {};
    const auto previousClipboard = frame->getClipboard();
    const bool copied =
        codeEditor_->handleCommand(VSTGUI::ITextEditor::Command::Copy);
    // Copy updates the clipboard, so capture the selected source first and
    // restore the user's previous clipboard contents immediately afterward.
    const auto copiedSelection = copied ? frame->getClipboard() : nullptr;
    std::string selected;
    if (copiedSelection) {
        for (std::uint32_t index = 0;
             index < copiedSelection->getCount(); ++index) {
            const void* bytes = nullptr;
            VSTGUI::IDataPackage::Type type = VSTGUI::IDataPackage::kError;
            const auto size =
                copiedSelection->getData(index, bytes, type);
            if (type == VSTGUI::IDataPackage::kText && bytes && size > 0) {
                selected.assign(static_cast<const char*>(bytes), size);
                break;
            }
        }
    }
    frame->setClipboard(previousClipboard);
    return selected;
}

bool Controller::sendSelectedCode() {
    return sendRunCode(selectedText());
}

bool Controller::goToRuntimeError() {
    if (editorMode_ != EditorMode::code) return false;
    const auto diagnostic = runtimeDiagnostic();
    if (!codeEditor_ || !diagnostic.lineMapped ||
        diagnostic.editorLine <= 0) {
        return false;
    }

    const auto source = currentCode();
    const auto line = sourceLineAt(
        source, static_cast<std::size_t>(diagnostic.editorLine));
    if (line.empty()) return false;

    codeEditor_->handleCommand(VSTGUI::ITextEditor::Command::TakeFocus);
    codeEditor_->setFindString(line);
    return codeEditor_->handleCommand(
        VSTGUI::ITextEditor::Command::FindNext);
}

std::string Controller::currentCode() const {
    return codeState_;
}

bool Controller::replaceCurrentCode(const std::string& code) {
    if (!setEditorMode(EditorMode::code)) return false;
    return replaceActiveText(code);
}

bool Controller::insertTextAtCursor(std::string_view text) {
    if (editorMode_ == EditorMode::preview || !codeEditor_ || text.empty() ||
        text.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    auto* editorView = dynamic_cast<VSTGUI::CView*>(
        const_cast<VSTGUI::ITextEditor*>(codeEditor_));
    auto* frame = editorView ? editorView->getFrame() : nullptr;
    if (!frame) return false;

    const auto previousClipboard = frame->getClipboard();
    auto insertData = VSTGUI::CDropSource::create(
        text.data(), static_cast<std::uint32_t>(text.size()),
        VSTGUI::IDataPackage::kText);
    if (!insertData) return false;

    frame->setClipboard(insertData);
    const bool inserted =
        codeEditor_->handleCommand(VSTGUI::ITextEditor::Command::Paste);
    frame->setClipboard(previousClipboard);
    return inserted;
}

bool Controller::sendStopAll() {
    auto message = Steinberg::owned(allocateMessage());
    if (!message) return false;
    message->setMessageID("SonicPi.StopAll");
    return sendMessage(message) == Steinberg::kResultOk;
}

bool Controller::requestRuntimeStatus() {
    auto message = Steinberg::owned(allocateMessage());
    if (!message) return false;
    message->setMessageID("SonicPi.GetStatus");
    return sendMessage(message) == Steinberg::kResultOk;
}

bool Controller::requestRuntimeLog() {
    auto message = Steinberg::owned(allocateMessage());
    if (!message) return false;
    message->setMessageID("SonicPi.GetLog");
    return sendMessage(message) == Steinberg::kResultOk;
}

RuntimeStatus Controller::runtimeStatus() const noexcept {
    return runtimeStatus_.load();
}

std::string Controller::runtimeLog() const {
    std::lock_guard<std::mutex> lock(runtimeLogMutex_);
    return runtimeLog_;
}

RuntimeDiagnostic Controller::runtimeDiagnostic() const {
    std::lock_guard<std::mutex> lock(runtimeLogMutex_);
    return runtimeDiagnostic_;
}

Steinberg::tresult PLUGIN_API Controller::notify(
    Steinberg::Vst::IMessage* message) {
    if (!message) return Steinberg::kResultFalse;
    const auto* messageId = message->getMessageID();
    if (messageId && std::strcmp(messageId, "SonicPi.RuntimeStatus") == 0) {
        auto* attributes = message->getAttributes();
        Steinberg::int64 state = static_cast<Steinberg::int64>(
            RuntimeStatus::inactive);
        if (attributes &&
            attributes->getInt("state", state) == Steinberg::kResultOk &&
            state >= static_cast<Steinberg::int64>(RuntimeStatus::inactive) &&
            state <= static_cast<Steinberg::int64>(RuntimeStatus::error)) {
            runtimeStatus_.store(static_cast<RuntimeStatus>(state));
        }
        return Steinberg::kResultOk;
    }
    if (messageId && std::strcmp(messageId, "SonicPi.RuntimeLog") == 0) {
        auto* attributes = message->getAttributes();
        const void* bytes = nullptr;
        Steinberg::uint32 size = 0;
        if (attributes &&
            attributes->getBinary("log", bytes, size) == Steinberg::kResultOk &&
            size <= 4096 && (size == 0 || bytes)) {
            std::lock_guard<std::mutex> lock(runtimeLogMutex_);
            std::string nextLog(
                size == 0 ? "" : static_cast<const char*>(bytes), size);
            const auto appended = appendedLogText(runtimeLog_, nextLog);
            runtimeLog_ = std::move(nextLog);
            for (const auto& event : parseRuntimeErrorEvents(appended)) {
                RuntimeDiagnostic diagnostic;
                diagnostic.submittedLine = event.line;
                diagnostic.message = event.message;
                if (runSourceMapped_ &&
                    runSourceLineOffset_ < runSourceLineCount_ &&
                    static_cast<std::size_t>(event.line) <=
                        runSourceLineCount_ - runSourceLineOffset_) {
                    diagnostic.editorLine = static_cast<int>(
                        runSourceLineOffset_ +
                        static_cast<std::size_t>(event.line));
                    diagnostic.lineMapped = true;
                }
                runtimeDiagnostic_ = std::move(diagnostic);
            }
        }
        return Steinberg::kResultOk;
    }
    return EditController::notify(message);
}

void Controller::onTextEditorCreated(const VSTGUI::ITextEditor& editor) {
    codeEditor_ = &editor;
    setEditorMode(editorMode_);
    syncCodeState();
}

void Controller::onTextEditorDestroyed(const VSTGUI::ITextEditor& editor) {
    if (codeEditor_ == &editor) {
        generator_.cancel();
        codeEditor_ = nullptr;
    }
}

void Controller::onTextEditorTextChanged(const VSTGUI::ITextEditor& editor) {
    if (restoringCode_ || editorMode_ == EditorMode::preview) return;
    const auto text = editor.getPlainText();
    const auto limit = editorMode_ == EditorMode::generator
        ? kMaxGeneratorSourceBytes : kMaxCodeStateBytes;
    if (text.size() > limit) {
        bufferWarning_ = editorMode_ == EditorMode::generator
            ? "Edit rejected: generator scripts must fit in 64 KiB."
            : "Edit rejected: source buffers must fit in 1 MiB.";
        restoringCode_ = true;
        editor.setPlainText(currentEditorText(), true);
        restoringCode_ = false;
        return;
    }
    bufferWarning_.clear();
    if (editorMode_ == EditorMode::generator) {
        generatorSource_ = text;
        ++generatorRevision_;
        generatorPreviewValid_ = false;
        generatorMessage_ = "Script changed. Generate again before applying output.";
    } else {
        codeState_ = text;
    }
    setDirty(true);
    syncCodeState();
    if (editorMode_ == EditorMode::code) {
        std::lock_guard<std::mutex> lock(runtimeLogMutex_);
        runtimeDiagnostic_ = {};
        runSourceMapped_ = false;
    }
    auto* editorView = dynamic_cast<VSTGUI::CView*>(
        const_cast<VSTGUI::ITextEditor*>(codeEditor_));
    if (editorView) editorView->invalid();
}

Steinberg::tresult PLUGIN_API Controller::queryInterface(
    const char* iid, void** obj) {
    return EditController::queryInterface(iid, obj);
}

void Controller::beginDraw(
    const VSTGUI::TextEditorColorization::IEditorExt& editor) {
    syntaxStyles_.clear();
    errorStyleStart_ = 0;
    errorStyleEnd_ = 0;
    const auto textLength = editor.getTextLength();
    if (textLength == 0) return;

    const auto diagnostic = editorMode_ == EditorMode::code
        ? runtimeDiagnostic() : RuntimeDiagnostic{};
    editor.readText(0, textLength, [this, diagnostic](std::u32string_view text) {
        syntaxStyles_ = tokenizeSonicPi(text);
        if (!diagnostic.lineMapped || diagnostic.editorLine <= 0) return;

        std::size_t lineStart = 0;
        for (int line = 1; line < diagnostic.editorLine; ++line) {
            const auto newline = text.find(U'\n', lineStart);
            if (newline == std::u32string_view::npos) return;
            lineStart = newline + 1;
        }
        if (lineStart >= text.size()) return;
        const auto newline = text.find(U'\n', lineStart);
        errorStyleStart_ = lineStart;
        errorStyleEnd_ =
            newline == std::u32string_view::npos ? text.size() : newline;
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

    const auto errorStart = std::max(beginOffset, errorStyleStart_);
    const auto errorEnd = std::min(endOffset, errorStyleEnd_);
    if (errorStart >= errorEnd) return result;

    VSTGUI::TextEditorColorization::IStyleProvider::Styles marked;
    marked.reserve(result.size() + 2);
    for (const auto& style : result) {
        const auto styleEnd = style.start + style.length;
        if (styleEnd <= errorStart || style.start >= errorEnd) {
            marked.push_back(style);
            continue;
        }
        if (style.start < errorStart) {
            marked.push_back(
                Style{style.start, errorStart - style.start, style.color});
        }
        const auto markedStart = std::max(style.start, errorStart);
        const auto markedEnd = std::min(styleEnd, errorEnd);
        if (markedStart < markedEnd) {
            marked.push_back(
                Style{markedStart, markedEnd - markedStart, kErrorColor});
        }
        if (styleEnd > errorEnd) {
            marked.push_back(
                Style{errorEnd, styleEnd - errorEnd, style.color});
        }
    }
    return marked;
}

void Controller::endDraw(
    const VSTGUI::TextEditorColorization::IEditorExt&) {}

} // namespace SonicPiVst3
