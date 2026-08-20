#pragma once

#include "tool/ask_user_question_tool.hpp"

#include <ftxui/screen/box.hpp>

#include <string>
#include <vector>

namespace acecode::tui {

enum class AskOverlayRowKind {
    Header,
    Body,
    Option,
    Hint,
    CustomPrompt,
    Blank,
};

struct AskOverlayRow {
    std::string       text;
    AskOverlayRowKind kind = AskOverlayRowKind::Body;
    int               option_index = -1;
    bool              focused = false;
    bool              continuation = false;
};

struct AskOverlayLayoutInput {
    const AskQuestion*       question = nullptr;
    bool                     submit_page = false;
    int                      current_question_index = 0;
    int                      total_questions = 1;
    int                      option_focus = 0;
    int                      selected_option = -1;
    bool                     question_answered = false;
    std::vector<bool>        multi_selected;
    std::vector<bool>        answered_questions;
    bool                     other_input_active = false;
    int                      submit_focus = 0;
    int                      content_width = 80;
    // question_policy=timeout 的静态提示秒数;>0 时在题目页 hint 行后追加
    // 「Xs 无操作将自动选择推荐项」提示(add-ask-question-policy)。
    int                      timeout_hint_seconds = 0;
};

struct AskOverlayLayout {
    std::vector<AskOverlayRow> rows;
    int                        focused_row_begin = -1;
    int                        focused_row_end = -1;
};

int display_width_cells(const std::string& text);

int ask_overlay_content_width_for_frame(int terminal_width,
                                        int measured_main_column_width,
                                        bool regular_sidebar_visible,
                                        int regular_sidebar_width);

AskOverlayLayout build_ask_overlay_layout(const AskOverlayLayoutInput& input);

int ask_overlay_visible_rows_for_terminal(int terminal_rows);

int clamp_scroll_offset(int offset, int total_rows, int visible_rows);
int scroll_offset_by_lines(int offset, int delta, int total_rows, int visible_rows);
int ensure_row_range_visible(int offset,
                             int visible_rows,
                             int total_rows,
                             int row_begin,
                             int row_end);
int scroll_offset_for_track_y(int mouse_y,
                              int track_y_min,
                              int track_height,
                              int total_rows,
                              int visible_rows);

// 鼠标点击选项行支持(add-tui-ask-overlay-mouse-select):
// 给定渲染帧按可见顺序 reflect 出的每行屏幕 box(row_boxes,长度 =
// 可见行数)、layout row → option_index 的映射(row_option_indices,
// 非选项行为 -1)与当前滚动偏移 scroll_offset,把 (x, y) 命中的行
// 映射回选项下标;未命中任何行返回 -1。
int ask_overlay_hit_option(const std::vector<ftxui::Box>& row_boxes,
                           int scroll_offset,
                           const std::vector<int>& row_option_indices,
                           int x,
                           int y);

} // namespace acecode::tui
