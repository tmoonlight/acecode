#pragma once
#include "tui/input/tui_input_context.hpp"

namespace acecode::tui {
InputDisposition handle_clipboard_ctrl_v(TuiInputContext& ctx, const ftxui::Event& event);
InputDisposition handle_clipboard_alt_v(TuiInputContext& ctx, const ftxui::Event& event);
InputDisposition handle_clipboard_right_click(TuiInputContext& ctx, const ftxui::Event& event);
}
