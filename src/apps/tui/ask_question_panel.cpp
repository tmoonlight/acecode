#include "ask_question_panel.hpp"

#include "ask_question_text.hpp"
#include "tui_helpers.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace acecode::tui {
namespace {

constexpr const char* kArrowUpDown = "\xE2\x86\x91\xE2\x86\x93";    // up/down
constexpr const char* kArrowLeft = "\xE2\x86\x90";                  // left
constexpr const char* kArrowRight = "\xE2\x86\x92";                 // right
constexpr const char* kArrowLeftRight = "\xE2\x86\x90\xE2\x86\x92"; // left/right

// Separator between shortcut entries in the footer.
constexpr int kHelpEntryGap = 3;

int display_width(const std::string& text) {
    return ask_question_text_width(text);
}

ftxui::Element fixed_cell(int width, ftxui::Element element) {
    return std::move(element) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, std::max(1, width));
}

ftxui::Element spaces(int count) {
    return ftxui::text(std::string(static_cast<std::size_t>(std::max(1, count)),
                                   ' '));
}

// Lays row text out at absolute column offsets so what this draws is exactly the
// four-slot grid AskQuestionLayout computed.
class RowBuilder {
public:
    void place(int column, int width, ftxui::Element element) {
        if (width <= 0) return;
        if (column > column_) {
            cells_.push_back(
                fixed_cell(column - column_, spaces(column - column_)));
            column_ = column;
        }
        cells_.push_back(fixed_cell(width, std::move(element)));
        column_ += width;
    }

    ftxui::Element finish() {
        cells_.push_back(ftxui::filler());
        return ftxui::hbox(std::move(cells_));
    }

private:
    int column_ = 0;
    ftxui::Elements cells_;
};

ftxui::Element styled_text(const std::string& value, ftxui::Color color) {
    return ftxui::text(value) | ftxui::color(color);
}

// Draws one line of the inline editor, including the terminal caret when the
// insertion point falls on this line. `focusCursorBlock` inside
// render_wrapped_input_text binds the terminal cursor to the caret; the panel
// itself must never claim focus.
ftxui::Element build_custom_editor_line(const AskQuestionLayoutRow& row,
                                        const AskQuestionSnapshot& snapshot,
                                        const AskQuestionPanelColors& colors) {
    const std::size_t begin = row.text_byte_begin;
    const std::size_t end = row.text_byte_end;
    const std::size_t cursor = snapshot.editor.cursor;
    const bool has_selection = snapshot.editor.has_selection &&
                               snapshot.editor.selection_anchor.has_value();
    const std::optional<std::pair<std::size_t, std::size_t>> selection =
        has_selection
            ? std::optional<std::pair<std::size_t, std::size_t>>(
                  std::minmax(cursor, *snapshot.editor.selection_anchor))
            : std::nullopt;

    ftxui::Element content = ftxui::text(row.title);
    if (selection.has_value()) {
        const std::size_t selected_begin = std::max(begin, selection->first);
        const std::size_t selected_end = std::min(end, selection->second);
        const bool cursor_on_this_line = row.has_cursor;
        if (selected_begin < selected_end && !cursor_on_this_line) {
            ftxui::Elements fragments;
            const auto append_fragment = [&](std::size_t from, std::size_t to,
                                             bool selected) {
                if (from >= to) return;
                auto fragment =
                    ftxui::text(row.title.substr(from - begin, to - from));
                if (selected) {
                    fragment = std::move(fragment) |
                               ftxui::color(colors.selection_fg) |
                               ftxui::bgcolor(colors.selection_bg);
                }
                fragments.push_back(std::move(fragment));
            };
            append_fragment(begin, selected_begin, false);
            append_fragment(selected_begin, selected_end, true);
            append_fragment(selected_end, end, false);
            content = ftxui::hbox(std::move(fragments));
        }
    }
    if (row.has_cursor) {
        std::optional<std::size_t> anchor;
        if (snapshot.editor.selection_anchor.has_value()) {
            anchor =
                std::clamp(*snapshot.editor.selection_anchor, begin, end) - begin;
        }
        content = render_wrapped_input_text(row.title, cursor - begin, nullptr,
                                            anchor);
    }
    return content;
}

