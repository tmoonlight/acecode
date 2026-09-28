#include "ask_question_layout.hpp"

#include "ask_question_text.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace acecode::tui {
namespace {

constexpr int kAskQuestionMinimumContentWidth = 8;
constexpr int kAskQuestionTerminalChromeWidth = 10;
constexpr int kAskQuestionMainColumnChromeWidth = 8;
constexpr int kOuterFrameHorizontalChromeWidth = 2;
constexpr int kSidebarSeparatorWidth = 1;

// Option grid layout:
//   number | 1 space | marker | 2 spaces | title | 2 spaces | description
constexpr int kMarkerWidth = 3;
constexpr int kNumberMarkerGap = 1;
constexpr int kColumnGap = 2;
constexpr int kMinTitleWidth = 8;
constexpr int kMaxTitleWidth = 36;
// Summary grid layout:
//   number | 2 spaces | question | 2 spaces | answer
constexpr int kSummaryNumberGap = 2;
constexpr int kMinSummaryAnswerWidth = 12;
constexpr int kMinSummaryQuestionWidth = 8;
constexpr const char* kCustomPlaceholder = "Type your own answer here";

bool continuation(unsigned char c) { return (c & 0xc0) == 0x80; }

std::size_t glyph_end(const std::string& text, std::size_t start) {
    if (start >= text.size()) return text.size();
    const auto c = static_cast<unsigned char>(text[start]);
    std::size_t length = c < 0x80 ? 1 : (c < 0xe0 ? 2 : (c < 0xf0 ? 3 : 4));
    if (start + length > text.size()) return start + 1;
    for (std::size_t i = start + 1; i < start + length; ++i) {
        if (!continuation(static_cast<unsigned char>(text[i]))) return start + 1;
    }
    return start + length;
}

int glyph_width(const std::string& text, std::size_t start, std::size_t end) {
    if (start >= text.size() || start >= end) return 0;
    return std::max(0, ask_question_codepoint_width(
        std::string_view(text).substr(start, end - start)));
}

std::uint32_t glyph_codepoint(const std::string& text, std::size_t start,
                              std::size_t end) {
    if (start >= end || end > text.size()) return 0;
    const auto c0 = static_cast<unsigned char>(text[start]);
    if (c0 < 0x80) return c0;
    const auto byte = [&text](std::size_t index) {
        return static_cast<std::uint32_t>(
            static_cast<unsigned char>(text[index]) & 0x3fu);
    };
    if ((c0 & 0xe0u) == 0xc0u && start + 1 < end) {
        return ((c0 & 0x1fu) << 6) | byte(start + 1);
    }
    if ((c0 & 0xf0u) == 0xe0u && start + 2 < end) {
        return ((c0 & 0x0fu) << 12) | (byte(start + 1) << 6) | byte(start + 2);
    }
    if ((c0 & 0xf8u) == 0xf0u && start + 3 < end) {
        return ((c0 & 0x07u) << 18) | (byte(start + 1) << 12) |
               (byte(start + 2) << 6) | byte(start + 3);
    }
    return 0;
}

bool is_space_glyph(const std::string& text, std::size_t start,
                    std::size_t end) {
    return end == start + 1 && text[start] == ' ';
}

// Closing punctuation must never start a wrapped line.
bool is_closing_glyph(const std::string& text, std::size_t start,
                      std::size_t end) {
    switch (glyph_codepoint(text, start, end)) {
        case 0xff0c:  // fullwidth comma
        case 0x3002:  // ideographic full stop
        case 0xff1f:  // fullwidth question mark
        case 0xff01:  // fullwidth exclamation mark
        case 0xff1a:  // fullwidth colon
        case 0x3001:  // ideographic comma
        case 0xff1b:  // fullwidth semicolon
        case 0xff09:  // fullwidth right parenthesis
        case 0x3011:  // right black lenticular bracket
        case 0x300b:  // right double angle bracket
        case 0x201d:  // right double quotation mark
        case 0x2019:  // right single quotation mark
            return true;
        default:
            return false;
    }
}

std::size_t previous_glyph_start(const std::string& text, std::size_t begin,
                                 std::size_t pos) {
    if (pos <= begin) return begin;
    std::size_t scan = begin;
    std::size_t last = begin;
    while (scan < pos) {
        last = scan;
        scan = glyph_end(text, scan);
    }
    return last;
}

// Line ranges for one width. ASCII words stay whole and a line never starts with
// closing punctuation; an over-long word is still split so the width bound holds.
std::vector<std::pair<std::size_t, std::size_t>> wrap_ranges_impl(
    const std::string& text, int width) {
    width = std::max(1, width);
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        const auto newline = text.find('\n', line_start);
        const auto end = newline == std::string::npos ? text.size() : newline;
        if (line_start == end) ranges.emplace_back(line_start, end);
        std::size_t part_start = line_start;
        while (part_start < end) {
            std::size_t fit_end = part_start;
            int fit_width = 0;
            while (fit_end < end) {
                const auto next = glyph_end(text, fit_end);
                const int next_width = glyph_width(text, fit_end, next);
                if (fit_width > 0 && fit_width + next_width > width) break;
                fit_width += next_width;
                fit_end = next;
            }
            if (fit_end >= end) {
                ranges.emplace_back(part_start, end);
                break;
            }
            std::size_t cut = fit_end;
            std::size_t space_start = part_start;
            std::size_t space_end = part_start;
            for (std::size_t scan = part_start; scan < fit_end;) {
                const auto next = glyph_end(text, scan);
                if (is_space_glyph(text, scan, next)) {
                    space_start = scan;
                    space_end = next;
                }
                scan = next;
            }
            if (space_start > part_start) {
                // Break after the last space and drop the space itself.
                ranges.emplace_back(part_start, space_start);
                part_start = space_end;
                continue;
            }
            if (is_closing_glyph(text, cut, end)) {
                // Never start a line with closing punctuation; carry the previous
                // glyph down with it. The backoff must still leave a non-empty
                // line, otherwise the loop would emit empty ranges forever when a
                // single glyph already fills the width.
                const std::size_t backed =
                    previous_glyph_start(text, part_start, cut);
                if (backed > part_start) cut = backed;
            }
            if (cut <= part_start) cut = fit_end;
            ranges.emplace_back(part_start, cut);
            part_start = cut;
        }
        if (newline == std::string::npos) break;
        line_start = newline + 1;
    }
    return ranges;
}

