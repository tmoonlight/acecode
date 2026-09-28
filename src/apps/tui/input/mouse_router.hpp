#pragma once
#include "tui/input/tui_input_context.hpp"

namespace acecode::tui {
InputDisposition handle_mouse(TuiInputContext& context, const ftxui::Event& event);
}