ftxui::Element build_row(const AskQuestionLayoutRow& row,
                         const AskQuestionLayout& layout,
                         const AskQuestionSnapshot& snapshot,
                         const AskQuestionPanelColors& colors) {
    RowBuilder builder;
    switch (row.kind) {
        case AskQuestionLayoutKind::Header:
        case AskQuestionLayoutKind::Question: {
            builder.place(0, layout.content_width,
                          ftxui::text(row.title) | ftxui::bold |
                              ftxui::color(colors.question));
            break;
        }
        case AskQuestionLayoutKind::Hint: {
            builder.place(0, layout.content_width,
                          styled_text(row.title, colors.secondary));
            break;
        }
        case AskQuestionLayoutKind::Option: {
            builder.place(0, layout.number_width,
                          styled_text(row.number, colors.description));
            builder.place(layout.number_width + 1, layout.marker_width,
                          styled_text(row.marker, colors.description));
            builder.place(layout.title_x, layout.title_width,
                          styled_text(row.title, colors.answer));
            builder.place(layout.description_x, layout.description_width,
                          styled_text(row.description, colors.description));
            break;
        }
        case AskQuestionLayoutKind::Custom: {
            builder.place(0, layout.number_width,
                          styled_text(row.number, colors.description));
            builder.place(layout.number_width + 1, layout.marker_width,
                          styled_text(row.marker, colors.description));
            const int span = std::max(
                1, layout.title_width + layout.column_gap +
                       layout.description_width);
            const bool editing_this_line =
                snapshot.editing_custom &&
                row.question_index == snapshot.current_question &&
                row.option_index == static_cast<int>(snapshot.options.size());
            ftxui::Element content;
            if (editing_this_line) {
                content = build_custom_editor_line(row, snapshot, colors);
            } else {
                content = styled_text(
                    row.title,
                    row.placeholder ? colors.placeholder : colors.answer);
            }
            builder.place(layout.title_x, span, std::move(content));
            break;
        }
        case AskQuestionLayoutKind::Summary: {
            builder.place(0, layout.number_width,
                          styled_text(row.number, colors.description));
            builder.place(layout.number_width + 2, layout.summary_question_width,
                          styled_text(row.title, colors.answer));
            if (!row.answer.empty() && row.answer_x >= 0) {
                builder.place(row.answer_x, layout.summary_answer_width,
                              styled_text(row.answer, colors.answer));
            }
            break;
        }
        case AskQuestionLayoutKind::SummaryAnswer: {
            const int at =
                row.answer_x >= 0 ? row.answer_x : layout.number_width + 2;
            builder.place(at, layout.summary_question_width,
                          styled_text(row.title, colors.answer));
            break;
        }
    }
    ftxui::Element element = builder.finish();
    const bool focusable_row = row.kind == AskQuestionLayoutKind::Option ||
                               row.kind == AskQuestionLayoutKind::Custom;
    element = element | ftxui::bgcolor(
                  focusable_row && row.focused ? colors.focus_bg
                                               : colors.panel_bg);
    return element;
}

} // namespace

std::vector<AskQuestionHelpEntry> ask_question_help_entries(
    const AskQuestionSnapshot& snapshot, bool scrollable) {
    std::vector<AskQuestionHelpEntry> entries;
    const int custom_index = static_cast<int>(snapshot.options.size());
    if (snapshot.page == AskQuestionPage::Summary) {
        entries = {{kArrowLeft, "返回末题"},
                   {kArrowRight, "返回首题"},
                   {"Enter", "提交"},
                   {"Esc", "取消"}};
    } else if (snapshot.editing_custom) {
        entries = {{kArrowLeftRight, "移动光标"},
                   {kArrowUpDown, "跨行移动"},
                   {"Enter", "提交"},
                   {"Ctrl+Enter", "换行"},
                   {"Esc", "结束编辑"},
                   {"Shift+X", "取消"}};
    } else if (snapshot.focused_option == custom_index) {
        entries = {{kArrowUpDown, "移动焦点"},
                   {"Enter", "编辑/提交"},
                   {"1-N", "快速选择"},
                   {"Esc", "清除"},
                   {"Shift+X", "取消"}};
    } else if (snapshot.multi_select) {
        entries = {{kArrowUpDown, "移动焦点"},
                   {"Space", "选中/取消"},
                   {"Enter", "提交"},
                   {"1-N", "快速选择"},
                   {"Y", "复制"},
                   {"Esc", "清除"},
                   {"Shift+X", "取消"}};
    } else {
        entries = {{kArrowUpDown, "移动焦点"},
                   {"Enter", "提交"},
                   {"1-N", "快速选择"},
                   {"Y", "复制"},
                   {"Esc", "清除"},
                   {"Shift+X", "取消"}};
    }
    if (scrollable) entries.push_back({"PgUp/PgDn", "滚动"});
    return entries;
}

