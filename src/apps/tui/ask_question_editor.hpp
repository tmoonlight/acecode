#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace acecode::tui {

// UTF-8-aware, multi-line editing state for AskUserQuestion custom answers.
// Cursor and selection offsets are byte offsets, always normalized to codepoint
// boundaries. This type is intentionally independent of FTXUI and TuiState.
class AskQuestionEditor {
public:
    explicit AskQuestionEditor(std::string text = {});

    const std::string& text() const { return text_; }
    std::size_t cursor() const { return cursor_; }
    const std::optional<std::size_t>& selection_anchor() const {
        return selection_anchor_;
    }
    bool has_selection() const;
    std::string selected_text() const;

    void set_text(std::string text);
    void set_cursor(std::size_t cursor);
    void move_cursor_to(std::size_t cursor, bool extend_selection = false);
    void clear_selection();
    void select_all();

    void insert(std::string_view text);
    bool backspace();
    bool erase_forward();
    bool move_left(bool extend_selection = false);
    bool move_right(bool extend_selection = false);
    bool move_up(bool extend_selection = false);
    bool move_down(bool extend_selection = false);
    void move_home(bool extend_selection = false);
    void move_end(bool extend_selection = false);
    std::string cut_selection();
    bool erase_selection();

private:
    std::string text_;
    std::size_t cursor_ = 0;
    std::optional<std::size_t> selection_anchor_;
    std::optional<std::size_t> vertical_goal_column_;

    std::pair<std::size_t, std::size_t> selection_range() const;
    void move_to(std::size_t target, bool extend_selection);
};

} // namespace acecode::tui
