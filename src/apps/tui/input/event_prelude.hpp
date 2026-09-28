#pragma once
#include "tui/input/tui_input_context.hpp"

namespace acecode::tui {
InputDisposition handle_event_prelude(TuiInputContext& context, const ftxui::Event& event);
}
