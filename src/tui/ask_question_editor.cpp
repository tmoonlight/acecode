#include "ask_question_editor.hpp"

#include "ask_question_text.hpp"
#include "tui/text_input_ops.hpp"

#include <algorithm>
#include <utility>

namespace acecode::tui {
namespace {

std::size_t previous_boundary(const std::string& text, std::size_t pos) {
    pos = acecode::clamp_utf8_boundary(text, pos);
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 &&
           (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    return pos;
}

std::size_t next_boundary(const std::string& text, std::size_t pos) {
    pos = acecode::clamp_utf8_boundary(text, pos);
    if (pos >= text.size()) return text.size();
    ++pos;
    while (pos < text.size() &&
           (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
        ++pos;
    }
    return pos;
}

std::size_t line_start(const std::string& text, std::size_t pos) {
    pos = acecode::clamp_utf8_boundary(text, pos);
    if (pos == 0) return 0;
    const auto newline = text.rfind('\n', pos - 1);
    return newline == std::string::npos ? 0 : newline + 1;
}

std::size_t line_end(const std::string& text, std::size_t pos) {
    pos = acecode::clamp_utf8_boundary(text, pos);
    const auto newline = text.find('\n', pos);
    return newline == std::string::npos ? text.size() : newline;
}

std::size_t display_column(const std::string& text,
                           std::size_t start,
                           std::size_t pos) {
    std::size_t column = 0;
    for (std::size_t cur = start; cur < pos;) {
        const std::size_t next = next_boundary(text, cur);
        column += static_cast<std::size_t>(
            std::max(0, ask_question_codepoint_width(
                std::string_view(text).substr(cur, next - cur))));
        cur = next;
    }
    return column;
}

std::size_t offset_at_display_column(const std::string& text,
                                     std::size_t start,
                                     std::size_t end,
                                     std::size_t column) {
    std::size_t cur = start;
    std::size_t current_column = 0;
    while (cur < end) {
        const std::size_t next = next_boundary(text, cur);
        const auto glyph_width = static_cast<std::size_t>(std::max(
            0, ask_question_codepoint_width(
                std::string_view(text).substr(cur, next - cur))));
        if (current_column + glyph_width > column) break;
        current_column += glyph_width;
        cur = next;
    }
    return cur;
}

} // namespace

AskQuestionEditor::AskQuestionEditor(std::string text) : text_(std::move(text)) {
    cursor_ = text_.size();
}

bool AskQuestionEditor::has_selection() const {
    return selection_anchor_.has_value() && *selection_anchor_ != cursor_;
}

std::pair<std::size_t, std::size_t> AskQuestionEditor::selection_range() const {
    if (!has_selection()) return {cursor_, cursor_};
    return std::minmax(cursor_, *selection_anchor_);
}

std::string AskQuestionEditor::selected_text() const {
    const auto [start, end] = selection_range();
    return text_.substr(start, end - start);
}

void AskQuestionEditor::set_text(std::string text) {
    text_ = std::move(text);
    cursor_ = acecode::clamp_utf8_boundary(text_, cursor_);
    clear_selection();
    vertical_goal_column_.reset();
}

void AskQuestionEditor::set_cursor(std::size_t cursor) {
    cursor_ = acecode::clamp_utf8_boundary(text_, cursor);
    clear_selection();
    vertical_goal_column_.reset();
}

void AskQuestionEditor::move_cursor_to(std::size_t cursor, bool extend_selection) {
    move_to(cursor, extend_selection);
    vertical_goal_column_.reset();
}

void AskQuestionEditor::clear_selection() {
    selection_anchor_.reset();
}

void AskQuestionEditor::select_all() {
    selection_anchor_ = 0;
    cursor_ = text_.size();
    vertical_goal_column_.reset();
}

bool AskQuestionEditor::erase_selection() {
    if (!has_selection()) return false;
    const auto [start, end] = selection_range();
    text_.erase(start, end - start);
    cursor_ = start;
    clear_selection();
    return true;
}

void AskQuestionEditor::insert(std::string_view text) {
    erase_selection();
    // A collapsed anchor is not a selection. Clear it before cursor advances so
    // inserted text never becomes an accidental selected range.
    clear_selection();
    text_.insert(cursor_, text.data(), text.size());
    cursor_ += text.size();
    cursor_ = acecode::clamp_utf8_boundary(text_, cursor_);
    vertical_goal_column_.reset();
}

bool AskQuestionEditor::backspace() {
    if (erase_selection()) {
        vertical_goal_column_.reset();
        return true;
    }
    const std::size_t before = previous_boundary(text_, cursor_);
    if (before == cursor_) return false;
    text_.erase(before, cursor_ - before);
    cursor_ = before;
    vertical_goal_column_.reset();
    return true;
}

bool AskQuestionEditor::erase_forward() {
    if (erase_selection()) {
        vertical_goal_column_.reset();
        return true;
    }
    const std::size_t after = next_boundary(text_, cursor_);
    if (after == cursor_) return false;
    text_.erase(cursor_, after - cursor_);
    vertical_goal_column_.reset();
    return true;
}

void AskQuestionEditor::move_to(std::size_t target, bool extend_selection) {
    target = acecode::clamp_utf8_boundary(text_, target);
    if (extend_selection) {
        if (!selection_anchor_) selection_anchor_ = cursor_;
    } else {
        clear_selection();
    }
    cursor_ = target;
}

bool AskQuestionEditor::move_left(bool extend_selection) {
    if (!extend_selection && has_selection()) {
        const auto [start, ignored] = selection_range();
        (void)ignored;
        move_to(start, false);
        vertical_goal_column_.reset();
        return true;
    }
    const std::size_t target = previous_boundary(text_, cursor_);
    if (target == cursor_) return false;
    move_to(target, extend_selection);
    vertical_goal_column_.reset();
    return true;
}

bool AskQuestionEditor::move_right(bool extend_selection) {
    if (!extend_selection && has_selection()) {
        const auto [ignored, end] = selection_range();
        (void)ignored;
        move_to(end, false);
        vertical_goal_column_.reset();
        return true;
    }
    const std::size_t target = next_boundary(text_, cursor_);
    if (target == cursor_) return false;
    move_to(target, extend_selection);
    vertical_goal_column_.reset();
    return true;
}

bool AskQuestionEditor::move_up(bool extend_selection) {
    const std::size_t current_start = line_start(text_, cursor_);
    if (current_start == 0) return false;
    const std::size_t current_column = vertical_goal_column_.value_or(
        display_column(text_, current_start, cursor_));
    vertical_goal_column_ = current_column;
    const std::size_t previous_end = current_start - 1;
    const std::size_t previous_start = line_start(text_, previous_end);
    move_to(offset_at_display_column(text_, previous_start, previous_end,
                                     current_column),
            extend_selection);
    return true;
}

bool AskQuestionEditor::move_down(bool extend_selection) {
    const std::size_t current_end = line_end(text_, cursor_);
    if (current_end >= text_.size()) return false;
    const std::size_t current_start = line_start(text_, cursor_);
    const std::size_t current_column = vertical_goal_column_.value_or(
        display_column(text_, current_start, cursor_));
    vertical_goal_column_ = current_column;
    const std::size_t next_start = current_end + 1;
    const std::size_t next_end = line_end(text_, next_start);
    move_to(offset_at_display_column(text_, next_start, next_end,
                                     current_column),
            extend_selection);
    return true;
}

void AskQuestionEditor::move_home(bool extend_selection) {
    move_to(line_start(text_, cursor_), extend_selection);
    vertical_goal_column_.reset();
}

void AskQuestionEditor::move_end(bool extend_selection) {
    move_to(line_end(text_, cursor_), extend_selection);
    vertical_goal_column_.reset();
}

std::string AskQuestionEditor::cut_selection() {
    std::string result = selected_text();
    if (!result.empty()) {
        erase_selection();
        vertical_goal_column_.reset();
    }
    return result;
}

} // namespace acecode::tui
