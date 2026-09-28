#pragma once

#include "ask_question_layout.hpp"

#include <ftxui/screen/box.hpp>

#include <chrono>
#include <vector>

namespace acecode::tui {

// Frame-local bridge between the pure question layout and FTXUI's reflected
// boxes. The renderer owns the frame for one TUI loop and the event adapter
// consumes the same geometry; question business state remains in the session.
struct AskQuestionFrame {
    ftxui::Box overlay_box;
    ftxui::Box scrollbar_box;
    std::vector<ftxui::Box> row_boxes;
    AskQuestionLayout layout;
    bool terminal_too_narrow = false;

    bool scrollbar_dragging = false;
    int scrollbar_grab_offset = 0;
    bool dragging_text = false;
    int press_x = -1;
    int press_y = -1;
    AskQuestionHit press_target;
    std::chrono::steady_clock::time_point last_click_at{};
    AskQuestionHit last_click;

    void reset_for_render();
};

bool ask_question_box_contains(const ftxui::Box& box, int x, int y);
AskQuestionHit hit_test_ask_question_frame(const AskQuestionFrame& frame,
                                           int x,
                                           int y);

} // namespace acecode::tui
