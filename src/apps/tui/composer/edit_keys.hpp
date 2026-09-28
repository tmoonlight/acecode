#pragma once
#include "tui/input/tui_input_context.hpp"

namespace acecode::tui {
InputDisposition handle_composer_shift_arrow(TuiInputContext& ctx, const ftxui::Event& event);
InputDisposition handle_composer_up(TuiInputContext& ctx, const ftxui::Event& event);
InputDisposition handle_composer_down(TuiInputContext& ctx, const ftxui::Event& event);
InputDisposition handle_composer_left(TuiInputContext& ctx, const ftxui::Event& event);
InputDisposition handle_composer_right(TuiInputContext& ctx, const ftxui::Event& event);
InputDisposition handle_composer_ctrl_a(TuiInputContext& ctx, const ftxui::Event& event);
InputDisposition handle_composer_home(TuiInputContext& ctx, const ftxui::Event& event);
InputDisposition handle_composer_end(TuiInputContext& ctx, const ftxui::Event& event);
InputDisposition handle_composer_delete(TuiInputContext& ctx, const ftxui::Event& event);
InputDisposition handle_composer_backspace(TuiInputContext& ctx, const ftxui::Event& event);
InputDisposition handle_composer_character(TuiInputContext& ctx, const ftxui::Event& event);
}