ftxui::Element build_ask_question_help_line(
    const std::vector<AskQuestionHelpEntry>& help,
    const AskQuestionPanelColors& colors, int width, int minimum_rows) {
    width = std::max(1, width);
    minimum_rows = std::max(1, minimum_rows);
    const auto entry_width = [](const AskQuestionHelpEntry& entry) {
        return display_width(entry.key) + 2 + display_width(entry.action);
    };
    std::vector<ftxui::Elements> lines;
    ftxui::Elements current;
    int used = 0;
    for (const auto& entry : help) {
        const int needed =
            entry_width(entry) + (current.empty() ? 0 : kHelpEntryGap);
        if (!current.empty() && used + needed > width) {
            lines.push_back(std::move(current));
            current = ftxui::Elements{};
            used = 0;
        }
        if (!current.empty()) {
            current.push_back(spaces(kHelpEntryGap));
            used += kHelpEntryGap;
        }
        current.push_back(styled_text(entry.key, colors.answer));
        current.push_back(styled_text(": " + entry.action, colors.secondary));
        used += entry_width(entry);
    }
    if (!current.empty()) lines.push_back(std::move(current));

    ftxui::Elements rows;
    for (auto& line : lines) {
        rows.push_back(ftxui::hbox(std::move(line)));
    }
    // Pad to the requested height so the footer's row count is a function of the
    // panel being open, not of which interaction state is active.
    while (static_cast<int>(rows.size()) < minimum_rows) {
        rows.push_back(ftxui::text(""));
    }
    if (rows.empty()) return ftxui::text("");
    return ftxui::vbox(std::move(rows));
}

ftxui::Element build_ask_question_panel(const AskQuestionPanelInput& input) {
    if (!input.layout || !input.snapshot) return ftxui::emptyElement();
    const auto& layout = *input.layout;
    const auto& snapshot = *input.snapshot;
    const auto& colors = input.colors;

    const int row_count =
        input.terminal_too_narrow ? 2 : std::max(1, layout.visible_rows);

    ftxui::Elements content_rows;
    if (input.terminal_too_narrow) {
        content_rows.push_back(ftxui::text(" Terminal too narrow ") |
                               ftxui::bold |
                               ftxui::color(colors.question));
        content_rows.push_back(ftxui::text(" Resize the terminal to continue ") |
                               ftxui::color(colors.secondary));
    } else {
        const int begin = layout.scroll_offset;
        const int end =
            std::min(layout.total_rows, begin + layout.visible_rows);
        for (int i = begin; i < end && i < begin + row_count; ++i) {
            const auto& row = layout.rows[static_cast<std::size_t>(i)];
            const int visible_row = i - begin;
            auto element = build_row(row, layout, snapshot, colors);
            if (input.row_boxes && visible_row >= 0 &&
                visible_row < static_cast<int>(input.row_boxes->size())) {
                element = std::move(element) |
                          ftxui::reflect(
                              (*input.row_boxes)[static_cast<std::size_t>(
                                  visible_row)]);
            }
            content_rows.push_back(std::move(element));
        }
    }
    while (static_cast<int>(content_rows.size()) < row_count) {
        content_rows.push_back(ftxui::hbox({ftxui::filler()}) |
                               ftxui::bgcolor(colors.panel_bg));
    }

    ftxui::Elements bar_rows;
    for (int i = 0; i < row_count; ++i) {
        const bool thumb = layout.scrollbar_thumb.height > 0 &&
                           i >= layout.scrollbar_thumb.y &&
                           i < layout.scrollbar_thumb.y +
                                   layout.scrollbar_thumb.height;
        bar_rows.push_back(ftxui::text(thumb ? " | " : "   ") |
                           ftxui::color(thumb ? colors.border
                                              : colors.description));
    }

    auto scrollbar = ftxui::vbox(std::move(bar_rows));
    if (input.scrollbar_box) {
        scrollbar = std::move(scrollbar) | ftxui::reflect(*input.scrollbar_box);
    }
    auto body = ftxui::hbox({
        ftxui::vbox(std::move(content_rows)) | ftxui::flex,
        std::move(scrollbar),
    });
    // bgcolor only paints cell backgrounds; it never erases the characters
    // underneath. Without clear_under the chat text behind the panel shows
    // through every cell the panel did not write a glyph into (short option
    // labels, the gap after a wrapped row, the empty row padding), which reads
    // as unrelated text leaking into the question box.
    auto panel = std::move(body) |
                 ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, row_count) |
                 ftxui::borderStyled(ftxui::ROUNDED, colors.border) |
                 ftxui::bgcolor(colors.panel_bg) | ftxui::clear_under;
    if (input.overlay_box) {
        panel = std::move(panel) | ftxui::reflect(*input.overlay_box);
    }
    return panel;
}

} // namespace acecode::tui