std::vector<std::string> wrap_impl(const std::string& text, int width) {
    std::vector<std::string> lines;
    for (const auto& range : wrap_ranges_impl(text, width)) {
        lines.push_back(text.substr(range.first, range.second - range.first));
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

int number_column_width(int highest_number) {
    return static_cast<int>(std::to_string(std::max(1, highest_number)).size());
}

std::string selection_marker(bool multi_select, bool selected) {
    if (multi_select) return selected ? "[x]" : "[ ]";
    return selected ? "(*)" : "( )";
}

std::string display_option_label(const std::string& label, bool recommended) {
    if (!recommended) return label;
    constexpr const char* kParenSuffix = "(Recommended)";
    constexpr const char* kBracketSuffix = "[Recommended]";
    if (label.size() >= std::char_traits<char>::length(kBracketSuffix) &&
        label.compare(label.size() - std::char_traits<char>::length(kBracketSuffix),
                      std::char_traits<char>::length(kBracketSuffix),
                      kBracketSuffix) == 0) {
        return label;
    }
    if (label.size() >= std::char_traits<char>::length(kParenSuffix) &&
        label.compare(label.size() - std::char_traits<char>::length(kParenSuffix),
                      std::char_traits<char>::length(kParenSuffix),
                      kParenSuffix) == 0) {
        return label.substr(0, label.size() -
                               std::char_traits<char>::length(kParenSuffix)) +
            kBracketSuffix;
    }
    return label + " [Recommended]";
}

std::string summary_answer_text(const AskQuestionAnswer& answer) {
    std::string value;
    for (const auto& selected : answer.selected) {
        if (!value.empty()) value += ", ";
        value += selected;
    }
    if (!answer.custom_text.empty()) {
        if (!value.empty()) value += ", ";
        value += answer.custom_text;
    }
    if (value.empty()) value = "Not answered";
    if (answer.auto_selected) value = "[Auto-selected] " + value;
    return value;
}

struct RowShape {
    AskQuestionLayoutKind kind = AskQuestionLayoutKind::Question;
    int question_index = -1;
    int option_index = -1;
    bool focused = false;
    bool selected = false;
    bool recommended = false;
    bool placeholder = false;
    int title_x = 0;
    int description_x = -1;
    int answer_x = -1;
    std::size_t text_byte_begin = 0;
    std::size_t text_byte_end = 0;
};

void append_row(AskQuestionLayout& layout, const RowShape& shape,
                std::string number, std::string marker, std::string title,
                std::string description, std::string answer, int y) {
    AskQuestionLayoutRow row;
    row.kind = shape.kind;
    row.question_index = shape.question_index;
    row.option_index = shape.option_index;
    row.number = std::move(number);
    row.marker = std::move(marker);
    row.title = std::move(title);
    row.description = std::move(description);
    row.answer = std::move(answer);
    row.focused = shape.focused;
    row.selected = shape.selected;
    row.recommended = shape.recommended;
    row.placeholder = shape.placeholder;
    row.title_x = shape.title_x;
    row.description_x = shape.description_x;
    row.answer_x = shape.answer_x;
    row.text_byte_begin = shape.text_byte_begin;
    row.text_byte_end = shape.text_byte_end;
    row.rect = {0, y, 0, 1};
    layout.rows.push_back(std::move(row));
}

} // namespace

int ask_question_display_width(const std::string& text) {
    return ask_question_text_width(text);
}

int ask_question_content_width_for_frame(int terminal_width,
                                         int measured_main_column_width,
                                         bool regular_sidebar_visible,
                                         int regular_sidebar_width) {
    terminal_width = std::max(1, terminal_width);
    regular_sidebar_width = std::max(0, regular_sidebar_width);

    int estimated_main_column_width =
        terminal_width - kOuterFrameHorizontalChromeWidth;
    if (regular_sidebar_visible) {
        estimated_main_column_width -=
            regular_sidebar_width + kSidebarSeparatorWidth;
    }

    int main_column_width = estimated_main_column_width;
    if (measured_main_column_width > kAskQuestionMainColumnChromeWidth) {
        main_column_width = std::min(
            measured_main_column_width, estimated_main_column_width);
    }

    const int terminal_bound = std::max(
        kAskQuestionMinimumContentWidth,
        terminal_width - kAskQuestionTerminalChromeWidth);
    const int main_column_bound = std::max(
        kAskQuestionMinimumContentWidth,
        main_column_width - kAskQuestionMainColumnChromeWidth);
    return std::min(terminal_bound, main_column_bound);
}

int ask_question_visible_rows_for_terminal(int terminal_rows,
                                           int minimum_visible_rows) {
    return std::max(std::clamp(minimum_visible_rows, 2, 12),
                    terminal_rows - 12);
}

std::vector<std::string> ask_question_wrap(const std::string& text, int width) {
    return wrap_impl(text, width);
}

std::size_t ask_question_text_byte_offset_for_x(const std::string& text,
                                                std::size_t byte_begin,
                                                std::size_t byte_end,
                                                int x) {
    byte_begin = std::min(byte_begin, text.size());
    byte_end = std::clamp(byte_end, byte_begin, text.size());
    if (x <= 0) return byte_begin;

    int column = 0;
    std::size_t pos = byte_begin;
    while (pos < byte_end) {
        const auto next = glyph_end(text, pos);
        const int width = glyph_width(text, pos, next);
        if (x < column + (width + 1) / 2) return pos;
        column += width;
        pos = next;
    }
    return byte_end;
}

AskQuestionLayout build_ask_question_layout(const AskQuestionLayoutInput& input) {
    AskQuestionLayout layout;
    if (!input.snapshot) return layout;

    const auto& snapshot = *input.snapshot;
    const int width = std::max(1, input.viewport_width);
    const int content_width = std::max(1, width - 2);
    layout.content_width = content_width;
    layout.terminal_too_narrow = content_width < 16;
    // The scrollbar occupies a small fixed column beside the content.
    const int row_content_width = std::max(1, content_width - 3);
    layout.column_gap = kColumnGap;
    layout.marker_width = kMarkerWidth;
    layout.scrollbar_x = width - 1;

    const bool summary_page = snapshot.page == AskQuestionPage::Summary;
    const int highest_number = summary_page
        ? std::max(1, static_cast<int>(snapshot.answers.size()))
        : std::max(1, static_cast<int>(snapshot.options.size()) + 1);
    layout.number_width = number_column_width(highest_number);
    layout.title_x =
        layout.number_width + kNumberMarkerGap + kMarkerWidth + kColumnGap;

    const int columns_width = std::max(2, row_content_width - layout.title_x);
    int natural_title = 0;
    for (const auto& option : snapshot.options) {
        natural_title = std::max(
            natural_title,
            ask_question_display_width(
                display_option_label(option.label, option.recommended)));
    }
    const int title_cap = std::max(
        kMinTitleWidth, std::min(kMaxTitleWidth, columns_width * 2 / 5));
    layout.title_width = std::clamp(natural_title, kMinTitleWidth, title_cap);
    layout.title_width = std::min(
        layout.title_width, std::max(1, columns_width - kColumnGap - 1));
    layout.description_width =
        std::max(1, columns_width - layout.title_width - kColumnGap);
    layout.description_x = layout.title_x + layout.title_width + kColumnGap;

    // Summary grid. Answers always start at the same x; the question column is
    // sized by the widest "question：" on the page so short questions pad out.
    const int summary_fixed = layout.number_width + kSummaryNumberGap;
    const int summary_avail = std::max(2, row_content_width - summary_fixed);
    int natural_question = 0;
    for (const auto& text : snapshot.question_texts) {
        natural_question = std::max(
            natural_question,
            ask_question_display_width(text) +
                ask_question_display_width("\xEF\xBC\x9A"));
    }
    if (summary_avail - kColumnGap - kMinSummaryAnswerWidth <
        kMinSummaryQuestionWidth) {
        layout.summary_stacked = true;
        layout.summary_question_width = summary_avail;
        layout.summary_answer_width = summary_avail;
        layout.summary_answer_x = summary_fixed;
    } else {
        const int question_cap =
            summary_avail - kColumnGap - kMinSummaryAnswerWidth;
        layout.summary_question_width = std::clamp(
            natural_question, kMinSummaryQuestionWidth, question_cap);
        layout.summary_answer_width = std::max(
            1, summary_avail - layout.summary_question_width - kColumnGap);
        layout.summary_answer_x =
            summary_fixed + layout.summary_question_width + kColumnGap;
    }

    int y = 0;
    if (summary_page) {
        append_row(layout, {AskQuestionLayoutKind::Header}, {}, {}, "Summary",
                   {}, {}, y++);
        for (std::size_t qi = 0; qi < snapshot.answers.size(); ++qi) {
            const auto& answer = snapshot.answers[qi];
            const std::string question_text =
                qi < snapshot.question_texts.size()
                    ? snapshot.question_texts[qi]
                    : std::string{};
            const std::string question = question_text + "\xEF\xBC\x9A";
            const std::string value = summary_answer_text(answer);
            const auto question_lines =
                ask_question_wrap(question, layout.summary_question_width);
            const auto answer_lines =
                ask_question_wrap(value, std::max(1, layout.summary_answer_width));
            RowShape shape;
            shape.kind = AskQuestionLayoutKind::Summary;
            shape.question_index = static_cast<int>(qi);
            shape.selected = !answer.not_answered;
            shape.recommended = answer.auto_selected;
            shape.answer_x = layout.summary_answer_x;
            if (layout.summary_stacked) {
                // The panel is too narrow for two columns: emit the question
                // block, then the answers indented to the same column.
                for (std::size_t line = 0; line < question_lines.size(); ++line) {
                    append_row(layout, shape,
                               line == 0 ? std::to_string(qi + 1) : "", {},
                               question_lines[line], {}, {}, y++);
                }
                RowShape answer_shape;
                answer_shape.kind = AskQuestionLayoutKind::SummaryAnswer;
                answer_shape.question_index = static_cast<int>(qi);
                answer_shape.selected = !answer.not_answered;
                answer_shape.recommended = answer.auto_selected;
                answer_shape.answer_x = layout.summary_answer_x;
                for (const auto& line : answer_lines) {
                    append_row(layout, answer_shape, {}, {}, line, {}, {}, y++);
                }
            } else {
                // Two columns, top aligned: answer line N shares the row with
                // question line N, and the block grows to the taller column.
                const std::size_t line_count =
                    std::max(question_lines.size(), answer_lines.size());
                for (std::size_t line = 0; line < line_count; ++line) {
                    append_row(
                        layout, shape,
                        line == 0 ? std::to_string(qi + 1) : "", {},
                        line < question_lines.size() ? question_lines[line] : "",
                        {},
                        line < answer_lines.size() ? answer_lines[line] : "",
                        y++);
                }
            }
            if (qi + 1 < snapshot.answers.size()) {
                append_row(layout, {AskQuestionLayoutKind::Summary}, {}, {}, {},
                           {}, {}, y++);
            }
        }
    } else {
        const std::string header_suffix = snapshot.question_header.empty()
            ? std::string{}
            : " [" + snapshot.question_header + "]";
        append_row(layout, {AskQuestionLayoutKind::Header}, {}, {},
                   "Question " + std::to_string(snapshot.current_question + 1) +
                       "/" +
                       std::to_string(std::max(1, snapshot.total_questions)) +
                       header_suffix,
                   {}, {}, y++);
        for (const auto& line :
             ask_question_wrap(snapshot.question_text, content_width)) {
            append_row(layout, {AskQuestionLayoutKind::Question}, {}, {}, line,
                       {}, {}, y++);
        }
        const bool multi = snapshot.multi_select;
        for (std::size_t i = 0; i < snapshot.options.size(); ++i) {
            const auto& option = snapshot.options[i];
            RowShape shape;
            shape.kind = AskQuestionLayoutKind::Option;
            shape.question_index = snapshot.current_question;
            shape.option_index = static_cast<int>(i);
            shape.focused = snapshot.focused_option == static_cast<int>(i);
            shape.selected = option.selected;
            shape.recommended = option.recommended;
            shape.title_x = layout.title_x;
            shape.description_x = layout.description_x;
            const std::string marker = selection_marker(multi, option.selected);
            const auto title_lines = ask_question_wrap(
                display_option_label(option.label, option.recommended),
                layout.title_width);
            const auto description_lines =
                ask_question_wrap(option.description, layout.description_width);
            const std::size_t line_count =
                std::max(title_lines.size(), description_lines.size());
            for (std::size_t line = 0; line < line_count; ++line) {
                append_row(layout, shape,
                           line == 0 ? std::to_string(i + 1) : "",
                           line == 0 ? marker : "",
                           line < title_lines.size() ? title_lines[line] : "",
                           line < description_lines.size()
                               ? description_lines[line]
                               : "",
                           {}, y++);
            }
        }

        const int custom_index = static_cast<int>(snapshot.options.size());
        RowShape custom_shape;
        custom_shape.kind = AskQuestionLayoutKind::Custom;
        custom_shape.question_index = snapshot.current_question;
        custom_shape.option_index = custom_index;
        custom_shape.focused = snapshot.focused_option == custom_index;
        custom_shape.selected = snapshot.custom_selected;
        custom_shape.title_x = layout.title_x;
        custom_shape.description_x = layout.description_x;
        const std::string custom_marker =
            selection_marker(multi, snapshot.custom_selected);
        const bool show_full_custom = snapshot.editing_custom ||
            snapshot.custom_selected || custom_shape.focused;
        std::string custom_value;
        if (snapshot.custom_text.empty()) {
            // Keep the placeholder visible unless the user is actively editing
            // an empty buffer, where only the caret should show.
            if (!snapshot.editing_custom) {
                custom_value = kCustomPlaceholder;
                custom_shape.placeholder = true;
            }
        } else if (show_full_custom) {
            custom_value = snapshot.custom_text;
        } else {
            const auto first_line_end = snapshot.custom_text.find('\n');
            custom_value = snapshot.custom_text.substr(0, first_line_end);
            if (first_line_end != std::string::npos) custom_value += "...";
        }
        const int custom_width = std::max(
            1, layout.title_width + kColumnGap + layout.description_width);
        // Keep one cell for the insertion cursor at the end of a full line;
        // otherwise FTXUI places it in the adjacent scrollbar column.
        const int custom_wrap_width = std::max(
            1, custom_width - (snapshot.editing_custom ? 1 : 0));
        const auto custom_ranges = wrap_ranges_impl(
            snapshot.editing_custom ? snapshot.custom_text : custom_value,
            custom_wrap_width);
        const auto ranges = custom_ranges.empty()
            ? std::vector<std::pair<std::size_t, std::size_t>>{{0, 0}}
            : custom_ranges;
        for (std::size_t line = 0; line < ranges.size(); ++line) {
            const auto begin = ranges[line].first;
            const auto end = ranges[line].second;
            RowShape shape = custom_shape;
            shape.text_byte_begin = begin;
            shape.text_byte_end = end;
            append_row(layout, shape,
                       line == 0 ? std::to_string(custom_index + 1) : "",
                       line == 0 ? custom_marker : "",
                       snapshot.editing_custom
                           ? snapshot.custom_text.substr(begin, end - begin)
                           : custom_value.substr(begin, end - begin),
                       {}, {}, y++);
        }
    }
    if (input.timeout_remaining_seconds > 0) {
        append_row(
            layout, {AskQuestionLayoutKind::Hint}, {}, {},
            std::to_string(input.timeout_remaining_seconds) +
                "s remaining; unanswered items use Recommended when available",
            {}, {}, y++);
    }
    layout.total_rows = static_cast<int>(layout.rows.size());
    const int available_rows = std::max(1, input.viewport_height);
    const int requested_minimum = std::clamp(input.minimum_visible_rows, 2, 12);
    layout.visible_rows = std::min(
        available_rows, std::max(layout.total_rows, requested_minimum));
    int requested_scroll = snapshot.scroll_offset;
    int focused_begin = -1;
    int focused_end = -1;
    int cursor_row = -1;
    for (int i = 0; i < layout.total_rows; ++i) {
        const auto& row = layout.rows[static_cast<std::size_t>(i)];
        if (snapshot.editing_custom && row.kind == AskQuestionLayoutKind::Custom &&
            snapshot.editor.cursor >= row.text_byte_begin &&
            snapshot.editor.cursor <= row.text_byte_end) {
            // Prefer the next row at a shared soft-wrap boundary, so both the
            // renderer and scrolling use the same insertion point.
            cursor_row = i;
        }
        if (!row.focused) continue;
        if (focused_begin < 0) focused_begin = i;
        focused_end = i;
    }
    if (cursor_row >= 0) {
        layout.rows[static_cast<std::size_t>(cursor_row)].has_cursor = true;
        focused_begin = focused_end = cursor_row;
    }
    if (snapshot.follow_focus && focused_begin >= 0 && layout.visible_rows > 0) {
        // Long preset descriptions cannot fit at once; reveal their beginning.
        focused_end = std::min(focused_end, focused_begin + layout.visible_rows - 1);
        if (focused_begin < requested_scroll) requested_scroll = focused_begin;
        if (focused_end >= requested_scroll + layout.visible_rows) {
            requested_scroll = focused_end - layout.visible_rows + 1;
        }
    }
    layout.scroll_offset = clamp_ask_question_layout_scroll(
        requested_scroll, layout.total_rows, layout.visible_rows);
    const int end = std::min(layout.total_rows,
                             layout.scroll_offset + layout.visible_rows);
    layout.scrollbar_track = {width - 1, 0, 1, layout.visible_rows};
    if (layout.total_rows > layout.visible_rows && layout.visible_rows > 0) {
        const int thumb_height = std::max(
            1, layout.visible_rows * layout.visible_rows / layout.total_rows);
        const int thumb_range = layout.visible_rows - thumb_height;
        const int max_offset = layout.total_rows - layout.visible_rows;
        const int thumb_y = max_offset > 0
            ? layout.scroll_offset * thumb_range / max_offset
            : 0;
        layout.scrollbar_thumb = {width - 1, thumb_y, 1, thumb_height};
    } else {
        layout.scrollbar_thumb = {width - 1, 0, 1, 0};
    }
    for (int i = 0; i < layout.total_rows; ++i) {
        auto& row = layout.rows[static_cast<std::size_t>(i)];
        row.rect = {0, i - layout.scroll_offset, content_width, 1};
        if (i < layout.scroll_offset || i >= end) row.rect.height = 0;
        if (row.focused) {
            if (layout.focused_row_begin < 0) layout.focused_row_begin = i;
            layout.focused_row_end = i;
        }
    }
    return layout;
}

int ask_question_scroll_offset_for_y(int y, const AskQuestionLayout& layout) {
    return ask_question_scroll_offset_for_track_y(
        y, layout.scrollbar_track.y, layout.scrollbar_track.height,
        layout.total_rows, layout.visible_rows);
}

int clamp_ask_question_layout_scroll(int offset, int total_rows,
                                     int visible_rows) {
    if (total_rows <= 0 || visible_rows <= 0 || total_rows <= visible_rows) {
        return 0;
    }
    return std::clamp(offset, 0, total_rows - visible_rows);
}

int ask_question_scroll_offset_for_track_y(int y,
                                           int track_y,
                                           int track_height,
                                           int total_rows,
                                           int visible_rows) {
    if (total_rows <= visible_rows || visible_rows <= 1 || track_height <= 1) {
        return 0;
    }
    const int relative = std::clamp(y - track_y, 0, track_height - 1);
    const int max_offset = total_rows - visible_rows;
    return clamp_ask_question_layout_scroll(
        relative * max_offset / (track_height - 1), total_rows, visible_rows);
}

int hit_test_ask_question_layout(const AskQuestionLayout& layout, int x, int y) {
    const auto hit = hit_test_ask_question_target(layout, x, y);
    if (hit.kind == AskQuestionHitKind::Option ||
        hit.kind == AskQuestionHitKind::Custom) {
        return hit.option_index;
    }
    return -1;
}

AskQuestionHit hit_test_ask_question_target(const AskQuestionLayout& layout,
                                            int x, int y) {
    if (layout.total_rows > layout.visible_rows &&
        layout.scrollbar_track.contains(x, y)) {
        return {AskQuestionHitKind::Scrollbar, -1, -1};
    }
    for (const auto& row : layout.rows) {
        if (row.rect.height <= 0 || !row.rect.contains(x, y)) continue;
        switch (row.kind) {
            case AskQuestionLayoutKind::Option:
                return {AskQuestionHitKind::Option, row.question_index,
                        row.option_index};
            case AskQuestionLayoutKind::Custom:
                return {AskQuestionHitKind::Custom, row.question_index,
                        row.option_index};
            case AskQuestionLayoutKind::Summary:
            case AskQuestionLayoutKind::SummaryAnswer:
                return row.question_index >= 0
                    ? AskQuestionHit{AskQuestionHitKind::SummaryQuestion,
                                     row.question_index, -1}
                    : AskQuestionHit{AskQuestionHitKind::None, -1, -1};
            default:
                break;
        }
    }
    return {};
}

} // namespace acecode::tui
