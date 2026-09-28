#pragma once
#include "tui/input/tui_input_context.hpp"

namespace acecode::tui {
InputDisposition handle_pending_attachment_input(TuiInputContext& ctx, const ftxui::Event& event);
}
