#pragma once
#include "tui/input/tui_input_context.hpp"

namespace acecode::tui {
InputDisposition handle_composer_submit(TuiInputContext& ctx, const ftxui::Event& event);
}
