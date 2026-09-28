#pragma once
#include "tui/input/tui_input_context.hpp"

namespace acecode::tui {
InputDisposition handle_ctrl_c(TuiInputContext& context, const ftxui::Event& event);
InputDisposition handle_escape(TuiInputContext& context, const ftxui::Event& event);
InputDisposition handle_tab(TuiInputContext& context, const ftxui::Event& event);
InputDisposition handle_shift_tab(TuiInputContext& context, const ftxui::Event& event);
}
