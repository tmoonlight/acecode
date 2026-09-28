#pragma once

#include "ask_question_controller.hpp"

#include <string>
#include <vector>

namespace acecode::tui {

struct AskQuestionLayoutRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool contains(int px, int py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }
};

enum class AskQuestionLayoutKind {
    Header,
    Question,
    Option,
    Custom,
    Summary,
    // Stacked (narrow panel) summary answer line. In two-column mode the answer
    // shares the row with its question line instead.
    SummaryAnswer,
    Hint,
};

enum class AskQuestionHitKind {
    None,
    Option,
    Custom,
    SummaryQuestion,
    Scrollbar,
};

struct AskQuestionHit {
    AskQuestionHitKind kind = AskQuestionHitKind::None;
    int question_index = -1;
    int option_index = -1;
};

// One visual line of the question panel. Column text is kept in separate fields
// instead of one concatenated string so the renderer can place each column at an
// exact x offset; the `*_x` fields hold those offsets so a renderer never has to
// recompute column widths.
struct AskQuestionLayoutRow {
    AskQuestionLayoutKind kind = AskQuestionLayoutKind::Question;
    int question_index = -1;
    int option_index = -1;
    std::string number;
    std::string marker;
    std::string title;
    std::string description;
    std::string answer;
    bool focused = false;
    bool selected = false;
    bool recommended = false;
    // Custom rows only: this line shows the placeholder instead of real text.
    bool placeholder = false;
    // Column offsets, relative to the panel content area.
    int title_x = 0;
    int description_x = -1;
    int answer_x = -1;
    // Custom rows only: byte range of the editor text covered by this line.
    std::size_t text_byte_begin = 0;
    std::size_t text_byte_end = 0;
    // A soft-wrap boundary belongs to exactly one visual editor row.
    bool has_cursor = false;
    AskQuestionLayoutRect rect;
};

struct AskQuestionLayoutInput {
    const AskQuestionSnapshot* snapshot = nullptr;
    int viewport_width = 80;
    // Height available to the question rows, excluding the overlay border and
    // footer. Callers must measure this from the current frame; the layout does
    // not reserve a fixed number of terminal rows.
    int viewport_height = 20;
    int minimum_visible_rows = 4;
    int timeout_remaining_seconds = 0;
};

struct AskQuestionLayout {
    // Four-slot option grid: number | marker | title | description.
    int content_width = 0;
    int number_width = 0;
    int marker_width = 0;
    int title_width = 0;
    int description_width = 0;
    // ASCII spaces between adjacent option columns.
    int column_gap = 2;
    int title_x = 0;
    int description_x = 0;
    // Summary grid: number | 2 spaces | question | 2 spaces | answer.
    int summary_question_width = 0;
    int summary_answer_width = 0;
    int summary_answer_x = -1;
    // True when the panel is too narrow for two summary columns; answers move to
    // their own lines below the matching question line.
    bool summary_stacked = false;
    int total_rows = 0;
    int visible_rows = 0;
    int scroll_offset = 0;
    int scrollbar_x = -1;
    AskQuestionLayoutRect scrollbar_track;
    AskQuestionLayoutRect scrollbar_thumb;
    int focused_row_begin = -1;
    int focused_row_end = -1;
    bool terminal_too_narrow = false;
    std::vector<AskQuestionLayoutRow> rows;
};

int ask_question_display_width(const std::string& text);
int ask_question_content_width_for_frame(int terminal_width,
                                         int measured_main_column_width,
                                         bool regular_sidebar_visible,
                                         int regular_sidebar_width);
int ask_question_visible_rows_for_terminal(int terminal_rows,
                                           int minimum_visible_rows = 4);
// Width-aware wrap. Keeps ASCII words whole, never starts a line with closing
// punctuation, and always respects the width bound (over-long words are split
// as a last resort). Explicit '\n' starts a new line.
std::vector<std::string> ask_question_wrap(const std::string& text, int width);
std::size_t ask_question_text_byte_offset_for_x(const std::string& text,
                                                std::size_t byte_begin,
                                                std::size_t byte_end,
                                                int x);
AskQuestionLayout build_ask_question_layout(const AskQuestionLayoutInput& input);
int clamp_ask_question_layout_scroll(int offset, int total_rows, int visible_rows);
int ask_question_scroll_offset_for_y(int y, const AskQuestionLayout& layout);
int ask_question_scroll_offset_for_track_y(int y,
                                           int track_y,
                                           int track_height,
                                           int total_rows,
                                           int visible_rows);
int hit_test_ask_question_layout(const AskQuestionLayout& layout, int x, int y);
AskQuestionHit hit_test_ask_question_target(const AskQuestionLayout& layout,
                                            int x,
                                            int y);

} // namespace acecode::tui
