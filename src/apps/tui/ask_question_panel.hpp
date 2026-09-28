#pragma once

// FTXUI rendering for the AskUserQuestion panel and its shortcut help footer.
//
// The panel is a bottom-anchored overlay inside the chat viewport. Colors are
// injected instead of read from the global theme so tests can render the panel
// with known colors and assert on the resulting screen cells.
//
// Column placement comes entirely from AskQuestionLayout: this module never
// recomputes column widths, so what the layout tests assert is exactly what the
// renderer draws.

#include "ask_question_layout.hpp"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace acecode::tui {

struct AskQuestionPanelColors {
    ftxui::Color border;
    // Question page heading ("Question 2/4 [Decision]") and question text.
    ftxui::Color question;
    // Option titles, custom answer text and summary answers.
    ftxui::Color answer;
    // Option descriptions, numbers and markers.
    ftxui::Color description;
    // "Type your own answer here" and other inactive placeholders.
    ftxui::Color placeholder;
    // Focus row background. Focus never changes text color or weight.
    ftxui::Color focus_bg;
    // Panel background so chat content behind the overlay does not bleed through.
    ftxui::Color panel_bg;
    // Shortcut help function text and other secondary hints.
    ftxui::Color secondary;
    // Text selection inside the inline editor.
    ftxui::Color selection_fg;
    ftxui::Color selection_bg;
};

// One shortcut help entry rendered as `key: action`.
struct AskQuestionHelpEntry {
    std::string key;
    std::string action;
};

struct AskQuestionPanelInput {
    const AskQuestionLayout* layout = nullptr;
    const AskQuestionSnapshot* snapshot = nullptr;
    AskQuestionPanelColors colors;
    bool terminal_too_narrow = false;
    // Total terminal columns available to the help footer; used only by
    // build_ask_question_help_line.
    int help_width = 80;
    // Optional frame geometry outputs consumed by the mouse adapter.
    std::vector<ftxui::Box>* row_boxes = nullptr;
    ftxui::Box* scrollbar_box = nullptr;
    ftxui::Box* overlay_box = nullptr;
};

// Bottom-anchored panel: header, question, options, custom row or summary,
// scrollbar rail and border. The returned element's height is
// `layout.visible_rows + 2`, so callers must cap it to the chat viewport.
ftxui::Element build_ask_question_panel(const AskQuestionPanelInput& input);

// Layered shortcut footer: keys in the main text color, `: action` in the
// secondary color, wrapped to `width` columns.
//
// `minimum_rows` pads the footer to a fixed height. The footer shares the
// vertical stack with the chat viewport, so a footer that grows when the
// interaction state changes would also move the question panel; padding to a
// stable row count keeps entering and leaving the inline editor height-neutral.
ftxui::Element build_ask_question_help_line(
    const std::vector<AskQuestionHelpEntry>& help,
    const AskQuestionPanelColors& colors, int width, int minimum_rows = 1);

// Shortcut entries for the current interaction state. `scrollable` appends the
// scroll hint when the content exceeds the viewport.
std::vector<AskQuestionHelpEntry> ask_question_help_entries(
    const AskQuestionSnapshot& snapshot, bool scrollable);

} // namespace acecode::tui
